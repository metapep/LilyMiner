#ifndef _STORAGE_H_
#define _STORAGE_H_

#include <Arduino.h>

// config files

// default settings
#ifndef HAN
#define DEFAULT_AP_SSID		"JLANT"
#define DEFAULT_WIFI_SSID	"JREAD"
#else
#define DEFAULT_AP_SSID		"JLANT"
#define DEFAULT_WIFI_SSID	"JREAD"
#endif
#define DEFAULT_AP_WIFIPW	"JELLOJLANT"
#define DEFAULT_WIFI_WIFIPW	"JELLOJREAD"
#define DEFAULT_POOLURL		"stratum+tcp://stratum.hashcash-test.network:3333"
#define DEFAULT_POOL_API_BASE	"http://stratum.hashcash-test.network:3334"
#define DEFAULT_POOLPASS	"x"
#define DEFAULT_WALLETID	"hcash1qch57r3rsh2wcy0dr8t0s8ehvm33e20cjyhpy3h"
#define DEFAULT_OWNER_WALLET_EVM	""
#define DEFAULT_ACTIVATION_STATE	"unclaimed"
#define DEFAULT_POOLPORT	3333
#define DEFAULT_TIMEZONE	2
#define DEFAULT_SAVESTATS	false
#define DEFAULT_INVERTCOLORS	false
#define DEFAULT_BRIGHTNESS	250

// JSON config files
#define JSON_CONFIG_FILE	"/config.json"

// JSON config file SD card (for user interaction, readme.md)
#define JSON_KEY_SSID		"SSID"
#define JSON_KEY_PASW		"WifiPW"
#define JSON_KEY_POOLURL	"PoolUrl"
#define JSON_KEY_POOLAPIBASE	"PoolApiBase"
#define JSON_KEY_POOLPASS	"PoolPassword"
#define JSON_KEY_WALLETID	"BtcWallet"
#define JSON_KEY_PAYOUT_WALLET_HCASH	"PayoutWalletHcash"
#define JSON_KEY_OWNER_WALLET_EVM	"OwnerWalletEvm"
#define JSON_KEY_ACTIVATION_STATE	"ActivationState"
#define JSON_KEY_ACTIVATION_CODE	"ActivationCode"
#define JSON_KEY_ACTIVATION_CODE_EXPIRES_AT	"ActivationCodeExpiresAt"
#define JSON_KEY_ACTIVATION_LAST_CHECK_AT	"ActivationLastCheckAt"
#define JSON_KEY_POOLPORT	"PoolPort"
#define JSON_KEY_TIMEZONE	"Timezone"
#define JSON_KEY_STATS2NV	"SaveStats"
#define JSON_KEY_INVCOLOR	"invertColors"
#define JSON_KEY_BRIGHTNESS	"Brightness"

// JSON config file SPIFFS (different for backward compatibility with existing devices)
#define JSON_SPIFFS_KEY_POOLURL		"poolString"
#define JSON_SPIFFS_KEY_POOLAPIBASE	"poolApiBase"
#define JSON_SPIFFS_KEY_POOLPORT	"portNumber"
#define JSON_SPIFFS_KEY_POOLPASS	"poolPassword"
#define JSON_SPIFFS_KEY_WALLETID	"btcString"
#define JSON_SPIFFS_KEY_PAYOUT_WALLET_HCASH	"payoutWalletHcash"
#define JSON_SPIFFS_KEY_OWNER_WALLET_EVM	"ownerWalletEvm"
#define JSON_SPIFFS_KEY_ACTIVATION_STATE	"activationState"
#define JSON_SPIFFS_KEY_ACTIVATION_CODE	"activationCode"
#define JSON_SPIFFS_KEY_ACTIVATION_CODE_EXPIRES_AT	"activationCodeExpiresAt"
#define JSON_SPIFFS_KEY_ACTIVATION_LAST_CHECK_AT	"activationLastCheckAt"
#define JSON_SPIFFS_KEY_TIMEZONE	"gmtZone"
#define JSON_SPIFFS_KEY_STATS2NV	"saveStatsToNVS"
#define JSON_SPIFFS_KEY_INVCOLOR	"invertColors"
#define JSON_SPIFFS_KEY_BRIGHTNESS	"Brightness"
// Per device-class plan F-2 / F-3 / F-9: policy cache + boot-strikes counter.
// Persisted alongside activation state in the same NVS layer (nvMemory.cpp).
#define JSON_SPIFFS_KEY_POLICY_CLASS_ID				"policyClassId"
#define JSON_SPIFFS_KEY_POLICY_TARGET_HASHRATE_HS	"policyTargetHashrateHs"
#define JSON_SPIFFS_KEY_POLICY_FETCHED_AT			"policyFetchedAt"
#define JSON_SPIFFS_KEY_POLICY_PREV_CLASS_ID		"policyPrevClassId"
#define JSON_SPIFFS_KEY_BOOT_STRIKES				"bootStrikes"

// Device-class plan constants (per F-2, F-6, audit fix #4).
// Hardcoded so the disconnected-mode TTL fallback never depends on the
// policy cache or class table existing.
#ifndef SAFEST_CAP_HS
#define SAFEST_CAP_HS 5000U
#endif
#ifndef POLICY_TTL_SECONDS
#define POLICY_TTL_SECONDS 3600U
#endif
#ifndef POLL_INTERVAL_SECONDS
#define POLL_INTERVAL_SECONDS 300U
#endif
// BOARD_ID is set as a build flag in platformio.ini for production builds
// (per Setup Step S0). Provide a dev-only default so non-production envs
// still compile.
#ifndef BOARD_ID
#define BOARD_ID "hashcash_nano_v1"
#endif
// NTP_SERVER pinned per device-class plan F-4 / audit fix #8.
#ifndef NTP_SERVER
#define NTP_SERVER "time.cloudflare.com"
#endif

// settings
struct TSettings
{
	String WifiSSID{ DEFAULT_WIFI_SSID };
	String WifiPW{ DEFAULT_WIFI_WIFIPW };
	String PoolAddress{ DEFAULT_POOLURL };
	char PoolApiBase[120]{ DEFAULT_POOL_API_BASE };
	char BtcWallet[80]{ DEFAULT_WALLETID }; // legacy mirror of payout wallet
	char PayoutWalletHcash[80]{ DEFAULT_WALLETID };
	char OwnerWalletEvm[64]{ DEFAULT_OWNER_WALLET_EVM };
	char ActivationState[24]{ DEFAULT_ACTIVATION_STATE };
	char ActivationCode[16]{ "" };
	uint64_t ActivationCodeExpiresAt{ 0 };
	uint64_t ActivationLastCheckAt{ 0 };
	char PoolPassword[80]{ DEFAULT_POOLPASS };
	int PoolPort{ DEFAULT_POOLPORT };
	int Timezone{ DEFAULT_TIMEZONE };
	bool saveStats{ DEFAULT_SAVESTATS };
	bool invertColors{ DEFAULT_INVERTCOLORS };
	int Brightness{ DEFAULT_BRIGHTNESS };
	// Per device-class plan F-2 / F-3 / F-5: policy cache fields. Sibling
	// of activation fields above; persisted via the same NVS layer.
	char PolicyClassId[12]{ "" };
	uint32_t PolicyTargetHashrateHs{ SAFEST_CAP_HS };
	uint64_t PolicyFetchedAt{ 0 };
	char PolicyPrevClassId[12]{ "" };
	// Per device-class plan F-9: 3-strikes counter for ESP-IDF rollback.
	// Reset on factory image install (NVS wipe). Survives normal reboots.
	uint8_t BootStrikes{ 0 };
};

#endif // _STORAGE_H_
