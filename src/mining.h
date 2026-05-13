
#ifndef MINING_API_H
#define MINING_API_H

// Mining
#define MAX_NONCE_STEP  5000000U
#define MAX_NONCE       25000000U
#define TARGET_NONCE    471136297U
#define DEFAULT_DIFFICULTY  0.00015
#define KEEPALIVE_TIME_ms       30000
#define POOLINACTIVITY_TIME_ms  60000

//#if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)
#define HARDWARE_SHA265
//#endif

#define TARGET_BUFFER_SIZE 64

void runMonitor(void *name);

void runStratumWorker(void *name);
void runMiner(void *name);

void minerWorkerSw(void * task_id);
void minerWorkerHw(void * task_id);

String printLocalTime(void);

void resetStat();

// Per device-class plan F-5 / F-8: shared eFuse helpers, exposed for the
// policy/OTA client (drivers/devicePolicy.cpp). Implementations remain
// in mining.cpp where the eFuse + HMAC plumbing lives.

// Lowercase 12-char hex of the eFuse base MAC. outSize must be >=13.
// Returns true on success.
bool getDeviceIdFromEfuse(char* output, size_t outSize);

// HMAC-SHA256 over an arbitrary UTF-8 message using the eFuse-provisioned
// HMAC key (same key buildDeviceProof uses). Output is lowercase hex
// (64 chars + null terminator); outHexSize must be >= 65.
// Returns false when HCASH_DEVICE_AUTH_SUPPORTED == 0 or the eFuse key
// is not provisioned.
bool computeDeviceHmacHex(const char* message, size_t messageLen,
                          char* outHex, size_t outHexSize);

typedef struct{
  uint8_t bytearray_target[32];
  uint8_t bytearray_pooltarget[32];
  uint8_t merkle_result[32];
  uint8_t bytearray_blockheader[128];
} miner_data;


#endif // UTILS_API_H