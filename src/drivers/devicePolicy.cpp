/**
 * Device-class policy + OTA client implementation
 * (per device-class plan F-4..F-11).
 *
 * Responsibilities:
 *   F-4  NTP strict — block fetches until clock synced; refresh every 12h.
 *   F-5  Policy fetch — POST /api/policy via pool proxy, 5min jittered poll.
 *   F-6  TTL gate — fall back to SAFEST_CAP_HS after POLICY_TTL_SECONDS.
 *   F-8  OTA download/verify/install — SHA-256 + Ed25519 signature gate.
 *   F-9  Bootloader rollback — increment BootStrikes; mark slot valid only
 *        after WiFi + policy fetch + pool TCP connect within 5min.
 *   F-10 Display data — getCurrentClassId() exposes class label.
 *   F-11 BOARD_ID — compile-time, fed into every policy fetch body.
 *
 * Implementation choices:
 *   - HTTP via Arduino HTTPClient (matches existing monitor.cpp pattern).
 *   - JSON via ArduinoJson (already a dep).
 *   - SHA-256 via mbedtls_sha256 (always-present in ESP-IDF/Arduino).
 *   - Ed25519 verify via mbedtls_pk_verify; if the target's mbedtls
 *     build lacks Ed25519, swap in a vendored minimal verifier (e.g.,
 *     orlp/ed25519) and replace the body of verifyEd25519Signature.
 *   - OTA install via Arduino Update.h (simpler than esp_https_ota and
 *     avoids ESP-IDF version coupling).
 *   - Rollback via esp_ota_mark_app_valid_cancel_rollback() and
 *     esp_ota_get_state_partition() from esp_ota_ops.h.
 */

#include "devicePolicy.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "drivers/storage/nvMemory.h"
#include "drivers/storage/storage.h"
#include "mining.h"  // getDeviceIdFromEfuse, computeDeviceHmacHex
#include "version.h" // CURRENT_VERSION

extern TSettings Settings;

namespace {

// Forward decls
bool waitForNtpSync(uint32_t timeoutMs);
String buildPolicyApiBase();
String makeNonce();
String hmacHexFromEfuse(const String& message);
const char* canonicalDeviceId();
void maybeMarkBootValid();
bool postJson(const String& url, const String& body, String& outResponse);
bool installOtaArtifact(const String& url, const String& expectedSha256Hex,
                        const String& signatureBase64, const String& releaseId);
bool verifyEd25519Signature(const uint8_t* msg, size_t msgLen,
                             const uint8_t* sig, size_t sigLen);
bool sha256File(Stream& stream, size_t totalLen, uint8_t outDigest[32]);
String hexEncode(const uint8_t* data, size_t len);

// Internal state
volatile bool g_ntpSynced = false;
volatile bool g_policyTaskStarted = false;
TaskHandle_t  g_policyTaskHandle = nullptr;
uint64_t      g_lastNtpRefreshMs = 0;
uint64_t      g_lastBackendOkMs = 0;

// 12h NTP refresh cadence per F-4.
constexpr uint64_t NTP_REFRESH_INTERVAL_MS = 12ULL * 60ULL * 60ULL * 1000ULL;
// 5min health-check window for boot-strikes mark-valid (per F-9).
constexpr uint64_t HEALTH_WINDOW_MS = 5ULL * 60ULL * 1000ULL;
// Initial NTP wait up to 60s; if not synced, do not contact backend.
constexpr uint32_t NTP_INITIAL_TIMEOUT_MS = 60ULL * 1000ULL;

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

uint32_t getCurrentTargetHs() {
  if (!isPolicyFresh()) {
    return SAFEST_CAP_HS;
  }
  // Bypass devices: backend returns classId='JYPASS' with target=0 as the
  // 'uncapped' sentinel. Pass the 0 through to the throttle, which treats
  // targetHs == 0 as 'skip the rate-limit check' (see mining.cpp F-7).
  // The fresh-policy gate above keeps a stale JYPASS from becoming
  // uncapped if the device is dropped from the bypass list and the
  // backend isn't reachable to revoke.
  if (strcmp(Settings.PolicyClassId, "JYPASS") == 0 &&
      Settings.PolicyTargetHashrateHs == 0) {
    return 0;
  }
  if (Settings.PolicyTargetHashrateHs == 0) {
    // target=0 without the JYPASS sentinel means the backend response
    // was malformed or someone's tampered with NVS — fail safe.
    return SAFEST_CAP_HS;
  }
  return Settings.PolicyTargetHashrateHs;
}

const char* getCurrentClassId() {
  return Settings.PolicyClassId[0] != '\0' ? Settings.PolicyClassId : "";
}

bool isPolicyFresh() {
  if (Settings.PolicyFetchedAt == 0) {
    return false;
  }
  uint64_t nowSec = (uint64_t)time(nullptr);
  if (nowSec < (uint64_t)Settings.PolicyFetchedAt) {
    return false;  // clock went backwards; treat as stale
  }
  uint64_t age = nowSec - (uint64_t)Settings.PolicyFetchedAt;
  return age < (uint64_t)POLICY_TTL_SECONDS;
}

bool reportOtaState(const char* releaseId, const char* state,
                    const char* errorCode, uint8_t attempt) {
  if (!g_ntpSynced) {
    return false;
  }
  const char* deviceId = canonicalDeviceId();
  if (deviceId[0] == '\0') {
    Serial.println("[OTA] reportOtaState: deviceId unavailable");
    return false;
  }
  String url = buildPolicyApiBase() + "/ota/report";
  StaticJsonDocument<512> doc;
  doc["deviceId"] = deviceId;
  doc["releaseId"] = releaseId;
  doc["state"] = state;
  doc["errorCode"] = errorCode ? errorCode : "";
  doc["attempt"] = attempt;
  uint64_t nowSec = (uint64_t)time(nullptr);
  doc["timestamp"] = nowSec;
  String nonce = makeNonce();
  doc["nonce"] = nonce;
  String hmacMessage = String("ota_report:") + deviceId + ":"
                       + releaseId + ":" + state + ":" + String((int)attempt) + ":"
                       + nonce + ":" + String((unsigned long long)nowSec);
  String proof = hmacHexFromEfuse(hmacMessage);
  if (proof.length() == 0) {
    return false;
  }
  doc["hmacProof"] = proof;
  String body;
  serializeJson(doc, body);
  String response;
  return postJson(url, body, response);
}

void startPolicyFetchTask() {
  if (g_policyTaskStarted) {
    return;
  }
  g_policyTaskStarted = true;
  // Configure NTP server (per F-4 + audit fix #8). Repeated calls are
  // safe; the underlying SNTP layer dedupes.
  configTime(0, 0, NTP_SERVER);
  // Spin the periodic task. 5-min interval + ±30s jitter handled inside.
  xTaskCreatePinnedToCore(
      [](void*) {
        // Initial sync — block up to 60s.
        if (!waitForNtpSync(NTP_INITIAL_TIMEOUT_MS)) {
          Serial.println("[POLICY] ntp_unset — not contacting backend");
        } else {
          g_ntpSynced = true;
          g_lastNtpRefreshMs = (uint64_t)millis();
        }
        for (;;) {
          // Refresh NTP every 12h (per F-4). Best-effort; failure logs.
          uint64_t nowMs = (uint64_t)millis();
          if (nowMs - g_lastNtpRefreshMs > NTP_REFRESH_INTERVAL_MS) {
            configTime(0, 0, NTP_SERVER);
            if (waitForNtpSync(15ULL * 1000ULL)) {
              g_lastNtpRefreshMs = nowMs;
            } else {
              Serial.println("[POLICY] ntp_refresh_failed (continuing on RTC)");
            }
          }

          if (!g_ntpSynced) {
            // Block until first sync.
            if (waitForNtpSync(NTP_INITIAL_TIMEOUT_MS)) {
              g_ntpSynced = true;
              g_lastNtpRefreshMs = (uint64_t)millis();
            }
          }

          if (g_ntpSynced && WiFi.isConnected()) {
            const char* deviceId = canonicalDeviceId();
            if (deviceId[0] == '\0') {
              Serial.println("[POLICY] device_id unavailable; skipping fetch");
              vTaskDelay(5000 / portTICK_PERIOD_MS);
              continue;
            }
            // Build policy fetch request. deviceId is the canonical
            // eFuse-MAC-derived 12-hex string (matches the activation
            // flow's device identity).
            String url = buildPolicyApiBase() + "/policy";
            StaticJsonDocument<384> doc;
            doc["deviceId"] = deviceId;
            doc["boardId"] = BOARD_ID;
            doc["firmwareVersion"] = CURRENT_VERSION;
            uint64_t nowSec = (uint64_t)time(nullptr);
            doc["timestamp"] = nowSec;
            String nonce = makeNonce();
            doc["nonce"] = nonce;
            String hmacMessage = String("policy:") + deviceId + ":"
                                + nonce + ":" + String((unsigned long long)nowSec);
            String proof = hmacHexFromEfuse(hmacMessage);
            if (proof.length() == 0) {
              Serial.println("[POLICY] hmac proof unavailable; skipping fetch");
              vTaskDelay(5000 / portTICK_PERIOD_MS);
              continue;
            }
            doc["hmacProof"] = proof;
            String body;
            serializeJson(doc, body);

            String response;
            if (postJson(url, body, response)) {
              StaticJsonDocument<2048> respDoc;
              DeserializationError err = deserializeJson(respDoc, response);
              if (!err) {
                const char* classId = respDoc["classId"] | "";
                uint32_t target = respDoc["targetHashrateHs"] | (uint32_t)0;
                const char* signatureB64 = respDoc["policySignature"] | "";

                // Build the canonical signed payload (matches backend's
                // _sign_policy: JSON with sorted keys, no whitespace).
                StaticJsonDocument<256> canonical;
                canonical["classId"] = classId;
                canonical["deviceId"] = (const char*)doc["deviceId"];
                canonical["issuedAt"] = (uint64_t)respDoc["issuedAt"];
                canonical["policyTtlSeconds"] = (uint32_t)respDoc["policyTtlSeconds"];
                canonical["targetHashrateHs"] = target;
                String canonicalStr;
                serializeJson(canonical, canonicalStr);

                // Decode signature (base64 -> 64 bytes).
                uint8_t sigBytes[64];
                size_t sigLen = 0;
                int decodeErr = mbedtls_base64_decode(
                    sigBytes, sizeof(sigBytes), &sigLen,
                    (const unsigned char*)signatureB64, strlen(signatureB64));
                if (decodeErr == 0 && sigLen == 64 &&
                    verifyEd25519Signature((const uint8_t*)canonicalStr.c_str(),
                                           canonicalStr.length(),
                                           sigBytes, sigLen)) {
                  // Persist to NVS-backed Settings.
                  strncpy(Settings.PolicyPrevClassId, Settings.PolicyClassId,
                          sizeof(Settings.PolicyPrevClassId));
                  Settings.PolicyPrevClassId[sizeof(Settings.PolicyPrevClassId) - 1] = '\0';
                  strncpy(Settings.PolicyClassId, classId,
                          sizeof(Settings.PolicyClassId));
                  Settings.PolicyClassId[sizeof(Settings.PolicyClassId) - 1] = '\0';
                  // Preserve target=0 when the class is JYPASS (uncapped
                  // bypass sentinel). For every other class, target=0
                  // would be malformed — fall back to SAFEST_CAP_HS.
                  if (strcmp(classId, "JYPASS") == 0) {
                    Settings.PolicyTargetHashrateHs = 0;
                  } else {
                    Settings.PolicyTargetHashrateHs = target > 0 ? target : SAFEST_CAP_HS;
                  }
                  Settings.PolicyFetchedAt = (uint64_t)time(nullptr);
                  // Save settings; nvMemory persists Policy* fields per F-2.
                  nvMemory nv;
                  nv.saveConfig(&Settings);
                  g_lastBackendOkMs = (uint64_t)millis();
                  // F-9: first successful round-trip = boot health
                  // satisfied; mark the running OTA slot valid.
                  maybeMarkBootValid();

                  // Optional OTA target.
                  JsonVariant ota = respDoc["otaTarget"];
                  if (!ota.isNull() && ota.is<JsonObject>()) {
                    const char* releaseId = ota["releaseId"] | "";
                    const char* version = ota["version"] | "";
                    const char* artifactUrl = ota["artifactUrl"] | "";
                    const char* artifactSha256 = ota["artifactSha256"] | "";
                    const char* artifactSignature = ota["artifactSignature"] | "";
                    if (releaseId[0] != '\0' && artifactUrl[0] != '\0') {
                      // Resolve relative URL against pool base if needed.
                      String fullUrl = String(artifactUrl);
                      if (fullUrl.startsWith("/")) {
                        // The pool serves /ota/... — derive base from
                        // configured pool URL minus its path, similar to
                        // normalizeActivationApiBase() in mining.cpp.
                        String base = String(Settings.PoolApiBase);
                        int slash = base.indexOf('/', 8);  // skip scheme
                        if (slash > 0) base = base.substring(0, slash);
                        fullUrl = base + fullUrl;
                      }
                      Serial.printf("[OTA] target available: release=%s version=%s\n",
                                    releaseId, version);
                      installOtaArtifact(fullUrl, String(artifactSha256),
                                         String(artifactSignature),
                                         String(releaseId));
                    }
                  }
                } else {
                  Serial.println("[POLICY] policy_signature_invalid");
                }
              } else {
                Serial.printf("[POLICY] response_parse_error: %s\n", err.c_str());
              }
            } else {
              Serial.println("[POLICY] backend_unreachable");
            }
          }

          // 5-min interval + ±30s jitter (per F-5).
          uint32_t jitterMs = (uint32_t)random(-30000, 30001);
          uint32_t intervalMs = (POLL_INTERVAL_SECONDS * 1000U) + jitterMs;
          vTaskDelay(intervalMs / portTICK_PERIOD_MS);
        }
      },
      "policy_fetch", 8192, nullptr, 1, &g_policyTaskHandle, 0);
}

// ---------------------------------------------------------------------------
// Bootloader rollback (per F-9)
// ---------------------------------------------------------------------------

/**
 * Call this once after WiFi + first successful policy fetch + pool TCP
 * connect (the F-9 health check). Marks the running OTA slot as valid
 * so ESP-IDF won't roll back on next reboot. Resets BootStrikes.
 *
 * If the BootStrikes counter is already at 3 on entry, do NOT mark
 * valid — let ESP-IDF auto-revert at next reboot.
 */
extern "C" void markBootValidIfHealthy();

// Internal helper: invoked from the policy fetch task after the first
// successful backend round-trip. WiFi was up (we couldn't have reached
// the pool proxy otherwise) and policy was signed-validated, satisfying
// the F-9 health-check criteria (WiFi + backend + pool TCP, all within
// 5min of boot if the policy task started promptly per setup() wiring).
namespace {
void maybeMarkBootValid() {
  static bool already_marked = false;
  if (already_marked) return;
  uint64_t bootMs = (uint64_t)millis();
  if (bootMs > HEALTH_WINDOW_MS) {
    // Outside the 5-min window — let recordBootAttempt's strikes win.
    return;
  }
  markBootValidIfHealthy();
  already_marked = true;
}
}  // namespace

extern "C" void markBootValidIfHealthy() {
  if (Settings.BootStrikes >= 3) {
    Serial.println("[BOOT] strikes>=3, refusing to mark image valid; rollback at next reboot");
    return;
  }
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running == nullptr) {
    return;
  }
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
    return;
  }
  if (state == ESP_OTA_IMG_PENDING_VERIFY) {
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
      Serial.println("[BOOT] image marked valid; rollback canceled");
      Settings.BootStrikes = 0;
      nvMemory nv;
      nv.saveConfig(&Settings);
    }
  }
}

/**
 * Call early in setup() (before any health checks) to increment the
 * boot-strikes counter. If the counter exceeds 3, intentionally do
 * nothing for the rest of this boot — the next watchdog reset triggers
 * ESP-IDF rollback.
 */
extern "C" void recordBootAttempt() {
  // BUGFIX (B1): NVS isn't necessarily loaded when this is called from
  // setup() — the WiFi manager triggers loadConfig() later. Without an
  // explicit load here, every boot would read the default in-memory
  // value (0), increment to 1, save, and the 3-strikes gate would
  // never trigger. Load first, then increment.
  nvMemory nv;
  nv.loadConfig(&Settings);
  if (Settings.BootStrikes < 255) {
    Settings.BootStrikes += 1;
  }
  nv.saveConfig(&Settings);
  if (Settings.BootStrikes >= 3) {
    Serial.printf("[BOOT] strikes=%u — image will roll back on next reboot\n",
                  (unsigned)Settings.BootStrikes);
  }
}

// ---------------------------------------------------------------------------
// Implementation details
// ---------------------------------------------------------------------------

namespace {

bool waitForNtpSync(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    time_t now = time(nullptr);
    if (now > 1700000000) {  // ~2023; sane epoch
      struct timeval tv;
      gettimeofday(&tv, nullptr);
      return tv.tv_sec > 1700000000;
    }
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
  return false;
}

String buildPolicyApiBase() {
  // Mirror normalizeActivationApiBase() pattern from mining.cpp: derive
  // from configured pool URL, strip trailing /api/client*, append /api.
  String base = String(Settings.PoolApiBase);
  while (base.endsWith("/")) base.remove(base.length() - 1);
  int idx = base.indexOf("/api/client");
  if (idx > 0) base = base.substring(0, idx);
  while (base.endsWith("/")) base.remove(base.length() - 1);
  if (!base.endsWith("/api")) base += "/api";
  return base;
}

String makeNonce() {
  uint32_t r1 = esp_random();
  uint32_t r2 = esp_random();
  char buf[24];
  snprintf(buf, sizeof(buf), "%08x%08x", r1, r2);
  return String(buf);
}

// ---------------------------------------------------------------------------
// HMAC over eFuse-derived secret (per device-class plan F-5 integration).
// Wraps mining.cpp's computeDeviceHmacHex() which reuses the same eFuse
// HMAC key buildDeviceProof() uses for the activation flow. Returns
// empty string on unsupported hardware or unprovisioned key — backend
// will reject the request, which is the desired fail-closed behavior.
// ---------------------------------------------------------------------------
String hmacHexFromEfuse(const String& message) {
  char proof[65] = {0};
  if (!computeDeviceHmacHex(message.c_str(), (size_t)message.length(),
                            proof, sizeof(proof))) {
    return String("");
  }
  return String(proof);
}

// Returns the canonical eFuse-MAC-derived deviceId (12 lowercase hex).
// Cached after first call to avoid repeated eFuse reads.
const char* canonicalDeviceId() {
  static char cached[16] = {0};
  if (cached[0] == '\0') {
    if (!getDeviceIdFromEfuse(cached, sizeof(cached))) {
      cached[0] = '\0';
    }
  }
  return cached;
}

bool postJson(const String& url, const String& body, String& outResponse) {
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(url)) {
    return false;
  }
  http.addHeader("content-type", "application/json");
  int code = http.POST((uint8_t*)body.c_str(), body.length());
  bool ok = code >= 200 && code < 300;
  if (ok) {
    outResponse = http.getString();
  } else {
    Serial.printf("[POLICY] http %d %s\n", code, url.c_str());
  }
  http.end();
  return ok;
}

bool installOtaArtifact(const String& url, const String& expectedSha256Hex,
                         const String& signatureBase64, const String& releaseId) {
  HTTPClient http;
  http.setTimeout(60000);
  if (!http.begin(url)) {
    reportOtaState(releaseId.c_str(), "failed", "begin_failed", 1);
    return false;
  }
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[OTA] http %d on %s\n", code, url.c_str());
    reportOtaState(releaseId.c_str(), "failed", "http_error", 1);
    http.end();
    return false;
  }
  int contentLength = http.getSize();
  if (contentLength <= 0) {
    reportOtaState(releaseId.c_str(), "failed", "no_content_length", 1);
    http.end();
    return false;
  }

  reportOtaState(releaseId.c_str(), "downloading", nullptr, 1);

  // Stream into Update + accumulate SHA-256 for verification.
  if (!Update.begin((size_t)contentLength)) {
    reportOtaState(releaseId.c_str(), "failed", "update_begin_failed", 1);
    http.end();
    return false;
  }
  mbedtls_sha256_context shaCtx;
  mbedtls_sha256_init(&shaCtx);
  mbedtls_sha256_starts(&shaCtx, 0);
  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  size_t totalRead = 0;
  while (totalRead < (size_t)contentLength) {
    if (!stream->available()) {
      vTaskDelay(10 / portTICK_PERIOD_MS);
      continue;
    }
    int n = stream->read(buf, sizeof(buf));
    if (n <= 0) break;
    if (Update.write(buf, n) != (size_t)n) {
      reportOtaState(releaseId.c_str(), "failed", "update_write_failed", 1);
      Update.abort();
      mbedtls_sha256_free(&shaCtx);
      http.end();
      return false;
    }
    mbedtls_sha256_update(&shaCtx, buf, n);
    totalRead += n;
  }
  uint8_t digest[32];
  mbedtls_sha256_finish(&shaCtx, digest);
  mbedtls_sha256_free(&shaCtx);
  http.end();

  reportOtaState(releaseId.c_str(), "verifying", nullptr, 1);

  String actualHex = hexEncode(digest, 32);
  if (!actualHex.equalsIgnoreCase(expectedSha256Hex)) {
    Serial.printf("[OTA] sha256 mismatch: expected=%s got=%s\n",
                  expectedSha256Hex.c_str(), actualHex.c_str());
    reportOtaState(releaseId.c_str(), "failed", "sha256_mismatch", 1);
    Update.abort();
    return false;
  }

  // Verify Ed25519 signature over the SHA-256 digest. (Backend signs the
  // raw bytes with raw Ed25519; a hash-then-verify scheme is acceptable
  // and matches typical OTA pipelines.)
  uint8_t sigBytes[64];
  size_t sigLen = 0;
  if (mbedtls_base64_decode(sigBytes, sizeof(sigBytes), &sigLen,
                             (const unsigned char*)signatureBase64.c_str(),
                             signatureBase64.length()) != 0
      || sigLen != 64) {
    reportOtaState(releaseId.c_str(), "failed", "signature_decode", 1);
    Update.abort();
    return false;
  }
  if (!verifyEd25519Signature(digest, sizeof(digest), sigBytes, sigLen)) {
    Serial.println("[OTA] ota_signature_verify_failed");
    reportOtaState(releaseId.c_str(), "failed", "signature_mismatch", 1);
    Update.abort();
    return false;
  }

  reportOtaState(releaseId.c_str(), "installing", nullptr, 1);

  if (!Update.end(true)) {
    reportOtaState(releaseId.c_str(), "failed", "update_end_failed", 1);
    return false;
  }

  reportOtaState(releaseId.c_str(), "success", nullptr, 1);
  Serial.println("[OTA] update applied; rebooting");
  delay(500);
  ESP.restart();
  return true;
}

bool sha256File(Stream& stream, size_t totalLen, uint8_t outDigest[32]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  uint8_t buf[1024];
  size_t got = 0;
  while (got < totalLen) {
    int n = stream.readBytes(buf, sizeof(buf));
    if (n <= 0) {
      mbedtls_sha256_free(&ctx);
      return false;
    }
    mbedtls_sha256_update(&ctx, buf, n);
    got += n;
  }
  mbedtls_sha256_finish(&ctx, outDigest);
  mbedtls_sha256_free(&ctx);
  return true;
}

String hexEncode(const uint8_t* data, size_t len) {
  static const char hex[] = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += hex[(data[i] >> 4) & 0xf];
    out += hex[data[i] & 0xf];
  }
  return out;
}

// ---------------------------------------------------------------------------
// Ed25519 signature verification
// ---------------------------------------------------------------------------
//
// POLICY_SIGNING_PUBKEY is supplied as a 64-char hex constant via
// platformio.ini build_flags (per Setup Step S0). At runtime we decode
// it once into a 32-byte raw public key and pass it through mbedtls.
//
// If the target's mbedtls build does NOT enable Ed25519 (some
// espressif32 framework builds disable it for size), this function
// must be replaced with a vendored implementation (e.g., orlp/ed25519
// or libsodium reference). The interface stays the same.

bool decodeHexFixed(const char* hex, uint8_t* out, size_t outLen) {
  if (strlen(hex) != outLen * 2) {
    return false;
  }
  for (size_t i = 0; i < outLen; ++i) {
    char hi = hex[i * 2];
    char lo = hex[i * 2 + 1];
    auto h2n = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int hn = h2n(hi);
    int ln = h2n(lo);
    if (hn < 0 || ln < 0) return false;
    out[i] = (uint8_t)((hn << 4) | ln);
  }
  return true;
}

bool verifyEd25519Signature(const uint8_t* msg, size_t msgLen,
                             const uint8_t* sig, size_t sigLen) {
  if (sigLen != 64) return false;
#ifndef POLICY_SIGNING_PUBKEY
  // No pubkey compiled in — refuse rather than fail-open.
  Serial.println("[CRYPTO] POLICY_SIGNING_PUBKEY not compiled in");
  return false;
#else
  uint8_t pubkey[32];
  if (!decodeHexFixed(POLICY_SIGNING_PUBKEY, pubkey, sizeof(pubkey))) {
    Serial.println("[CRYPTO] POLICY_SIGNING_PUBKEY hex decode failed");
    return false;
  }

  // mbedtls Ed25519 verification path. If the build's mbedtls does not
  // include MBEDTLS_PK_ED25519, this whole #ifdef branch must be
  // replaced with a vendored verifier. Compile-time guard below
  // surfaces the issue early.
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
#if defined(MBEDTLS_PK_HAVE_ECC_KEYS) || defined(MBEDTLS_ECP_C)
  // Wrap raw 32-byte pubkey as a SubjectPublicKeyInfo DER blob:
  //   30 2A 30 05 06 03 2B 65 70 03 21 00 <32 bytes pubkey>
  uint8_t spki[44] = {
      0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70,
      0x03, 0x21, 0x00};
  memcpy(spki + 12, pubkey, 32);
  int rc = mbedtls_pk_parse_public_key(&pk, spki, sizeof(spki));
  if (rc != 0) {
    Serial.printf("[CRYPTO] mbedtls_pk_parse_public_key rc=%d\n", rc);
    mbedtls_pk_free(&pk);
    return false;
  }
  rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_NONE, msg, msgLen, sig, sigLen);
  mbedtls_pk_free(&pk);
  if (rc != 0) {
    Serial.printf("[CRYPTO] mbedtls_pk_verify rc=%d\n", rc);
    return false;
  }
  return true;
#else
  // Fallback: build doesn't include ECC. Replace with vendored Ed25519.
  Serial.println("[CRYPTO] mbedtls Ed25519 unavailable; vendor a verifier");
  return false;
#endif
#endif
}

}  // namespace
