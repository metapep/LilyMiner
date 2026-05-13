#ifndef MONITOR_API_H
#define MONITOR_API_H

#include <Arduino.h>

// Monitor states
#define SCREEN_MINING   0
#define SCREEN_CLOCK    1
#define SCREEN_GLOBAL   2
#define NO_SCREEN       3   //Used when board has no TFT

//Time update period
#define UPDATE_PERIOD_h   5

//API BTC price (Update to USDT cus it's more liquidity and flow price updade)   

//#define getBTCAPI "https://api.coindesk.com/v1/bpi/currentprice.json" -- doesn't work anymore
//#define getBTCAPI "https://api.blockchain.com/v3/exchange/tickers/BTC-USDT" -- updates infrequently
#define getBTCAPI "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd"

#define UPDATE_BTC_min   1

//API Block height
#define getHeightAPI "https://mempool.space/api/blocks/tip/height"
#define UPDATE_Height_min 2

//APIs Global Stats
#define getGlobalHash "https://mempool.space/api/v1/mining/hashrate/3d"
#define getDifficulty "https://mempool.space/api/v1/difficulty-adjustment"
#define getFees "https://mempool.space/api/v1/fees/recommended"
#define UPDATE_Global_min 2

//API public-pool.io
// https://public-pool.io:40557/api/client/btcString
#define getPublicPool "https://public-pool.io:40557/api/client/" // +btcString
#define UPDATE_POOL_min   1

#define NEXT_HALVING_EVENT 1050000 //840000
#define HALVING_BLOCKS 210000

enum NMState {
  NM_waitingConfig,
  NM_waitingActivation,
  NM_Connecting,
  NM_hashing
};

typedef struct{
  uint8_t screen;
  bool rotation;
  NMState NerdStatus;
}monitor_data;

typedef struct{
  String globalHash; //hexahashes
  String currentBlock;
  String difficulty;
  String blocksHalving;
  float progressPercent;
  int remainingBlocks;
  int halfHourFee;
#ifdef NERDMINER_T_HMI
  int fastestFee;
  int hourFee;
  int economyFee;
  int minimumFee;
#endif
}global_data;

typedef struct {
  String completedShares; // accepted shares
  String totalMHashes;
  String totalKHashes;
  String currentHashRate;
  String templates;
  String bestDiff;
  String timeMining;
  String valids;
  String temp;
  String currentTime;
  // Per device-class plan F-10. classId is the policy-derived class
  // label (e.g., "J50"); classCapKHs is the human-readable cap label
  // (e.g., "50 KH/s"). Empty when no policy has been fetched yet (cold
  // boot pre-first-fetch). Display drivers should render these next to
  // currentHashRate without swapping skins/wallpapers.
  String classId;
  String classCapKHs;
}mining_data;

typedef struct {
  String completedShares; // accepted shares
  String totalKHashes;
  String currentHashRate;
  String btcPrice;
  String blockHeight;
  String currentTime;
  String currentDate;
  // Per device-class plan F-10: same class label fields as mining_data
  // so display drivers can render the class tag from any screen, not
  // just the main mining screen. Empty string when no policy is cached.
  String classId;
  String classCapKHs;
}clock_data;

typedef struct {
  String currentHashRate;
  String valids;
  unsigned long currentHours;
  unsigned long currentMinutes;
  unsigned long currentSeconds;
}clock_data_t;

typedef struct {
  String completedShares; // accepted shares
  String totalKHashes;
  String currentHashRate;
  String btcPrice;
  String currentTime;
  String halfHourFee;
#ifdef NERDMINER_T_HMI
  String hourFee;
  String fastestFee;
  String economyFee;
  String minimumFee;
#endif
  String netwrokDifficulty;
  String globalHashRate;
  String blockHeight;
  float progressPercent;
  String remainingBlocks;
  // Per device-class plan F-10: class label fields for non-mining screens.
  String classId;
  String classCapKHs;
}coin_data;

typedef struct{
  int workersCount;       // Workers count, how many nerdminers using your address
  String workersHash;     // Workers Total Hash Rate
  String bestDifficulty;  // Your miners best difficulty
}pool_data;

void setup_monitor(void);

mining_data getMiningData(unsigned long mElapsed);
clock_data getClockData(unsigned long mElapsed);
coin_data getCoinData(unsigned long mElapsed);
pool_data getPoolData(void);

clock_data_t getClockData_t(unsigned long mElapsed);
String getPoolAPIUrl(void);

#endif //MONITOR_API_H
