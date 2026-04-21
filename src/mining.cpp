#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>
#include <nvs_flash.h>
#include <nvs.h>
//#include "ShaTests/nerdSHA256.h"
#include "ShaTests/nerdSHA256plus.h"
#include "stratum.h"
#include "mining.h"
#include "utils.h"
#include "monitor.h"
#include "timeconst.h"
#include "drivers/displays/display.h"
#include "drivers/storage/storage.h"
#include "drivers/storage/nvMemory.h"
#include <mutex>
#include <list>
#include <map>
#include <ctype.h>
#include <time.h>
#include "mbedtls/sha256.h"
#include "i2c_master.h"
#include "esp_mac.h"

#if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32H2)
#define HCASH_DEVICE_AUTH_SUPPORTED 1
#include "esp_hmac.h"
#include "esp_efuse.h"
#else
#define HCASH_DEVICE_AUTH_SUPPORTED 0
#endif

//10 Jobs per second
#define NONCE_PER_JOB_SW 4096
#define NONCE_PER_JOB_HW 16*1024

//#define I2C_SLAVE

//#define SHA256_VALIDATE
//#define RANDOM_NONCE
#define RANDOM_NONCE_MASK 0xFFFFC000

#ifdef HARDWARE_SHA265
#include <sha/sha_dma.h>
#include <hal/sha_hal.h>
#include <hal/sha_ll.h>

#if defined(CONFIG_IDF_TARGET_ESP32)
#include <sha/sha_parallel_engine.h>
#endif

#endif

nvs_handle_t stat_handle;

uint32_t templates = 0;
uint32_t hashes = 0;
uint32_t Mhashes = 0;
uint32_t totalKHashes = 0;
uint32_t elapsedKHs = 0;
uint64_t upTime = 0;

volatile uint32_t shares; // increase if blockhash has 32 bits of zeroes
volatile uint32_t valids; // accepted shares that also met network target before pool-side signet signing
volatile uint32_t acceptedShares; // accepted by pool (Stratum success)

// Track best diff
double best_diff = 0.0;

// Variables to hold data from custom textboxes
//Track mining stats in non volatile memory
extern TSettings Settings;
extern nvMemory nvMem;

IPAddress serverIP(1, 1, 1, 1); //Temporally save poolIPaddres

//Global work data 
static WiFiClient client;
static miner_data mMiner; //Global miner data (Create a miner class TODO)
mining_subscribe mWorker;
mining_job mJob;
monitor_data mMonitor;
static bool volatile isMinerSuscribed = false;
unsigned long mLastTXtoPool = millis();

#if HCASH_DEVICE_AUTH_SUPPORTED
#ifndef DEVICE_HMAC_KEY_SLOT
#define DEVICE_HMAC_KEY_SLOT -1
#endif
#endif

int saveIntervals[7] = {5 * 60, 15 * 60, 30 * 60, 1 * 3600, 3 * 3600, 6 * 3600, 12 * 3600};
int saveIntervalsSize = sizeof(saveIntervals)/sizeof(saveIntervals[0]);
int currentIntervalIndex = 0;

static void normalizeWalletLowercase(const char* input, char* output, size_t outSize)
{
  if (output == nullptr || outSize == 0) {
    return;
  }
  output[0] = '\0';
  if (input == nullptr) {
    return;
  }
  size_t i = 0;
  while (input[i] != '\0' && i + 1 < outSize) {
    output[i] = (char)tolower((unsigned char)input[i]);
    ++i;
  }
  output[i] = '\0';
}

static bool getDeviceIdFromEfuse(char* output, size_t outSize)
{
  if (output == nullptr || outSize < 13) {
    return false;
  }
  uint8_t mac[6] = {0};
  if (esp_efuse_mac_get_default(mac) != ESP_OK) {
    return false;
  }
  snprintf(
      output,
      outSize,
      "%02x%02x%02x%02x%02x%02x",
      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
  );
  return true;
}

static bool bytesToLowerHex(const uint8_t* bytes, size_t byteCount, char* outHex, size_t outHexSize)
{
  if (bytes == nullptr || outHex == nullptr || outHexSize < ((byteCount * 2) + 1)) {
    return false;
  }
  static const char* kHex = "0123456789abcdef";
  for (size_t i = 0; i < byteCount; ++i) {
    outHex[(i * 2)] = kHex[(bytes[i] >> 4) & 0x0F];
    outHex[(i * 2) + 1] = kHex[bytes[i] & 0x0F];
  }
  outHex[byteCount * 2] = '\0';
  return true;
}

#if HCASH_DEVICE_AUTH_SUPPORTED
static bool resolveDeviceHmacKeyId(hmac_key_id_t* keyIdOut)
{
  if (keyIdOut == nullptr) {
    return false;
  }

#if DEVICE_HMAC_KEY_SLOT >= 0
  const int configuredSlot = (int)DEVICE_HMAC_KEY_SLOT;
  if (configuredSlot < 0 || configuredSlot >= (int)HMAC_KEY_MAX) {
    Serial.printf("Configured DEVICE_HMAC_KEY_SLOT is out of range: %d\n", configuredSlot);
    return false;
  }
  esp_efuse_block_t configuredBlock = (esp_efuse_block_t)((int)EFUSE_BLK_KEY0 + configuredSlot);
  esp_efuse_purpose_t configuredPurpose = esp_efuse_get_key_purpose(configuredBlock);
  if (configuredPurpose != ESP_EFUSE_KEY_PURPOSE_HMAC_UP) {
    Serial.printf(
        "Configured DEVICE_HMAC_KEY_SLOT=%d has wrong purpose=%d (expected HMAC_UP=%d)\n",
        configuredSlot, (int)configuredPurpose, (int)ESP_EFUSE_KEY_PURPOSE_HMAC_UP
    );
    return false;
  }
  *keyIdOut = (hmac_key_id_t)configuredSlot;
  return true;
#endif

  esp_efuse_block_t keyBlock = EFUSE_BLK_KEY_MAX;
  if (!esp_efuse_find_purpose(ESP_EFUSE_KEY_PURPOSE_HMAC_UP, &keyBlock)) {
    Serial.println("Device HMAC key not provisioned: no eFuse key block with HMAC_UP purpose");
    return false;
  }

  const int keySlot = (int)keyBlock - (int)EFUSE_BLK_KEY0;
  if (keySlot < 0 || keySlot >= (int)HMAC_KEY_MAX) {
    Serial.printf("Device HMAC key purpose found in invalid eFuse block: %d\n", (int)keyBlock);
    return false;
  }

  *keyIdOut = (hmac_key_id_t)keySlot;
  return true;
}
#endif

static bool buildDeviceProof(const char* wallet, const device_challenge& challenge, char* proofOut, size_t proofOutSize)
{
  if (wallet == nullptr || proofOut == nullptr || proofOutSize < 65) {
    return false;
  }

#if HCASH_DEVICE_AUTH_SUPPORTED
  char normalizedWallet[80] = {0};
  normalizeWalletLowercase(wallet, normalizedWallet, sizeof(normalizedWallet));

  String payload = String(challenge.challenge_id);
  payload += ":";
  payload += challenge.nonce;
  payload += ":";
  char expiresAtBuffer[24] = {0};
  snprintf(expiresAtBuffer, sizeof(expiresAtBuffer), "%llu", (unsigned long long)challenge.expires_at);
  payload += expiresAtBuffer;
  payload += ":";
  payload += normalizedWallet;

  hmac_key_id_t keyId = HMAC_KEY0;
  if (!resolveDeviceHmacKeyId(&keyId)) {
    Serial.println("Device proof generation failed: HMAC_UP eFuse key missing or invalid");
    return false;
  }

  uint8_t hmacDigest[32] = {0};
  esp_err_t err = esp_hmac_calculate(keyId, payload.c_str(), payload.length(), hmacDigest);
  if (err != ESP_OK) {
    Serial.printf(
        "Device proof generation failed, esp_hmac_calculate err=%d (%s), key_slot=%d\n",
        (int)err, esp_err_to_name(err), (int)keyId
    );
    return false;
  }
  return bytesToLowerHex(hmacDigest, sizeof(hmacDigest), proofOut, proofOutSize);
#else
  Serial.println("Device proof generation is unsupported on this target");
  return false;
#endif
}


static bool isActivationStateReady()
{
  String state = String(Settings.ActivationState);
  state.trim();
  state.toLowerCase();
  return state == "claimed" || state == "bypass_auto";
}

static void setActivationState(const char* state)
{
  if (state == nullptr) {
    return;
  }
  strncpy(Settings.ActivationState, state, sizeof(Settings.ActivationState));
  Settings.ActivationState[sizeof(Settings.ActivationState) - 1] = '\0';
}

static bool setActivationCode(const char* code, uint64_t expiresAt)
{
  if (code == nullptr) {
    return false;
  }
  if (strncmp(Settings.ActivationCode, code, sizeof(Settings.ActivationCode)) == 0 &&
      Settings.ActivationCodeExpiresAt == expiresAt) {
    return false;
  }
  strncpy(Settings.ActivationCode, code, sizeof(Settings.ActivationCode));
  Settings.ActivationCode[sizeof(Settings.ActivationCode) - 1] = '\0';
  Settings.ActivationCodeExpiresAt = expiresAt;
  return true;
}

static String normalizeActivationApiBase()
{
  String base = String(Settings.PoolApiBase);
  base.trim();
  if (base.length() == 0)
  {
    base = "http://" + Settings.PoolAddress + ":3334";
  }

  int apiClientIdx = base.indexOf("/api/client/");
  if (apiClientIdx > 0)
  {
    base = base.substring(0, apiClientIdx);
  }
  else
  {
    apiClientIdx = base.indexOf("/api/client");
    if (apiClientIdx > 0)
    {
      base = base.substring(0, apiClientIdx);
    }
  }

  while (base.endsWith("/")) {
    base.remove(base.length() - 1);
  }

  if (!base.endsWith("/api/activation")) {
    base += "/api/activation";
  }

  return base;
}

static bool readActivationStatus(const JsonVariantConst& root, const char* fallbackState)
{
  const char* status = root["status"] | fallbackState;
  if (status == nullptr) {
    return false;
  }

  String state = String(status);
  state.toLowerCase();
  if (state == "claimed" || state == "bypass_auto") {
    setActivationState(status);
    return true;
  }

  setActivationState("unclaimed");
  return false;
}

static bool fetchActivationCodeFromServer(const char* deviceId, const char* payoutWallet)
{
  HTTPClient http;
  String endpoint = normalizeActivationApiBase() + "/code";
  http.setTimeout(10000);

  StaticJsonDocument<320> requestDoc;
  requestDoc["deviceId"] = deviceId;
  requestDoc["payoutWalletHcash"] = payoutWallet;

  String body;
  serializeJson(requestDoc, body);

  if (!http.begin(endpoint)) {
    Serial.println("Activation code request: failed to begin HTTP");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  int httpCode = http.POST(body);
  if (httpCode != HTTP_CODE_OK && httpCode != HTTP_CODE_CREATED)
  {
    Serial.printf("Activation code request failed: HTTP %d\n", httpCode);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  StaticJsonDocument<1024> responseDoc;
  DeserializationError error = deserializeJson(responseDoc, payload);
  if (error)
  {
    Serial.println("Activation code response JSON parse failed");
    return false;
  }

  if (readActivationStatus(responseDoc.as<JsonVariantConst>(), "unclaimed"))
  {
    strncpy(Settings.ActivationCode, "", sizeof(Settings.ActivationCode));
    Settings.ActivationCodeExpiresAt = 0;
    Settings.ActivationLastCheckAt = millis();
    nvMem.saveConfig(&Settings);
    return true;
  }

  const char* activationCode = responseDoc["activationCode"] | "";
  uint64_t expiresAt = responseDoc["expiresAt"] | 0;
  bool changed = setActivationCode(activationCode, expiresAt);
  if (changed) {
    nvMem.saveConfig(&Settings);
  }

  String activationUrl = String(responseDoc["activationUrl"] | "");
  Serial.println("Device activation required before mining can start");
  if (strlen(Settings.ActivationCode) > 0) {
    Serial.printf("Activation code: %s\n", Settings.ActivationCode);
  }
  if (activationUrl.length() > 0) {
    Serial.printf("Activate at: %s\n", activationUrl.c_str());
  }
  return false;
}

static bool pollActivationStatus(const char* deviceId)
{
  HTTPClient http;
  String endpoint = normalizeActivationApiBase() + "/status/" + String(deviceId);
  http.setTimeout(10000);

  if (!http.begin(endpoint)) {
    Serial.println("Activation status poll: failed to begin HTTP");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK)
  {
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  StaticJsonDocument<640> responseDoc;
  DeserializationError error = deserializeJson(responseDoc, payload);
  if (error)
  {
    return false;
  }

  bool ready = readActivationStatus(responseDoc.as<JsonVariantConst>(), Settings.ActivationState);
  if (ready)
  {
    strncpy(Settings.ActivationCode, "", sizeof(Settings.ActivationCode));
    Settings.ActivationCodeExpiresAt = 0;
    Settings.ActivationLastCheckAt = millis();
    nvMem.saveConfig(&Settings);
  }
  return ready;
}

static bool ensureDeviceActivationReady()
{
  if (Settings.PayoutWalletHcash[0] == '\0') {
    strncpy(Settings.PayoutWalletHcash, Settings.BtcWallet, sizeof(Settings.PayoutWalletHcash));
    Settings.PayoutWalletHcash[sizeof(Settings.PayoutWalletHcash) - 1] = '\0';
  }

  char deviceId[16] = {0};
  if (!getDeviceIdFromEfuse(deviceId, sizeof(deviceId))) {
    Serial.println("Activation gate: failed to read device ID");
    return false;
  }

  const uint64_t nowTick = millis();
  bool hasValidCode = Settings.ActivationCode[0] != '\0' &&
                      Settings.ActivationCodeExpiresAt > 0 &&
                      (Settings.ActivationCodeExpiresAt > (uint64_t)time(nullptr) * 1000ULL + 60000ULL);

  if (!hasValidCode) {
    if (fetchActivationCodeFromServer(deviceId, Settings.PayoutWalletHcash)) {
      return true;
    }
  }

  if ((nowTick - Settings.ActivationLastCheckAt) > 5000ULL)
  {
    Settings.ActivationLastCheckAt = nowTick;
    if (pollActivationStatus(deviceId)) {
      return true;
    }
  }

  mMonitor.NerdStatus = NM_waitingConfig;
  if (Settings.ActivationCode[0] != '\0') {
    Serial.printf("Waiting for activation claim. Code: %s\n", Settings.ActivationCode);
  } else {
    Serial.println("Waiting for activation claim...");
  }
  return false;
}

static bool authenticateDeviceWithPool(WiFiClient& poolClient, const char* wallet)
{
  if (wallet == nullptr || wallet[0] == '\0') {
    Serial.println("Wallet is empty; cannot run device authentication");
    return false;
  }

  char deviceId[16] = {0};
  if (!getDeviceIdFromEfuse(deviceId, sizeof(deviceId))) {
    Serial.println("Failed to read eFuse base MAC for device_id");
    return false;
  }

  device_challenge challenge;
  challenge.challenge_id = "";
  challenge.nonce = "";
  challenge.expires_at = 0;
  if (!tx_mining_device_challenge(poolClient, deviceId, wallet, challenge)) {
    Serial.println("Pool device challenge request failed");
    return false;
  }

  char proofHex[65] = {0};
  if (!buildDeviceProof(wallet, challenge, proofHex, sizeof(proofHex))) {
    Serial.println("Device proof build failed");
    return false;
  }

  if (!tx_mining_device_auth(poolClient, deviceId, wallet, challenge.challenge_id.c_str(), proofHex)) {
    Serial.println("Pool device auth request failed");
    return false;
  }

  return true;
}

bool checkPoolConnection(void) {
  
  if (client.connected()) {
    return true;
  }
  
  isMinerSuscribed = false;

  Serial.println("Client not connected, trying to connect..."); 
  
  //Resolve first time pool DNS and save IP
  if(serverIP == IPAddress(1,1,1,1)) {
    WiFi.hostByName(Settings.PoolAddress.c_str(), serverIP);
    Serial.printf("Resolved DNS and save ip (first time) got: %s\n", serverIP.toString());
  }

  //Try connecting pool IP
  if (!client.connect(serverIP, Settings.PoolPort)) {
    Serial.println("Imposible to connect to : " + Settings.PoolAddress);
    WiFi.hostByName(Settings.PoolAddress.c_str(), serverIP);
    Serial.printf("Resolved DNS got: %s\n", serverIP.toString());
    return false;
  }

  return true;
}

//Implements a socketKeepAlive function and 
//checks if pool is not sending any data to reconnect again.
//Even connection could be alive, pool could stop sending new job NOTIFY
unsigned long mStart0Hashrate = 0;
bool checkPoolInactivity(unsigned int keepAliveTime, unsigned long inactivityTime, double suggestedDifficulty){ 

    unsigned long currentKHashes = (Mhashes*1000) + hashes/1000;
    unsigned long elapsedKHs = currentKHashes - totalKHashes;

    uint32_t time_now = millis();

    // If no shares sent to pool
    // send something to pool to hold socket oppened
    if (time_now < mLastTXtoPool) //32bit wrap
      mLastTXtoPool = time_now;
    if ( time_now > mLastTXtoPool + keepAliveTime)
    {
      mLastTXtoPool = time_now;
      Serial.println("  Sending  : KeepAlive suggest_difficulty");
      //if (client.print("{}\n") == 0) {
      tx_suggest_difficulty(client, suggestedDifficulty);
      /*if(tx_suggest_difficulty(client, DEFAULT_DIFFICULTY)){
        Serial.println("  Sending keepAlive to pool -> Detected client disconnected");
        return true;
      }*/
    }

    if(elapsedKHs == 0){
      //Check if hashrate is 0 during inactivityTIme
      if(mStart0Hashrate == 0) mStart0Hashrate  = time_now; 
      if((time_now-mStart0Hashrate) > inactivityTime) { mStart0Hashrate=0; return true;}
      return false;
    }

  mStart0Hashrate = 0;
  return false;
}

struct JobRequest
{
  uint32_t id;
  uint32_t nonce_start;
  uint32_t nonce_count;
  double difficulty;
  uint8_t sha_buffer[128];
  uint32_t midstate[8];
  uint32_t bake[16];
};

struct JobResult
{
  uint32_t id;
  uint32_t nonce;
  uint32_t nonce_count;
  double difficulty;
  uint8_t hash[32];
};

static std::mutex s_job_mutex;
std::list<std::shared_ptr<JobRequest>> s_job_request_list_sw;
#ifdef HARDWARE_SHA265
std::list<std::shared_ptr<JobRequest>> s_job_request_list_hw;
#endif
std::list<std::shared_ptr<JobResult>> s_job_result_list;
static volatile uint8_t s_working_current_job_id = 0xFF;

static void JobPush(std::list<std::shared_ptr<JobRequest>> &job_list,  uint32_t id, uint32_t nonce_start, uint32_t nonce_count, double difficulty,
                    const uint8_t* sha_buffer, const uint32_t* midstate, const uint32_t* bake)
{
  std::shared_ptr<JobRequest> job = std::make_shared<JobRequest>();
  job->id = id;
  job->nonce_start = nonce_start;
  job->nonce_count = nonce_count;
  job->difficulty = difficulty;
  memcpy(job->sha_buffer, sha_buffer, sizeof(job->sha_buffer));
  memcpy(job->midstate, midstate, sizeof(job->midstate));
  memcpy(job->bake, bake, sizeof(job->bake));
  job_list.push_back(job);
}

struct Submition
{
  double diff;
  bool is32bit;
  bool isValid;
};

static void MiningJobStop(uint32_t &job_pool, std::map<uint32_t, std::shared_ptr<Submition>> & submition_map)
{
  {
    std::lock_guard<std::mutex> lock(s_job_mutex);
    s_job_result_list.clear();
    s_job_request_list_sw.clear();
    #ifdef HARDWARE_SHA265
    s_job_request_list_hw.clear();
    #endif
  }
  s_working_current_job_id = 0xFF;
  job_pool = 0xFFFFFFFF;
  submition_map.clear();
}

#ifdef RANDOM_NONCE
uint64_t s_random_state = 1;
static uint32_t RandomGet()
{
    s_random_state += 0x9E3779B97F4A7C15ull;
    uint64_t z = s_random_state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

#endif

void runStratumWorker(void *name) {

// TEST: https://bitcoin.stackexchange.com/questions/22929/full-example-data-for-scrypt-stratum-client

  Serial.println("");
  Serial.printf("\n[WORKER] Started. Running %s on core %d\n", (char *)name, xPortGetCoreID());

#ifdef DEBUG_VALID_BLOCK_CHECK
  checkValidSelfTest();
#endif

  #ifdef DEBUG_MEMORY
  Serial.printf("### [Total Heap / Free heap / Min free heap]: %d / %d / %d \n", ESP.getHeapSize(), ESP.getFreeHeap(), ESP.getMinFreeHeap());
  #endif

  std::map<uint32_t, std::shared_ptr<Submition>> s_submition_map;

#ifdef I2C_SLAVE
  std::vector<uint8_t> i2c_slave_vector;

  //scan for i2c slaves
  if (i2c_master_start() == 0)
    i2c_slave_vector = i2c_master_scan(0x0, 0x80);
  Serial.printf("Found %d slave workers\n", i2c_slave_vector.size());
  if (!i2c_slave_vector.empty())
  {
    Serial.print("  Workers: ");
    for (size_t n = 0; n < i2c_slave_vector.size(); ++n)
      Serial.printf("0x%02X,", (uint32_t)i2c_slave_vector[n]);
    Serial.println("");
  }
#endif

  // connect to pool  
  double currentPoolDifficulty = DEFAULT_DIFFICULTY;
  uint32_t nonce_pool = 0;
  uint32_t job_pool = 0xFFFFFFFF;
  uint32_t last_job_time = millis();

  while(true) {
      
    if(WiFi.status() != WL_CONNECTED){
      // WiFi is disconnected, so reconnect now
      mMonitor.NerdStatus = NM_Connecting;
      MiningJobStop(job_pool, s_submition_map);
      WiFi.reconnect();
      vTaskDelay(5000 / portTICK_PERIOD_MS);
      continue;
    } 

    if (!ensureDeviceActivationReady()) {
      MiningJobStop(job_pool, s_submition_map);
      vTaskDelay(3000 / portTICK_PERIOD_MS);
      continue;
    }

    if(!checkPoolConnection()){
      //If server is not reachable add random delay for connection retries
      //Generate value between 1 and 60 secs
      MiningJobStop(job_pool, s_submition_map);
      vTaskDelay(((1 + rand() % 60) * 1000) / portTICK_PERIOD_MS);
      continue;
    }

    if(!isMinerSuscribed)
    {
      //Stop miner current jobs
      mWorker = init_mining_subscribe();

      // STEP 1: Pool server connection (SUBSCRIBE)
      if(!tx_mining_subscribe(client, mWorker)) { 
        client.stop();
        MiningJobStop(job_pool, s_submition_map);
        continue; 
      }
      
      strcpy(mWorker.wName, Settings.PayoutWalletHcash);
      strcpy(mWorker.wPass, Settings.PoolPassword);
      // STEP 2: Pool authorize work (Block Info)
      if (!tx_mining_auth(client, mWorker.wName, mWorker.wPass)) {
        client.stop();
        MiningJobStop(job_pool, s_submition_map);
        continue;
      }
      // STEP 2.1: Device challenge-response auth before mining activation
      if (!authenticateDeviceWithPool(client, mWorker.wName)) {
        client.stop();
        MiningJobStop(job_pool, s_submition_map);
        continue;
      }

      // STEP 3: Suggest pool difficulty
      tx_suggest_difficulty(client, currentPoolDifficulty);

      isMinerSuscribed=true;
      uint32_t time_now = millis();
      mLastTXtoPool = time_now;
      last_job_time = time_now;
    }

    //Check if pool is down for almost 5minutes and then restart connection with pool (1min=600000ms)
    if(checkPoolInactivity(KEEPALIVE_TIME_ms, POOLINACTIVITY_TIME_ms, currentPoolDifficulty)){
      //Restart connection
      Serial.println("  Detected more than 2 min without data form stratum server. Closing socket and reopening...");
      client.stop();
      isMinerSuscribed=false;
      MiningJobStop(job_pool, s_submition_map);
      continue; 
    }

    {
      uint32_t time_now = millis();
      if (time_now < last_job_time) //32bit wrap
        last_job_time = time_now;
      if (time_now >= last_job_time + 10*60*1000)  //10minutes without job
      {
        client.stop();
        isMinerSuscribed=false;
        MiningJobStop(job_pool, s_submition_map);
        continue;
      }
    }

    uint32_t hw_midstate[8];
    uint32_t diget_mid[8];
    uint32_t bake[16];
    #if defined(CONFIG_IDF_TARGET_ESP32)
    uint8_t sha_buffer_swap[128];
    #endif

    //Read pending messages from pool
    while(client.connected() && client.available())
    {
      String line = client.readStringUntil('\n');
      //Serial.println("  Received message from pool");      
      stratum_method result = parse_mining_method(line);
      switch (result)
      {
          case MINING_NOTIFY:         if(parse_mining_notify(line, mJob))
                                      {
                                          {
                                            std::lock_guard<std::mutex> lock(s_job_mutex);
                                            s_job_request_list_sw.clear();
                                            #ifdef HARDWARE_SHA265
                                            s_job_request_list_hw.clear();
                                            #endif
                                          }
                                          //Increse templates readed
                                          templates++;
                                          job_pool++;
                                          s_working_current_job_id = job_pool & 0xFF; //Terminate current job in thread

                                          last_job_time = millis();
                                          mLastTXtoPool = last_job_time;

                                          uint32_t mh = hashes/1000000;
                                          Mhashes += mh;
                                          hashes -= mh*1000000;

                                          //Prepare data for new jobs
                                          mMiner=calculateMiningData(mWorker, mJob);

                                          memset(mMiner.bytearray_blockheader+80, 0, 128-80);
                                          mMiner.bytearray_blockheader[80] = 0x80;
                                          mMiner.bytearray_blockheader[126] = 0x02;
                                          mMiner.bytearray_blockheader[127] = 0x80;

                                          nerd_mids(diget_mid, mMiner.bytearray_blockheader);
                                          nerd_sha256_bake(diget_mid, mMiner.bytearray_blockheader+64, bake);

                                          #ifdef HARDWARE_SHA265
                                          #if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)
                                            esp_sha_acquire_hardware();
                                            sha_hal_hash_block(SHA2_256,  mMiner.bytearray_blockheader, 64/4, true);
                                            sha_hal_read_digest(SHA2_256, hw_midstate);
                                            esp_sha_release_hardware();
                                          #endif
                                          #endif

                                          #if defined(CONFIG_IDF_TARGET_ESP32)
                                          for (int i = 0; i < 32; ++i)
                                            ((uint32_t*)sha_buffer_swap)[i] = __builtin_bswap32(((const uint32_t*)(mMiner.bytearray_blockheader))[i]);
                                          #endif

                                          #ifdef RANDOM_NONCE
                                          nonce_pool = RandomGet() & RANDOM_NONCE_MASK;
                                          #else
                                            #ifdef I2C_SLAVE
                                            if (!i2c_slave_vector.empty())
                                              nonce_pool = 0x10000000;
                                            else
                                            #endif
                                              nonce_pool = 0xDA54E700;  //nonce 0x00000000 is not possible, start from some random nonce
                                          #endif
                                          

                                          {
                                            std::lock_guard<std::mutex> lock(s_job_mutex);
                                            for (int i = 0; i < 4; ++ i)
                                            {
                                              #if 1
                                              JobPush( s_job_request_list_sw, job_pool, nonce_pool, NONCE_PER_JOB_SW, currentPoolDifficulty, mMiner.bytearray_blockheader, diget_mid, bake);
                                              #ifdef RANDOM_NONCE
                                              nonce_pool = RandomGet() & RANDOM_NONCE_MASK;
                                              #else
                                              nonce_pool += NONCE_PER_JOB_SW;
                                              #endif
                                              #endif
                                              #ifdef HARDWARE_SHA265
                                                #if defined(CONFIG_IDF_TARGET_ESP32)
                                                  JobPush( s_job_request_list_hw, job_pool, nonce_pool, NONCE_PER_JOB_HW, currentPoolDifficulty, sha_buffer_swap, hw_midstate, bake);
                                                #else
                                                  JobPush( s_job_request_list_hw, job_pool, nonce_pool, NONCE_PER_JOB_HW, currentPoolDifficulty, mMiner.bytearray_blockheader, hw_midstate, bake);
                                                #endif
                                              #ifdef RANDOM_NONCE
                                              nonce_pool = RandomGet() & RANDOM_NONCE_MASK;
                                              #else
                                              nonce_pool += NONCE_PER_JOB_HW;
                                              #endif
                                              #endif
                                            }
                                          }
                                          #ifdef I2C_SLAVE
                                          //Nonce for nonce_pool starts from 0x10000000
                                          //For i2c slave we give nonces from 0x20000000, that is 0x10000000 nonces per slave
                                          i2c_feed_slaves(i2c_slave_vector, job_pool & 0xFF, 0x20, currentPoolDifficulty, mMiner.bytearray_blockheader);
                                          #endif
                                      } else
                                      {
                                        Serial.println("Parsing error, need restart");
                                        client.stop();
                                        isMinerSuscribed=false;
                                        MiningJobStop(job_pool, s_submition_map);
                                      }
                                      break;
          case MINING_SET_DIFFICULTY: parse_mining_set_difficulty(line, currentPoolDifficulty);
                                      break;
          case STRATUM_SUCCESS:       {
                                        unsigned long id = parse_extract_id(line);
                                        auto itt = s_submition_map.find(id);
                                        if (itt != s_submition_map.end())
                                        {
                                          if (itt->second->diff > best_diff)
                                            best_diff = itt->second->diff;
                                          acceptedShares++;
                                          if (itt->second->is32bit)
                                            shares++;
                                          if (itt->second->isValid)
                                          {
                                            Serial.println("Share accepted: met network target pre-signet (pool will decide final block acceptance)");
                                            valids++;
                                          }
                                          s_submition_map.erase(itt);
                                        }
                                      }
                                      break;
          case STRATUM_PARSE_ERROR:   {
                                        unsigned long id = parse_extract_id(line);
                                        auto itt = s_submition_map.find(id);
                                        if (itt != s_submition_map.end())
                                        {
                                          Serial.printf("Share rejected by pool for submit id %lu\n", id);
                                          s_submition_map.erase(itt);
                                        }
                                      }
                                      break;
          default:                    Serial.println("  Parsed JSON: unknown"); break;

      }
    }

    std::list<std::shared_ptr<JobResult>> job_result_list;
    #ifdef I2C_SLAVE
    if (i2c_slave_vector.empty() || job_pool == 0xFFFFFFFF)
    {
      vTaskDelay(50 / portTICK_PERIOD_MS); //Small delay
    } else
    {
      uint32_t time_start = millis();
      i2c_hit_slaves(i2c_slave_vector);
      vTaskDelay(5 / portTICK_PERIOD_MS);
      uint32_t nonces_done = 0;
      std::vector<uint32_t> nonce_vector = i2c_harvest_slaves(i2c_slave_vector, job_pool & 0xFF, nonces_done);
      hashes += nonces_done;
      for (size_t n = 0; n < nonce_vector.size(); ++n)
      {
        std::shared_ptr<JobResult> result = std::make_shared<JobResult>();
        ((uint32_t*)(mMiner.bytearray_blockheader+64+12))[0] = nonce_vector[n];
        if (nerd_sha256d_baked(diget_mid, mMiner.bytearray_blockheader+64, bake, result->hash))
        {
          result->id = job_pool;
          result->nonce = nonce_vector[n];
          result->nonce_count = 0;
          result->difficulty = diff_from_target(result->hash);
          job_result_list.push_back(result);
        }
      }
      uint32_t time_end = millis();
      //if (nonces_done > 16384)
        //Serial.printf("Harvest slaves in %dms hashes=%d\n", time_end - time_start, nonces_done);
      if (time_end > time_start)
      {
        uint32_t elapsed = time_end - time_start;
        if (elapsed < 50)
          vTaskDelay((50 - elapsed) / portTICK_PERIOD_MS);
      } else
        vTaskDelay(40 / portTICK_PERIOD_MS);
    }
    #else
    vTaskDelay(50 / portTICK_PERIOD_MS); //Small delay
    #endif

    
    if (job_pool != 0xFFFFFFFF)
    {
      std::lock_guard<std::mutex> lock(s_job_mutex);
      job_result_list.insert(job_result_list.end(), s_job_result_list.begin(), s_job_result_list.end());
      s_job_result_list.clear();

#if 1
      while (s_job_request_list_sw.size() < 4)
      {
        JobPush( s_job_request_list_sw, job_pool, nonce_pool, NONCE_PER_JOB_SW, currentPoolDifficulty, mMiner.bytearray_blockheader, diget_mid, bake);
        #ifdef RANDOM_NONCE
        nonce_pool = RandomGet() & RANDOM_NONCE_MASK;
        #else
        nonce_pool += NONCE_PER_JOB_SW;
        #endif
      }
#endif

      #ifdef HARDWARE_SHA265
      while (s_job_request_list_hw.size() < 4)
      {
        #if defined(CONFIG_IDF_TARGET_ESP32)
          JobPush( s_job_request_list_hw, job_pool, nonce_pool, NONCE_PER_JOB_HW, currentPoolDifficulty, sha_buffer_swap, hw_midstate, bake);
        #else
          JobPush( s_job_request_list_hw, job_pool, nonce_pool, NONCE_PER_JOB_HW, currentPoolDifficulty, mMiner.bytearray_blockheader, hw_midstate, bake);
        #endif
        #ifdef RANDOM_NONCE
        nonce_pool = RandomGet() & RANDOM_NONCE_MASK;
        #else
        nonce_pool += NONCE_PER_JOB_HW;
        #endif
      }
      #endif
    }

    while (!job_result_list.empty())
    {
      std::shared_ptr<JobResult> res = job_result_list.front();
      job_result_list.pop_front();

      hashes += res->nonce_count;
      if (res->difficulty > currentPoolDifficulty && job_pool == res->id && res->nonce != 0xFFFFFFFF)
      {
        if (!client.connected())
          break;
        unsigned long sumbit_id = 0;
        tx_mining_submit(client, mWorker, mJob, res->nonce, sumbit_id);
        Serial.print("   - Current diff share: "); Serial.println(res->difficulty,12);
        Serial.print("   - Current pool diff : "); Serial.println(currentPoolDifficulty,12);
        Serial.print("   - TX SHARE: ");
        for (size_t i = 0; i < 32; i++)
            Serial.printf("%02x", res->hash[i]);
        Serial.println("");
        mLastTXtoPool = millis();

        std::shared_ptr<Submition> submition = std::make_shared<Submition>();
        submition->diff = res->difficulty;
        submition->is32bit = (res->hash[29] == 0 && res->hash[28] == 0);
        // Valid block candidates are determined by network target, not 32-bit share threshold.
        submition->isValid = checkValid(res->hash, mMiner.bytearray_target);

        s_submition_map.insert(std::make_pair(sumbit_id, submition));
        if (s_submition_map.size() > 32)
          s_submition_map.erase(s_submition_map.begin());
      }
    }
  }
}

//////////////////THREAD CALLS///////////////////

void minerWorkerSw(void * task_id)
{
  unsigned int miner_id = (uint32_t)task_id;
  Serial.printf("[MINER] %d Started minerWorkerSw Task!\n", miner_id);

  std::shared_ptr<JobRequest> job;
  std::shared_ptr<JobResult> result;
  uint8_t hash[32];
  uint32_t wdt_counter = 0;
  while (1)
  {
    {
      std::lock_guard<std::mutex> lock(s_job_mutex);
      if (result)
      {
        if (s_job_result_list.size() < 16)
          s_job_result_list.push_back(result);
        result.reset();
      }
      if (!s_job_request_list_sw.empty())
      {
        job = s_job_request_list_sw.front();
        s_job_request_list_sw.pop_front();
      } else
        job.reset();
    }
    if (job)
    {
      result = std::make_shared<JobResult>();
      result->difficulty = job->difficulty;
      result->nonce = 0xFFFFFFFF;
      result->id = job->id;
      result->nonce_count = job->nonce_count;
      uint8_t job_in_work = job->id & 0xFF;
      for (uint32_t n = 0; n < job->nonce_count; ++n)
      {
        ((uint32_t*)(job->sha_buffer+64+12))[0] = job->nonce_start+n;
        if (nerd_sha256d_baked(job->midstate, job->sha_buffer+64, job->bake, hash))
        {
          double diff_hash = diff_from_target(hash);
          if (diff_hash > result->difficulty)
          {
            result->difficulty = diff_hash;
            result->nonce = job->nonce_start+n;
            memcpy(result->hash, hash, 32);
          }
        }

        if ( (uint16_t)(n & 0xFF) == 0 &&s_working_current_job_id != job_in_work)
        {
          result->nonce_count = n+1;
          break;
        }
      }
    } else
      vTaskDelay(2 / portTICK_PERIOD_MS);

    wdt_counter++;
    if (wdt_counter >= 8)
    {
      wdt_counter = 0;
      esp_task_wdt_reset();
    }
  }
}

#ifdef HARDWARE_SHA265

#if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)

static inline void nerd_sha_ll_fill_text_block_sha256(const void *input_text, uint32_t nonce)
{
    uint32_t *data_words = (uint32_t *)input_text;
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

    REG_WRITE(&reg_addr_buf[0], data_words[0]);
    REG_WRITE(&reg_addr_buf[1], data_words[1]);
    REG_WRITE(&reg_addr_buf[2], data_words[2]);
#if 0
    REG_WRITE(&reg_addr_buf[3], nonce);
    //REG_WRITE(&reg_addr_buf[3], data_words[3]);    
    REG_WRITE(&reg_addr_buf[4], data_words[4]);
    REG_WRITE(&reg_addr_buf[5], data_words[5]);
    REG_WRITE(&reg_addr_buf[6], data_words[6]);
    REG_WRITE(&reg_addr_buf[7], data_words[7]);
    REG_WRITE(&reg_addr_buf[8], data_words[8]);
    REG_WRITE(&reg_addr_buf[9], data_words[9]);
    REG_WRITE(&reg_addr_buf[10], data_words[10]);
    REG_WRITE(&reg_addr_buf[11], data_words[11]);
    REG_WRITE(&reg_addr_buf[12], data_words[12]);
    REG_WRITE(&reg_addr_buf[13], data_words[13]);
    REG_WRITE(&reg_addr_buf[14], data_words[14]);
    REG_WRITE(&reg_addr_buf[15], data_words[15]);
#else
    REG_WRITE(&reg_addr_buf[3], nonce);
    REG_WRITE(&reg_addr_buf[4], 0x00000080);
    REG_WRITE(&reg_addr_buf[5], 0x00000000);
    REG_WRITE(&reg_addr_buf[6], 0x00000000);
    REG_WRITE(&reg_addr_buf[7], 0x00000000);
    REG_WRITE(&reg_addr_buf[8], 0x00000000);
    REG_WRITE(&reg_addr_buf[9], 0x00000000);
    REG_WRITE(&reg_addr_buf[10], 0x00000000);
    REG_WRITE(&reg_addr_buf[11], 0x00000000);
    REG_WRITE(&reg_addr_buf[12], 0x00000000);
    REG_WRITE(&reg_addr_buf[13], 0x00000000);
    REG_WRITE(&reg_addr_buf[14], 0x00000000);
    REG_WRITE(&reg_addr_buf[15], 0x80020000);
#endif
}

static inline void nerd_sha_ll_fill_text_block_sha256_inter()
{
  uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

  DPORT_INTERRUPT_DISABLE();
  REG_WRITE(&reg_addr_buf[0], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 0 * 4));
  REG_WRITE(&reg_addr_buf[1], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 1 * 4));
  REG_WRITE(&reg_addr_buf[2], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 2 * 4));
  REG_WRITE(&reg_addr_buf[3], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 3 * 4));
  REG_WRITE(&reg_addr_buf[4], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 4 * 4));
  REG_WRITE(&reg_addr_buf[5], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 5 * 4));
  REG_WRITE(&reg_addr_buf[6], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 6 * 4));
  REG_WRITE(&reg_addr_buf[7], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 7 * 4));
  DPORT_INTERRUPT_RESTORE();

  REG_WRITE(&reg_addr_buf[8], 0x00000080);
  REG_WRITE(&reg_addr_buf[9], 0x00000000);
  REG_WRITE(&reg_addr_buf[10], 0x00000000);
  REG_WRITE(&reg_addr_buf[11], 0x00000000);
  REG_WRITE(&reg_addr_buf[12], 0x00000000);
  REG_WRITE(&reg_addr_buf[13], 0x00000000);
  REG_WRITE(&reg_addr_buf[14], 0x00000000);
  REG_WRITE(&reg_addr_buf[15], 0x00010000);
}

static inline void nerd_sha_ll_read_digest(void* ptr)
{
  DPORT_INTERRUPT_DISABLE();
  ((uint32_t*)ptr)[0] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 0 * 4);
  ((uint32_t*)ptr)[1] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 1 * 4);
  ((uint32_t*)ptr)[2] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 2 * 4);
  ((uint32_t*)ptr)[3] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 3 * 4);
  ((uint32_t*)ptr)[4] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 4 * 4);
  ((uint32_t*)ptr)[5] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 5 * 4);
  ((uint32_t*)ptr)[6] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 6 * 4);  
  ((uint32_t*)ptr)[7] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 7 * 4);
  DPORT_INTERRUPT_RESTORE();
}


static inline bool nerd_sha_ll_read_digest_if(void* ptr)
{
  DPORT_INTERRUPT_DISABLE();
  uint32_t last = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 7 * 4);
  #if 1
  if ( (uint16_t)(last >> 16) != 0)
  {
    DPORT_INTERRUPT_RESTORE();
    return false;
  }
  #endif

  ((uint32_t*)ptr)[7] = last;
  ((uint32_t*)ptr)[0] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 0 * 4);
  ((uint32_t*)ptr)[1] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 1 * 4);
  ((uint32_t*)ptr)[2] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 2 * 4);
  ((uint32_t*)ptr)[3] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 3 * 4);
  ((uint32_t*)ptr)[4] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 4 * 4);
  ((uint32_t*)ptr)[5] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 5 * 4);
  ((uint32_t*)ptr)[6] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 6 * 4);  
  DPORT_INTERRUPT_RESTORE();
  return true;
}

static inline void nerd_sha_ll_write_digest(void *digest_state)
{
    uint32_t *digest_state_words = (uint32_t *)digest_state;
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_H_BASE);

    REG_WRITE(&reg_addr_buf[0], digest_state_words[0]);
    REG_WRITE(&reg_addr_buf[1], digest_state_words[1]);
    REG_WRITE(&reg_addr_buf[2], digest_state_words[2]);
    REG_WRITE(&reg_addr_buf[3], digest_state_words[3]);
    REG_WRITE(&reg_addr_buf[4], digest_state_words[4]);
    REG_WRITE(&reg_addr_buf[5], digest_state_words[5]);
    REG_WRITE(&reg_addr_buf[6], digest_state_words[6]);
    REG_WRITE(&reg_addr_buf[7], digest_state_words[7]);
}

static inline void nerd_sha_hal_wait_idle()
{
    while (REG_READ(SHA_BUSY_REG))
    {}
}

//#define VALIDATION
void minerWorkerHw(void * task_id)
{
  unsigned int miner_id = (uint32_t)task_id;
  Serial.printf("[MINER] %d Started minerWorkerHw Task!\n", miner_id);

  std::shared_ptr<JobRequest> job;
  std::shared_ptr<JobResult> result;
  uint8_t interResult[64];
  uint8_t hash[32];
  uint8_t digest_mid[32];
  uint8_t sha_buffer[64];
  uint32_t wdt_counter = 0;

#ifdef VALIDATION
  uint8_t doubleHash[32];
  uint32_t diget_mid[8];
  uint32_t bake[16];
#endif

  while (1)
  {
    {
      std::lock_guard<std::mutex> lock(s_job_mutex);
      if (result)
      {
        if (s_job_result_list.size() < 16)
          s_job_result_list.push_back(result);
        result.reset();
      }
      if (!s_job_request_list_hw.empty())
      {
        job = s_job_request_list_hw.front();
        s_job_request_list_hw.pop_front();
      } else
        job.reset();
    }
    if (job)
    {
      result = std::make_shared<JobResult>();
      result->id = job->id;
      result->nonce = 0xFFFFFFFF;
      result->nonce_count = job->nonce_count;
      result->difficulty = job->difficulty;
      uint8_t job_in_work = job->id & 0xFF;
      memcpy(digest_mid, job->midstate, sizeof(digest_mid));
      memcpy(sha_buffer, job->sha_buffer+64, sizeof(sha_buffer));
#ifdef VALIDATION
      nerd_mids(diget_mid, job->sha_buffer);
      nerd_sha256_bake(diget_mid, job->sha_buffer+64, bake);
#endif

      esp_sha_acquire_hardware();
      REG_WRITE(SHA_MODE_REG, SHA2_256);
      uint32_t nend = job->nonce_start + job->nonce_count;
      for (uint32_t n = job->nonce_start; n < nend; ++n)
      {
        //nerd_sha_hal_wait_idle();
        nerd_sha_ll_write_digest(digest_mid);
        //nerd_sha_hal_wait_idle();
        nerd_sha_ll_fill_text_block_sha256(sha_buffer, n);
        //sha_ll_continue_block(SHA2_256);
        REG_WRITE(SHA_CONTINUE_REG, 1);
        
        sha_ll_load(SHA2_256);
        nerd_sha_hal_wait_idle();
        nerd_sha_ll_fill_text_block_sha256_inter();
        //sha_ll_start_block(SHA2_256);
        REG_WRITE(SHA_START_REG, 1);
        sha_ll_load(SHA2_256);
        nerd_sha_hal_wait_idle();
        if (nerd_sha_ll_read_digest_if(hash))
        {
          //Serial.printf("Hw 16bit Share, nonce=0x%X\n", n);
#ifdef VALIDATION
          //Validation
          ((uint32_t*)(job->sha_buffer+64+12))[0] = n;
          nerd_sha256d_baked(diget_mid, job->sha_buffer+64, bake, doubleHash);
          for (int i = 0; i < 32; ++i)
          {
            if (hash[i] != doubleHash[i])
            {
              Serial.println("***HW sha256 esp32s3 bug detected***");
              break;
            }
          }
#endif
          //~5 per second
          double diff_hash = diff_from_target(hash);
          if (diff_hash > result->difficulty)
          {
            if (isSha256Valid(hash))
            {
              result->difficulty = diff_hash;
              result->nonce = n;
              memcpy(result->hash, hash, sizeof(hash));
            }
          }
        }
        if (
             (uint8_t)(n & 0xFF) == 0 &&
             s_working_current_job_id != job_in_work)
        {
          result->nonce_count = n-job->nonce_start+1;
          break;
        }
      }
      esp_sha_release_hardware();
    } else
      vTaskDelay(2 / portTICK_PERIOD_MS);

    wdt_counter++;
    if (wdt_counter >= 8)
    {
      wdt_counter = 0;
      esp_task_wdt_reset();
    }
  }
}

#endif  //#if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)

#if defined(CONFIG_IDF_TARGET_ESP32)

static inline bool nerd_sha_ll_read_digest_swap_if(void* ptr)
{
  DPORT_INTERRUPT_DISABLE();
  uint32_t fin = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 7 * 4);
  if ( (uint32_t)(fin & 0xFFFF) != 0)
  {
    DPORT_INTERRUPT_RESTORE();
    return false;
  }
  ((uint32_t*)ptr)[7] = __builtin_bswap32(fin);
  ((uint32_t*)ptr)[0] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 0 * 4));
  ((uint32_t*)ptr)[1] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 1 * 4));
  ((uint32_t*)ptr)[2] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 2 * 4));
  ((uint32_t*)ptr)[3] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 3 * 4));
  ((uint32_t*)ptr)[4] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 4 * 4));
  ((uint32_t*)ptr)[5] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 5 * 4));
  ((uint32_t*)ptr)[6] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 6 * 4));
  DPORT_INTERRUPT_RESTORE();
  return true;
}

static inline void nerd_sha_ll_read_digest(void* ptr)
{
  DPORT_INTERRUPT_DISABLE();
  ((uint32_t*)ptr)[0] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 0 * 4);
  ((uint32_t*)ptr)[1] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 1 * 4);
  ((uint32_t*)ptr)[2] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 2 * 4);
  ((uint32_t*)ptr)[3] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 3 * 4);
  ((uint32_t*)ptr)[4] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 4 * 4);
  ((uint32_t*)ptr)[5] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 5 * 4);
  ((uint32_t*)ptr)[6] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 6 * 4);
  ((uint32_t*)ptr)[7] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 7 * 4);
  DPORT_INTERRUPT_RESTORE();
}

static inline void nerd_sha_hal_wait_idle()
{
    while (DPORT_REG_READ(SHA_256_BUSY_REG))
    {}
}

static inline void nerd_sha_ll_fill_text_block_sha256(const void *input_text)
{
    uint32_t *data_words = (uint32_t *)input_text;
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

    reg_addr_buf[0]  = data_words[0];
    reg_addr_buf[1]  = data_words[1];
    reg_addr_buf[2]  = data_words[2];
    reg_addr_buf[3]  = data_words[3];
    reg_addr_buf[4]  = data_words[4];
    reg_addr_buf[5]  = data_words[5];
    reg_addr_buf[6]  = data_words[6];
    reg_addr_buf[7]  = data_words[7];
    reg_addr_buf[8]  = data_words[8];
    reg_addr_buf[9]  = data_words[9];
    reg_addr_buf[10] = data_words[10];
    reg_addr_buf[11] = data_words[11];
    reg_addr_buf[12] = data_words[12];
    reg_addr_buf[13] = data_words[13];
    reg_addr_buf[14] = data_words[14];
    reg_addr_buf[15] = data_words[15];
}

static inline void nerd_sha_ll_fill_text_block_sha256_upper(const void *input_text, uint32_t nonce)
{
    uint32_t *data_words = (uint32_t *)input_text;
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

    reg_addr_buf[0]  = data_words[0];
    reg_addr_buf[1]  = data_words[1];
    reg_addr_buf[2]  = data_words[2];
    reg_addr_buf[3]  = __builtin_bswap32(nonce);
#if 1
    reg_addr_buf[4]  = 0x80000000;
    reg_addr_buf[5]  = 0x00000000;
    reg_addr_buf[6]  = 0x00000000;
    reg_addr_buf[7]  = 0x00000000;
    reg_addr_buf[8]  = 0x00000000;
    reg_addr_buf[9]  = 0x00000000;
    reg_addr_buf[10] = 0x00000000;
    reg_addr_buf[11] = 0x00000000;
    reg_addr_buf[12] = 0x00000000;
    reg_addr_buf[13] = 0x00000000;
    reg_addr_buf[14] = 0x00000000;
    reg_addr_buf[15] = 0x00000280;
#else
    reg_addr_buf[4]  = data_words[4];
    reg_addr_buf[5]  = data_words[5];
    reg_addr_buf[6]  = data_words[6];
    reg_addr_buf[7]  = data_words[7];
    reg_addr_buf[8]  = data_words[8];
    reg_addr_buf[9]  = data_words[9];
    reg_addr_buf[10] = data_words[10];
    reg_addr_buf[11] = data_words[11];
    reg_addr_buf[12] = data_words[12];
    reg_addr_buf[13] = data_words[13];
    reg_addr_buf[14] = data_words[14];
    reg_addr_buf[15] = data_words[15];
#endif
}

static inline void nerd_sha_ll_fill_text_block_sha256_double()
{
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

#if 0
    //No change
    reg_addr_buf[0]  = data_words[0];
    reg_addr_buf[1]  = data_words[1];
    reg_addr_buf[2]  = data_words[2];
    reg_addr_buf[3]  = data_words[3];
    reg_addr_buf[4]  = data_words[4];
    reg_addr_buf[5]  = data_words[5];
    reg_addr_buf[6]  = data_words[6];
    reg_addr_buf[7]  = data_words[7];
#endif
    reg_addr_buf[8]  = 0x80000000;
    reg_addr_buf[9]  = 0x00000000;
    reg_addr_buf[10] = 0x00000000;
    reg_addr_buf[11] = 0x00000000;
    reg_addr_buf[12] = 0x00000000;
    reg_addr_buf[13] = 0x00000000;
    reg_addr_buf[14] = 0x00000000;
    reg_addr_buf[15] = 0x00000100;
}

void minerWorkerHw(void * task_id)
{
  unsigned int miner_id = (uint32_t)task_id;
  Serial.printf("[MINER] %d Started minerWorkerHwEsp32D Task!\n", miner_id);

  std::shared_ptr<JobRequest> job;
  std::shared_ptr<JobResult> result;
  uint8_t hash[32];
  uint8_t sha_buffer[128];

  while (1)
  {
    {
      std::lock_guard<std::mutex> lock(s_job_mutex);
      if (result)
      {
        if (s_job_result_list.size() < 16)
          s_job_result_list.push_back(result);
        result.reset();
      }
      if (!s_job_request_list_hw.empty())
      {
        job = s_job_request_list_hw.front();
        s_job_request_list_hw.pop_front();
      } else
        job.reset();
    }
    if (job)
    {
      result = std::make_shared<JobResult>();
      result->id = job->id;
      result->nonce = 0xFFFFFFFF;
      result->nonce_count = job->nonce_count;
      result->difficulty = job->difficulty;
      uint8_t job_in_work = job->id & 0xFF;
      memcpy(sha_buffer, job->sha_buffer, 80);

      esp_sha_lock_engine(SHA2_256);
      for (uint32_t n = 0; n < job->nonce_count; ++n)
      {
        //((uint32_t*)(sha_buffer+64+12))[0] = __builtin_bswap32(job->nonce_start+n);

        //sha_hal_hash_block(SHA2_256, s_test_buffer, 64/4, true);
        //nerd_sha_hal_wait_idle();
        nerd_sha_ll_fill_text_block_sha256(sha_buffer);
        sha_ll_start_block(SHA2_256);

        //sha_hal_hash_block(SHA2_256, s_test_buffer+64, 64/4, false);
        nerd_sha_hal_wait_idle();
        nerd_sha_ll_fill_text_block_sha256_upper(sha_buffer+64, job->nonce_start+n);
        sha_ll_continue_block(SHA2_256);

        nerd_sha_hal_wait_idle();
        sha_ll_load(SHA2_256);

        //sha_hal_hash_block(SHA2_256, interResult, 64/4, true);
        nerd_sha_hal_wait_idle();
        nerd_sha_ll_fill_text_block_sha256_double();
        sha_ll_start_block(SHA2_256);

        nerd_sha_hal_wait_idle();
        sha_ll_load(SHA2_256);
        if (nerd_sha_ll_read_digest_swap_if(hash))
        {
          //~5 per second
          double diff_hash = diff_from_target(hash);
          if (diff_hash > result->difficulty)
          {
            if (isSha256Valid(hash))
            {
              result->difficulty = diff_hash;
              result->nonce = job->nonce_start+n;
              memcpy(result->hash, hash, sizeof(hash));
            }
          }
        }
        if (
             (uint8_t)(n & 0xFF) == 0 &&
             s_working_current_job_id != job_in_work)
        {
          result->nonce_count = n+1;
          break;
        }
      }
      esp_sha_unlock_engine(SHA2_256);
    } else
      vTaskDelay(2 / portTICK_PERIOD_MS);

    esp_task_wdt_reset();
  }
}

#endif  //CONFIG_IDF_TARGET_ESP32

#endif  //HARDWARE_SHA265


#define DELAY 100
#define REDRAW_EVERY 10

void restoreStat() {
  if(!Settings.saveStats) return;
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    Serial.printf("[MONITOR] NVS partition is full or has invalid version, erasing...\n");
    nvs_flash_init();
  }

  ret = nvs_open("state", NVS_READWRITE, &stat_handle);

  size_t required_size = sizeof(double);
  nvs_get_blob(stat_handle, "best_diff", &best_diff, &required_size);
  nvs_get_u32(stat_handle, "Mhashes", &Mhashes);
  uint32_t nv_shares = 0, nv_valids = 0, nv_acceptedShares = 0;
  nvs_get_u32(stat_handle, "shares", &nv_shares);
  nvs_get_u32(stat_handle, "valids", &nv_valids);
  nvs_get_u32(stat_handle, "acceptedShares", &nv_acceptedShares);
  shares = nv_shares;
  valids = nv_valids;
  acceptedShares = nv_acceptedShares;
  nvs_get_u32(stat_handle, "templates", &templates);
  nvs_get_u64(stat_handle, "upTime", &upTime);

  uint32_t crc = crc32_reset();
  crc = crc32_add(crc, &best_diff, sizeof(best_diff));
  crc = crc32_add(crc, &Mhashes, sizeof(Mhashes));
  crc = crc32_add(crc, &nv_shares, sizeof(nv_shares));
  crc = crc32_add(crc, &nv_valids, sizeof(nv_valids));
  crc = crc32_add(crc, &nv_acceptedShares, sizeof(nv_acceptedShares));
  crc = crc32_add(crc, &templates, sizeof(templates));
  crc = crc32_add(crc, &upTime, sizeof(upTime));
  crc = crc32_finish(crc);

  uint32_t nv_crc;
  nvs_get_u32(stat_handle, "crc32", &nv_crc);
  if (nv_crc != crc)
  {
    best_diff = 0.0;
    Mhashes = 0;
    shares = 0;
    valids = 0;
    acceptedShares = 0;
    templates = 0;
    upTime = 0;
  }
}

void saveStat() {
  if(!Settings.saveStats) return;
  Serial.printf("[MONITOR] Saving stats\n");
  nvs_set_blob(stat_handle, "best_diff", &best_diff, sizeof(best_diff));
  nvs_set_u32(stat_handle, "Mhashes", Mhashes);
  nvs_set_u32(stat_handle, "shares", shares);
  nvs_set_u32(stat_handle, "valids", valids);
  nvs_set_u32(stat_handle, "acceptedShares", acceptedShares);
  nvs_set_u32(stat_handle, "templates", templates);
  nvs_set_u64(stat_handle, "upTime", upTime);

  uint32_t crc = crc32_reset();
  crc = crc32_add(crc, &best_diff, sizeof(best_diff));
  crc = crc32_add(crc, &Mhashes, sizeof(Mhashes));
  uint32_t nv_shares = shares;
  uint32_t nv_valids = valids;
  uint32_t nv_acceptedShares = acceptedShares;
  crc = crc32_add(crc, &nv_shares, sizeof(nv_shares));
  crc = crc32_add(crc, &nv_valids, sizeof(nv_valids));
  crc = crc32_add(crc, &nv_acceptedShares, sizeof(nv_acceptedShares));
  crc = crc32_add(crc, &templates, sizeof(templates));
  crc = crc32_add(crc, &upTime, sizeof(upTime));
  crc = crc32_finish(crc);
  nvs_set_u32(stat_handle, "crc32", crc);
}

void resetStat() {
    Serial.printf("[MONITOR] Resetting NVS stats\n");
    templates = hashes = Mhashes = totalKHashes = elapsedKHs = upTime = shares = valids = acceptedShares = 0;
    best_diff = 0.0;
    saveStat();
}

void runMonitor(void *name)
{

  Serial.println("[MONITOR] started");
  restoreStat();

  unsigned long mLastCheck = 0;

  resetToFirstScreen();

  unsigned long frame = 0;

  uint32_t seconds_elapsed = 0;

  totalKHashes = (Mhashes * 1000) + hashes / 1000;
  uint32_t last_update_millis = millis();
  uint32_t uptime_frac = 0;

  while (1)
  {
    uint32_t now_millis = millis();
    if (now_millis < last_update_millis)
      now_millis = last_update_millis;
    
    uint32_t mElapsed = now_millis - mLastCheck;
    if (mElapsed >= 1000)
    { 
      mLastCheck = now_millis;
      last_update_millis = now_millis;
      unsigned long currentKHashes = (Mhashes * 1000) + hashes / 1000;
      elapsedKHs = currentKHashes - totalKHashes;
      totalKHashes = currentKHashes;

      uptime_frac += mElapsed;
      while (uptime_frac >= 1000)
      {
        uptime_frac -= 1000;
        upTime ++;
      }

      if (mMonitor.NerdStatus == NM_waitingConfig) {
        drawSetupScreen();
      } else {
        drawCurrentScreen(mElapsed);
      }

      // Monitor state when hashrate is 0.0
      if (elapsedKHs == 0)
      {
        Serial.printf(">>> [i] Miner: newJob>%s / inRun>%s) - Client: connected>%s / subscribed>%s / wificonnected>%s\n",
            "true",//(1) ? "true" : "false",
            isMinerSuscribed ? "true" : "false",
            client.connected() ? "true" : "false", isMinerSuscribed ? "true" : "false", WiFi.status() == WL_CONNECTED ? "true" : "false");
      }

      #ifdef DEBUG_MEMORY
      Serial.printf("### [Total Heap / Free heap / Min free heap]: %d / %d / %d \n", ESP.getHeapSize(), ESP.getFreeHeap(), ESP.getMinFreeHeap());
      Serial.printf("### Max stack usage: %d\n", uxTaskGetStackHighWaterMark(NULL));
      #endif

      seconds_elapsed++;

      if(seconds_elapsed % (saveIntervals[currentIntervalIndex]) == 0){
        saveStat();
        seconds_elapsed = 0;
        if(currentIntervalIndex < saveIntervalsSize - 1)
          currentIntervalIndex++;
      }    
    }
    animateCurrentScreen(frame);
    doLedStuff(frame);

    vTaskDelay(DELAY / portTICK_PERIOD_MS);
    frame++;
  }
}
