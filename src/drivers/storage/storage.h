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
#define DEFAULT_POOLURL		"stratum+tcp://stratum.hcash-dev.network:3333"
#define DEFAULT_POOL_API_BASE	""
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
};

#endif // _STORAGE_H_
