#include <Arduino.h>
#include <WiFi.h>
#include "mbedtls/md.h"
#include "HTTPClient.h"
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <list>
#include "mining.h"
#include "utils.h"
#include "monitor.h"
#include "drivers/storage/storage.h"
#include "drivers/devices/device.h"

extern uint32_t templates;
extern uint32_t hashes;
extern uint32_t Mhashes;
extern uint32_t totalKHashes;
extern uint32_t elapsedKHs;
extern uint64_t upTime;

extern uint32_t shares; // increase if blockhash has 32 bits of zeroes
extern uint32_t valids; // increased if blockhash <= targethalfshares
extern uint32_t acceptedShares; // accepted by pool

extern double best_diff; // track best diff

extern monitor_data mMonitor;

//from saved config
extern TSettings Settings; 
bool invertColors = false;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", 3600, 60000);
unsigned int bitcoin_price=0;
String current_block = "0";
global_data gData;
pool_data pData;
String poolAPIUrl;
String poolNetworkStatsUrl;
unsigned long mGlobalUpdate = 0;
unsigned long mHeightUpdate = 0;
unsigned long mBTCUpdate = 0;


static String formatHashrateEH(double hashrateHps)
{
    if (hashrateHps <= 0) {
        return "0.00";
    }

    const double exaHash = hashrateHps / 1000000000000000000.0;
    char output[16] = {0};
    if (exaHash >= 100.0) {
        snprintf(output, sizeof(output), "%.0f", exaHash);
    } else if (exaHash >= 10.0) {
        snprintf(output, sizeof(output), "%.1f", exaHash);
    } else {
        snprintf(output, sizeof(output), "%.2f", exaHash);
    }
    return String(output);
}

static String formatDifficultyT(double difficulty)
{
    if (difficulty <= 0) {
        return "0.00T";
    }

    const double teraDifficulty = difficulty / 1000000000000.0;
    char output[16] = {0};
    if (teraDifficulty >= 100.0) {
        snprintf(output, sizeof(output), "%.0fT", teraDifficulty);
    } else if (teraDifficulty >= 10.0) {
        snprintf(output, sizeof(output), "%.1fT", teraDifficulty);
    } else {
        snprintf(output, sizeof(output), "%.2fT", teraDifficulty);
    }
    return String(output);
}

static String getPoolNetworkStatsUrl(void)
{
    String base = getPoolAPIUrl();
    int apiClientIdx = base.indexOf("/api/client/");
    if (apiClientIdx > 0) {
        return base.substring(0, apiClientIdx) + "/api/stats";
    }

    apiClientIdx = base.indexOf("/api/client");
    if (apiClientIdx > 0) {
        return base.substring(0, apiClientIdx) + "/api/stats";
    }

    if (base.endsWith("/")) {
        base.remove(base.length() - 1);
    }
    return base + "/api/stats";
}

static bool allowExternalNetworkFallback(void)
{
    String configuredBase = String(Settings.PoolApiBase);
    configuredBase.trim();
    if (configuredBase.length() > 0) {
        return false;
    }

    String poolAddress = Settings.PoolAddress;
    poolAddress.trim();
    poolAddress.toLowerCase();

    return (poolAddress == "public-pool.io"
        || poolAddress == "pool.nerdminers.org"
        || poolAddress == "pool.sethforprivacy.com"
        || poolAddress == "pool.solomining.de");
}

static bool updatePoolNetworkStats(bool forceRefresh)
{
    const unsigned long now = millis();
    if (!forceRefresh && mGlobalUpdate != 0 && now - mGlobalUpdate <= UPDATE_Global_min * 60 * 1000) {
        return true;
    }

    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    poolNetworkStatsUrl = getPoolNetworkStatsUrl();

    HTTPClient http;
    http.setTimeout(10000);
    try {
        http.begin(poolNetworkStatsUrl);
        const int httpCode = http.GET();
        if (httpCode != HTTP_CODE_OK) {
            Serial.println("Pool stats HTTP error " + String(httpCode) + " from " + poolNetworkStatsUrl);
            http.end();
            return false;
        }

        String payload = http.getString();
        http.end();

        StaticJsonDocument<1024> doc;
        DeserializationError error = deserializeJson(doc, payload);
        if (error) {
            Serial.println("Pool stats JSON parse failed from " + poolNetworkStatsUrl);
            return false;
        }

        long blockHeight = 0;
        if (doc.containsKey("blockHeight")) {
            blockHeight = doc["blockHeight"].as<long>();
        } else if (doc.containsKey("blocks")) {
            blockHeight = doc["blocks"].as<long>();
        }

        int blocksTillHalving = 0;
        if (doc.containsKey("blocksTillHalving")) {
            blocksTillHalving = doc["blocksTillHalving"].as<int>();
        } else if (blockHeight > 0) {
            blocksTillHalving = (((blockHeight / HALVING_BLOCKS) + 1) * HALVING_BLOCKS) - blockHeight;
        }

        int mediumFeeSatVb = gData.halfHourFee;
        if (doc.containsKey("mediumFeeSatVb")) {
            mediumFeeSatVb = doc["mediumFeeSatVb"].as<int>();
        } else if (doc.containsKey("halfHourFee")) {
            mediumFeeSatVb = doc["halfHourFee"].as<int>();
        }

        double networkDifficulty = 0;
        if (doc.containsKey("networkDifficulty")) {
            networkDifficulty = doc["networkDifficulty"].as<double>();
        } else if (doc.containsKey("difficulty")) {
            networkDifficulty = doc["difficulty"].as<double>();
        }

        double networkHashrate = 0;
        if (doc.containsKey("networkHashrate")) {
            networkHashrate = doc["networkHashrate"].as<double>();
        } else if (doc.containsKey("networkhashps")) {
            networkHashrate = doc["networkhashps"].as<double>();
        } else if (doc.containsKey("currentHashrate")) {
            networkHashrate = doc["currentHashrate"].as<double>();
        }

        if (doc.containsKey("priceUsd")) {
            bitcoin_price = (unsigned int)doc["priceUsd"].as<double>();
        } else if (doc.containsKey("btcPriceUsd")) {
            bitcoin_price = (unsigned int)doc["btcPriceUsd"].as<double>();
        }

        if (blockHeight > 0) {
            current_block = String(blockHeight);
            gData.currentBlock = current_block;
        }
        gData.remainingBlocks = blocksTillHalving;
        gData.progressPercent = blocksTillHalving > 0
            ? ((HALVING_BLOCKS - blocksTillHalving) * 100.0f / HALVING_BLOCKS)
            : 0;
        gData.halfHourFee = mediumFeeSatVb;
        gData.difficulty = formatDifficultyT(networkDifficulty);
        gData.globalHash = formatHashrateEH(networkHashrate);

        mGlobalUpdate = now;
        mHeightUpdate = now;
        mBTCUpdate = now;
        return true;
    } catch (...) {
        Serial.println("Pool stats request failed for " + poolNetworkStatsUrl);
        http.end();
        return false;
    }
}

void setup_monitor(void){
    /******** TIME ZONE SETTING *****/

    timeClient.begin();
    
    // Adjust offset depending on your zone
    // GMT +2 in seconds (zona horaria de Europa Central)
    timeClient.setTimeOffset(3600 * Settings.Timezone);

    Serial.println("TimeClient setup done");
#ifdef SCREEN_WORKERS_ENABLE
    poolAPIUrl = getPoolAPIUrl();
    Serial.println("poolAPIUrl: " + poolAPIUrl);
#endif
}

void updateGlobalData(void){
    
    if((mGlobalUpdate == 0) || (millis() - mGlobalUpdate > UPDATE_Global_min * 60 * 1000)){
    
        if (WiFi.status() != WL_CONNECTED) return;

        if (updatePoolNetworkStats(true)) {
            return;
        }

        if (!allowExternalNetworkFallback()) {
            return;
        }
            
        //Make first API call to get global hash and current difficulty
        HTTPClient http;
        http.setTimeout(10000);
        try {
        http.begin(getGlobalHash);
        int httpCode = http.GET();

        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            
            StaticJsonDocument<1024> doc;
            deserializeJson(doc, payload);
            String temp = "";
            if (doc.containsKey("currentHashrate")) temp = String(doc["currentHashrate"].as<float>());
            if(temp.length()>18 + 3) //Exahashes more than 18 digits + 3 digits decimals
              gData.globalHash = temp.substring(0,temp.length()-18 - 3);
            if (doc.containsKey("currentDifficulty")) temp = String(doc["currentDifficulty"].as<float>());
            if(temp.length()>10 + 3){ //Terahash more than 10 digits + 3 digit decimals
              temp = temp.substring(0,temp.length()-10 - 3);
              gData.difficulty = temp.substring(0,temp.length()-2) + "." + temp.substring(temp.length()-2,temp.length()) + "T";
            }
            doc.clear();

            mGlobalUpdate = millis();
        }
        http.end();

      
        //Make third API call to get fees
        http.begin(getFees);
        httpCode = http.GET();

        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            
            StaticJsonDocument<1024> doc;
            deserializeJson(doc, payload);
            String temp = "";
            if (doc.containsKey("halfHourFee")) gData.halfHourFee = doc["halfHourFee"].as<int>();
#ifdef SCREEN_FEES_ENABLE
            if (doc.containsKey("fastestFee"))  gData.fastestFee = doc["fastestFee"].as<int>();
            if (doc.containsKey("hourFee"))     gData.hourFee = doc["hourFee"].as<int>();
            if (doc.containsKey("economyFee"))  gData.economyFee = doc["economyFee"].as<int>();
            if (doc.containsKey("minimumFee"))  gData.minimumFee = doc["minimumFee"].as<int>();
#endif
            doc.clear();

            mGlobalUpdate = millis();
        }
        
        http.end();
        } catch(...) {
          Serial.println("Global data HTTP error caught");
          http.end();
        }
    }
}

String getBlockHeight(void){
    
    if((mHeightUpdate == 0) || (millis() - mHeightUpdate > UPDATE_Height_min * 60 * 1000)){
    
        if (WiFi.status() != WL_CONNECTED) return current_block;

        if (updatePoolNetworkStats(true)) {
            return current_block;
        }

        if (!allowExternalNetworkFallback()) {
            return current_block;
        }
            
        HTTPClient http;
        http.setTimeout(10000);
        try {
        http.begin(getHeightAPI);
        int httpCode = http.GET();

        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            payload.trim();

            current_block = payload;

            mHeightUpdate = millis();
        }        
        http.end();
        } catch(...) {
          Serial.println("Height HTTP error caught");
          http.end();
        }
    }
  
  return current_block;
}

String getBTCprice(void){
    
    if((mBTCUpdate == 0) || (millis() - mBTCUpdate > UPDATE_BTC_min * 60 * 1000)){
    
        if (WiFi.status() != WL_CONNECTED) {
            static char price_buffer[16];
            snprintf(price_buffer, sizeof(price_buffer), "$%u", bitcoin_price);
            return String(price_buffer);
        }

        if (updatePoolNetworkStats(true)) {
            static char price_buffer[16];
            snprintf(price_buffer, sizeof(price_buffer), "$%u", bitcoin_price);
            return String(price_buffer);
        }

        if (!allowExternalNetworkFallback()) {
            static char price_buffer[16];
            snprintf(price_buffer, sizeof(price_buffer), "$%u", bitcoin_price);
            return String(price_buffer);
        }
        
        HTTPClient http;
        http.setTimeout(10000);
        bool priceUpdated = false;

        try {
        http.begin(getBTCAPI);
        int httpCode = http.GET();

        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();

            StaticJsonDocument<1024> doc;
            deserializeJson(doc, payload);
          
            if (doc.containsKey("bitcoin") && doc["bitcoin"].containsKey("usd")) {
                bitcoin_price = doc["bitcoin"]["usd"];
            }

            doc.clear();

            mBTCUpdate = millis();
        }
        
        http.end();
        } catch(...) {
          Serial.println("BTC price HTTP error caught");
          http.end();
        }
    }  
  
  static char price_buffer[16];
  snprintf(price_buffer, sizeof(price_buffer), "$%u", bitcoin_price);
  return String(price_buffer);
}

unsigned long mTriggerUpdate = 0;
unsigned long initialMillis = millis();
unsigned long initialTime = 0;
unsigned long mPoolUpdate = 0;

void getTime(unsigned long* currentHours, unsigned long* currentMinutes, unsigned long* currentSeconds){
  
  //Check if need an NTP call to check current time
  if((mTriggerUpdate == 0) || (millis() - mTriggerUpdate > UPDATE_PERIOD_h * 60 * 60 * 1000)){ //60 sec. * 60 min * 1000ms
    if(WiFi.status() == WL_CONNECTED) {
        if(timeClient.update()) mTriggerUpdate = millis(); //NTP call to get current time
        initialTime = timeClient.getEpochTime(); // Guarda la hora inicial (en segundos desde 1970)
        Serial.print("TimeClient NTPupdateTime ");
    }
  }

  unsigned long elapsedTime = (millis() - mTriggerUpdate) / 1000; // Tiempo transcurrido en segundos
  unsigned long currentTime = initialTime + elapsedTime; // La hora actual

  // convierte la hora actual en horas, minutos y segundos
  *currentHours = currentTime % 86400 / 3600;
  *currentMinutes = currentTime % 3600 / 60;
  *currentSeconds = currentTime % 60;
}

String getDate(){
  
  unsigned long elapsedTime = (millis() - mTriggerUpdate) / 1000; // Tiempo transcurrido en segundos
  unsigned long currentTime = initialTime + elapsedTime; // La hora actual

  // Convierte la hora actual (epoch time) en una estructura tm
  struct tm *tm = localtime((time_t *)&currentTime);

  int year = tm->tm_year + 1900; // tm_year es el número de años desde 1900
  int month = tm->tm_mon + 1;    // tm_mon es el mes del año desde 0 (enero) hasta 11 (diciembre)
  int day = tm->tm_mday;         // tm_mday es el día del mes

  char currentDate[20];
  sprintf(currentDate, "%02d/%02d/%04d", tm->tm_mday, tm->tm_mon + 1, tm->tm_year + 1900);

  return String(currentDate);
}

String getTime(void){
  unsigned long currentHours, currentMinutes, currentSeconds;
  getTime(&currentHours, &currentMinutes, &currentSeconds);

  char LocalHour[10];
  sprintf(LocalHour, "%02d:%02d", currentHours, currentMinutes);
  
  String mystring(LocalHour);
  return LocalHour;
}

enum EHashRateScale
{
  HashRateScale_99KH,
  HashRateScale_999KH,
  HashRateScale_9MH
};

static EHashRateScale s_hashrate_scale = HashRateScale_99KH;
static uint32_t s_skip_first = 3;
static double s_top_hashrate = 0.0;

static std::list<double> s_hashrate_avg_list;
static double s_hashrate_summ = 0.0;
static uint8_t s_hashrate_recalc = 0;

String getCurrentHashRate(unsigned long mElapsed)
{
  double hashrate = (double)elapsedKHs * 1000.0 / (double)mElapsed;

  s_hashrate_summ += hashrate;
  s_hashrate_avg_list.push_back(hashrate);
  if (s_hashrate_avg_list.size() > 10)
  {
    s_hashrate_summ -= s_hashrate_avg_list.front();
    s_hashrate_avg_list.pop_front();
  }

  ++s_hashrate_recalc;
  if (s_hashrate_recalc == 0)
  {
    s_hashrate_summ = 0.0;
    for (auto itt = s_hashrate_avg_list.begin(); itt != s_hashrate_avg_list.end(); ++itt)
      s_hashrate_summ += *itt;
  }

  double avg_hashrate = s_hashrate_summ / (double)s_hashrate_avg_list.size();
  if (avg_hashrate < 0.0)
    avg_hashrate = 0.0;

  if (s_skip_first > 0)
  {
    s_skip_first--;
  } else
  {
    if (avg_hashrate > s_top_hashrate)
    {
      s_top_hashrate = avg_hashrate;
      if (avg_hashrate > 999.9)
        s_hashrate_scale = HashRateScale_9MH;
      else if (avg_hashrate > 99.9)
        s_hashrate_scale = HashRateScale_999KH;
    }
  }

  switch (s_hashrate_scale)
  {
    case HashRateScale_99KH:
      return String(avg_hashrate, 2);
    case HashRateScale_999KH:
      return String(avg_hashrate, 1);
    default:
      return String((int)avg_hashrate );
  }
}

mining_data getMiningData(unsigned long mElapsed)
{
  mining_data data;

  char best_diff_string[16] = {0};
  suffix_string(best_diff, best_diff_string, 16, 0);

  char timeMining[15] = {0};
  uint64_t tm = upTime;
  int secs = tm % 60;
  tm /= 60;
  int mins = tm % 60;
  tm /= 60;
  int hours = tm % 24;
  int days = tm / 24;
  sprintf(timeMining, "%01d  %02d:%02d:%02d", days, hours, mins, secs);

  data.completedShares = acceptedShares;
  data.totalMHashes = Mhashes;
  data.totalKHashes = totalKHashes;
  data.currentHashRate = getCurrentHashRate(mElapsed);
  data.templates = templates;
  data.bestDiff = best_diff_string;
  data.timeMining = timeMining;
  data.valids = valids;
  data.temp = String(temperatureRead(), 0);
  data.currentTime = getTime();

  return data;
}

clock_data getClockData(unsigned long mElapsed)
{
  clock_data data;

  data.completedShares = acceptedShares;
  data.totalKHashes = totalKHashes;
  data.currentHashRate = getCurrentHashRate(mElapsed);
  data.btcPrice = getBTCprice();
  data.blockHeight = getBlockHeight();
  data.currentTime = getTime();
  data.currentDate = getDate();

  return data;
}

clock_data_t getClockData_t(unsigned long mElapsed)
{
  clock_data_t data;

  data.valids = valids;
  data.currentHashRate = getCurrentHashRate(mElapsed);
  getTime(&data.currentHours, &data.currentMinutes, &data.currentSeconds);

  return data;
}

coin_data getCoinData(unsigned long mElapsed)
{
  coin_data data;

  updateGlobalData(); // Update gData vars asking mempool APIs

  data.completedShares = acceptedShares;
  data.totalKHashes = totalKHashes;
  data.currentHashRate = getCurrentHashRate(mElapsed);
  data.btcPrice = getBTCprice();
  data.currentTime = getTime();
#ifdef SCREEN_FEES_ENABLE
  data.hourFee = String(gData.hourFee);
  data.fastestFee = String(gData.fastestFee);
  data.economyFee = String(gData.economyFee);
  data.minimumFee = String(gData.minimumFee);
#endif
  data.halfHourFee = String(gData.halfHourFee) + " jat/vB";
  data.netwrokDifficulty = gData.difficulty;
  data.globalHashRate = gData.globalHash;
  data.blockHeight = getBlockHeight();

  unsigned long currentBlock = data.blockHeight.toInt();
  unsigned long remainingBlocks = gData.remainingBlocks > 0
    ? (unsigned long)gData.remainingBlocks
    : (((currentBlock / HALVING_BLOCKS) + 1) * HALVING_BLOCKS) - currentBlock;
  data.progressPercent = gData.remainingBlocks > 0
    ? gData.progressPercent
    : (HALVING_BLOCKS - remainingBlocks) * 100 / HALVING_BLOCKS;
  data.remainingBlocks = String(remainingBlocks) + " BLOCKS";

  return data;
}

String getPoolAPIUrl(void) {
    String configuredBase = String(Settings.PoolApiBase);
    configuredBase.trim();

    if (configuredBase.length() > 0) {
        configuredBase.replace("\\", "/");
        if (!configuredBase.endsWith("/")) {
            configuredBase += "/";
        }
        if (configuredBase.indexOf("/api/client/") > 0) {
            poolAPIUrl = configuredBase;
        } else if (configuredBase.indexOf("/api/client") > 0) {
            poolAPIUrl = configuredBase + "/";
        } else {
            poolAPIUrl = configuredBase + "api/client/";
        }
        return poolAPIUrl;
    }

    poolAPIUrl = String(getPublicPool);
    if (Settings.PoolAddress == "public-pool.io") {
        poolAPIUrl = "https://public-pool.io:40557/api/client/";
    } 
    else {
        if (Settings.PoolAddress == "pool.nerdminers.org") {
            poolAPIUrl = "https://pool.nerdminers.org/users/";
        }
        else {
            switch (Settings.PoolPort) {
                case 3333:
                    poolAPIUrl = "http://" + Settings.PoolAddress + ":3334/api/client/";
                    if (Settings.PoolAddress == "pool.sethforprivacy.com")
                        poolAPIUrl = "https://pool.sethforprivacy.com/api/client/";
                    if (Settings.PoolAddress == "pool.solomining.de")
                        poolAPIUrl = "https://pool.solomining.de/api/client/";
                    // Add more cases for other addresses with port 3333 if needed
                    break;
                case 2018:
                    // Local instance of public-pool.io on Umbrel or Start9
                    poolAPIUrl = "http://" + Settings.PoolAddress + ":2019/api/client/";
                    break;
                default:
                    poolAPIUrl = String(getPublicPool);
                    break;
            }
        }
    }
    return poolAPIUrl;
}

pool_data getPoolData(void){
    //pool_data pData;    
    if((mPoolUpdate == 0) || (millis() - mPoolUpdate > UPDATE_POOL_min * 60 * 1000)){      
        if (WiFi.status() != WL_CONNECTED) return pData;            
        poolAPIUrl = getPoolAPIUrl();
        //Make first API call to get global hash and current difficulty
        HTTPClient http;
        http.setTimeout(10000);        
        try {          
          String btcWallet = Settings.BtcWallet;
          // Serial.println(btcWallet);
          if (btcWallet.indexOf(".")>0) btcWallet = btcWallet.substring(0,btcWallet.indexOf("."));
#ifdef SCREEN_WORKERS_ENABLE
          Serial.println("Pool API : " + poolAPIUrl+btcWallet);
          http.begin(poolAPIUrl+btcWallet);
#else
          http.begin(String(getPublicPool)+btcWallet);
#endif
          int httpCode = http.GET();
          if (httpCode == HTTP_CODE_OK) {
              String payload = http.getString();
              // Serial.println(payload);
              StaticJsonDocument<300> filter;
              filter["bestDifficulty"] = true;
              filter["workersCount"] = true;
              filter["workers"][0]["sessionId"] = true;
              filter["workers"][0]["hashRate"] = true;
              StaticJsonDocument<2048> doc;
              deserializeJson(doc, payload, DeserializationOption::Filter(filter));
              //Serial.println(serializeJsonPretty(doc, Serial));
              if (doc.containsKey("workersCount")) pData.workersCount = doc["workersCount"].as<int>();
              const JsonArray& workers = doc["workers"].as<JsonArray>();
              float totalhashs = 0;
              for (const JsonObject& worker : workers) {
                totalhashs += worker["hashRate"].as<double>();
                /* Serial.print(worker["sessionId"].as<String>()+": ");
                Serial.print(" - "+worker["hashRate"].as<String>()+": ");
                Serial.println(totalhashs); */
              }
              char totalhashs_s[16] = {0};
              suffix_string(totalhashs, totalhashs_s, 16, 0);
              pData.workersHash = String(totalhashs_s);

              double temp;
              if (doc.containsKey("bestDifficulty")) {
              temp = doc["bestDifficulty"].as<double>();            
              char best_diff_string[16] = {0};
              suffix_string(temp, best_diff_string, 16, 0);
              pData.bestDifficulty = String(best_diff_string);
              }
              doc.clear();
              mPoolUpdate = millis();
              Serial.println("\n####### Pool Data OK!");               
          } else {
              Serial.println("\n####### Pool Data HTTP Error!");    
              /* Serial.println(httpCode);
              String payload = http.getString();
              Serial.println(payload); */
              // mPoolUpdate = millis();
              pData.bestDifficulty = "P";
              pData.workersHash = "E";
              pData.workersCount = 0;
              http.end();
              return pData; 
          }
          http.end();
        } catch(...) {
          Serial.println("####### Pool Error!");          
          // mPoolUpdate = millis();
          pData.bestDifficulty = "P";
          pData.workersHash = "Error";
          pData.workersCount = 0;
          http.end();
          return pData;
        } 
    }
    return pData;
}
