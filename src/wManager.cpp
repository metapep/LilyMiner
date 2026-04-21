#define ESP_DRD_USE_SPIFFS true

// Include Libraries
//#include ".h"

#include <WiFi.h>

#include <WiFiManager.h>

#include "wManager.h"
#include "monitor.h"
#include "drivers/displays/display.h"
#include "drivers/storage/SDCard.h"
#include "drivers/storage/nvMemory.h"
#include "drivers/storage/storage.h"
#include "mining.h"
#include "timeconst.h"

#include <ArduinoJson.h>
#include <esp_flash.h>


// Flag for saving data
bool shouldSaveConfig = false;

// Variables to hold data from custom textboxes
TSettings Settings;

// Define WiFiManager Object
WiFiManager wm;
extern monitor_data mMonitor;

nvMemory nvMem;

extern SDCard SDCrd;

static int parsePoolPort(const String& value)
{
    if (value.length() == 0) {
        return 0;
    }

    for (unsigned int i = 0; i < value.length(); ++i) {
        const char c = value.charAt(i);
        if (c < '0' || c > '9') {
            return 0;
        }
    }

    const long port = value.toInt();
    if (port < 1 || port > 65535) {
        return 0;
    }

    return (int)port;
}

static void normalizePoolEndpoint(TSettings* settings)
{
    String endpoint = settings->PoolAddress;
    endpoint.trim();
    if (endpoint.length() == 0) {
        return;
    }

    const int schemeIdx = endpoint.indexOf("://");
    if (schemeIdx >= 0) {
        endpoint = endpoint.substring(schemeIdx + 3);
    }

    const int authIdx = endpoint.lastIndexOf('@');
    if (authIdx >= 0) {
        endpoint = endpoint.substring(authIdx + 1);
    }

    int stopIdx = endpoint.indexOf('/');
    if (stopIdx >= 0) {
        endpoint = endpoint.substring(0, stopIdx);
    }
    stopIdx = endpoint.indexOf('?');
    if (stopIdx >= 0) {
        endpoint = endpoint.substring(0, stopIdx);
    }
    stopIdx = endpoint.indexOf('#');
    if (stopIdx >= 0) {
        endpoint = endpoint.substring(0, stopIdx);
    }

    endpoint.trim();
    if (endpoint.length() == 0) {
        return;
    }

    int parsedPort = 0;
    if (endpoint.startsWith("[")) {
        const int closeIdx = endpoint.indexOf(']');
        if (closeIdx > 1) {
            String host = endpoint.substring(1, closeIdx);
            if (closeIdx + 1 < endpoint.length() && endpoint.charAt(closeIdx + 1) == ':') {
                parsedPort = parsePoolPort(endpoint.substring(closeIdx + 2));
            }
            endpoint = host;
        }
    } else {
        const int firstColon = endpoint.indexOf(':');
        const int lastColon = endpoint.lastIndexOf(':');
        if (firstColon > 0 && firstColon == lastColon) {
            const int maybePort = parsePoolPort(endpoint.substring(firstColon + 1));
            if (maybePort > 0) {
                parsedPort = maybePort;
                endpoint = endpoint.substring(0, firstColon);
            }
        }
    }

    endpoint.trim();
    if (endpoint.length() > 0) {
        settings->PoolAddress = endpoint;
    }
    if (parsedPort > 0) {
        settings->PoolPort = parsedPort;
    }
}

String readCustomAPName() {
    Serial.println("DEBUG: Attempting to read custom AP name from flash at 0x3F0000...");
    
    // Leer directamente desde flash
    const size_t DATA_SIZE = 128;
    uint8_t buffer[DATA_SIZE];
    memset(buffer, 0, DATA_SIZE); // Clear buffer
    
    // Leer desde 0x3F0000
    esp_err_t result = esp_flash_read(NULL, buffer, 0x3F0000, DATA_SIZE);
    if (result != ESP_OK) {
        Serial.printf("DEBUG: Flash read error: %s\n", esp_err_to_name(result));
        return "";
    }
    
    Serial.println("DEBUG: Successfully read from flash");
    String data = String((char*)buffer);
    
    // Debug: show raw data read
    Serial.printf("DEBUG: Raw flash data: '%s'\n", data.c_str());
    
    if (data.startsWith("WEBFLASHER_CONFIG:")) {
        Serial.println("DEBUG: Found WEBFLASHER_CONFIG marker");
        String jsonPart = data.substring(18); // Después del marcador "WEBFLASHER_CONFIG:"
        
        Serial.printf("DEBUG: JSON part: '%s'\n", jsonPart.c_str());
        
        DynamicJsonDocument doc(256);
        DeserializationError error = deserializeJson(doc, jsonPart);
        
        if (error == DeserializationError::Ok) {
            Serial.println("DEBUG: JSON parsed successfully");
            
            if (doc.containsKey("apname")) {
                String customAP = doc["apname"].as<String>();
                customAP.trim();
                
                if (customAP.length() > 0 && customAP.length() < 32) {
                    Serial.printf("✅ Custom AP name from webflasher: %s\n", customAP.c_str());
                    return customAP;
                } else {
                    Serial.printf("DEBUG: AP name invalid length: %d\n", customAP.length());
                }
            } else {
                Serial.println("DEBUG: 'apname' key not found in JSON");
            }
        } else {
            Serial.printf("DEBUG: JSON parse error: %s\n", error.c_str());
        }
    } else {
        Serial.println("DEBUG: WEBFLASHER_CONFIG marker not found - no custom config");
    }
    
    Serial.println("DEBUG: Using default AP name");
    return "";
}

void saveConfigCallback()
// Callback notifying us of the need to save configuration
{
    Serial.println("Should save config");
    shouldSaveConfig = true;    
    //wm.setConfigPortalBlocking(false);
}

/* void saveParamsCallback()
// Callback notifying us of the need to save configuration
{
    Serial.println("Should save config");
    shouldSaveConfig = true;
    nvMem.saveConfig(&Settings);
} */

void configModeCallback(WiFiManager* myWiFiManager)
// Called when config mode launched
{
    Serial.println("Entered Configuration Mode");
    drawSetupScreen();
    Serial.print("Config SSID: ");
    Serial.println(myWiFiManager->getConfigPortalSSID());

    Serial.print("Config IP Address: ");
    Serial.println(WiFi.softAPIP());
}

void reset_configuration()
{
    Serial.println("Erasing Config, restarting");
    nvMem.deleteConfig();
    resetStat();
    wm.resetSettings();
    ESP.restart();
}

void init_WifiManager()
{
#ifdef MONITOR_SPEED
    Serial.begin(MONITOR_SPEED);
#else
    Serial.begin(115200);
#endif //MONITOR_SPEED
    //Serial.setTxTimeoutMs(10);
    
    // Check for custom AP name from flasher config, otherwise use default
    String customAPName = readCustomAPName();
    const char* apName = customAPName.length() > 0 ? customAPName.c_str() : DEFAULT_AP_SSID;

    //Init pin 15 to eneble 5V external power (LilyGo bug)
#ifdef PIN_ENABLE5V
    pinMode(PIN_ENABLE5V, OUTPUT);
    digitalWrite(PIN_ENABLE5V, HIGH);
#endif

    // Change to true when testing to force configuration every time we run
    bool forceConfig = false;

#if defined(PIN_BUTTON_2)
    // Check if button2 is pressed to enter configMode with actual configuration
    if (!digitalRead(PIN_BUTTON_2)) {
        Serial.println(F("Button pressed to force start config mode"));
        forceConfig = true;
        wm.setBreakAfterConfig(true); //Set to detect config edition and save
    }
#endif
    // Explicitly set WiFi mode
    WiFi.mode(WIFI_STA);

    if (!nvMem.loadConfig(&Settings))
    {
        //No config file on internal flash.
        if (SDCrd.loadConfigFile(&Settings))
        {
            //Config file on SD card.
            SDCrd.SD2nvMemory(&nvMem, &Settings); // reboot on success.          
        }
        else
        {
            //No config file on SD card. Starting wifi config server.
            forceConfig = true;
        }
    };
    
    // Free the memory from SDCard class 
    SDCrd.terminate();
    
    // Accept PoolUrl formats such as stratum+tcp://host:port
    normalizePoolEndpoint(&Settings);
    
    // Reset settings (only for development)
    //wm.resetSettings();

    //Set dark theme
    //wm.setClass("invert"); // dark theme

    // Set config save notify callback
    wm.setSaveConfigCallback(saveConfigCallback);
    wm.setSaveParamsCallback(saveConfigCallback);

    // Set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
    wm.setAPCallback(configModeCallback);    

    //Advanced settings
    wm.setConfigPortalBlocking(false); //Hacemos que el portal no bloquee el firmware
    wm.setConnectTimeout(40); // how long to try to connect for before continuing
    wm.setConfigPortalTimeout(180); // auto close configportal after n seconds
    // wm.setCaptivePortalEnable(false); // disable captive portal redirection
    // wm.setAPClientCheck(true); // avoid timeout if client connected to softap
    //wm.setTimeout(120);
    //wm.setConfigPortalTimeout(120); //seconds

    // Custom elements

    // Text box (String) - 80 characters maximum
    WiFiManagerParameter pool_text_box("Poolurl", "Pool url", Settings.PoolAddress.c_str(), 80);

    // Need to convert numerical input to string to display the default value.
    char convertedValue[6];
    sprintf(convertedValue, "%d", Settings.PoolPort);

    // Text box (Number) - 7 characters maximum
    WiFiManagerParameter port_text_box_num("Poolport", "Pool port", convertedValue, 7);

    // Optional explicit API base for worker stats endpoint (e.g. http://host:3334/api/client/)
    WiFiManagerParameter pool_api_text_box("PoolApiBase", "Pool API base (optional)", Settings.PoolApiBase, 120);

    // Text box (String) - 80 characters maximum
    //WiFiManagerParameter password_text_box("Poolpassword", "Pool password (Optional)", Settings.PoolPassword, 80);

    // Text box (String) - 80 characters maximum
    WiFiManagerParameter addr_text_box("minerPayoutWallet", "Your payout wallet (HCASH)", Settings.PayoutWalletHcash, 80);
    WiFiManagerParameter owner_wallet_text_box("ownerWalletEvm", "Owner wallet (EVM for staking)", Settings.OwnerWalletEvm, 64);

  // Text box (Number) - 2 characters maximum
  char charZone[6];
  sprintf(charZone, "%d", Settings.Timezone);
  WiFiManagerParameter time_text_box_num("TimeZone", "TimeZone fromUTC (-12/+12)", charZone, 3);

  WiFiManagerParameter features_html("<hr><br><label style=\"font-weight: bold;margin-bottom: 25px;display: inline-block;\">Features</label>");

  char checkboxParams[24] = "type=\"checkbox\"";
  if (Settings.saveStats)
  {
    strcat(checkboxParams, " checked");
  }
  WiFiManagerParameter save_stats_to_nvs("SaveStatsToNVS", "Save mining statistics to flash memory.", "T", 2, checkboxParams, WFM_LABEL_AFTER);
  // Text box (String) - 80 characters maximum
  WiFiManagerParameter password_text_box("PoolpasswordOptional", "Pool password", Settings.PoolPassword, 80);

  // Add all defined parameters
  wm.addParameter(&pool_text_box);
  wm.addParameter(&port_text_box_num);
  wm.addParameter(&pool_api_text_box);
  wm.addParameter(&password_text_box);
  wm.addParameter(&addr_text_box);
  wm.addParameter(&owner_wallet_text_box);
  wm.addParameter(&time_text_box_num);
  wm.addParameter(&features_html);
  wm.addParameter(&save_stats_to_nvs);
  #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
  char checkboxParams2[24] = "type=\"checkbox\"";
  if (Settings.invertColors)
  {
    strcat(checkboxParams2, " checked");
  }
  WiFiManagerParameter invertColors("inverColors", "Invert Display Colors (if the colors looks weird)", "T", 2, checkboxParams2, WFM_LABEL_AFTER);
  wm.addParameter(&invertColors);
  #endif
  #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
    char brightnessConvValue[2];
    sprintf(brightnessConvValue, "%d", Settings.Brightness);
    // Text box (Number) - 3 characters maximum
    WiFiManagerParameter brightness_text_box_num("Brightness", "Screen backlight Duty Cycle (0-255)", brightnessConvValue, 3);
    wm.addParameter(&brightness_text_box_num);
  #endif

    Serial.println("AllDone: ");
    if (forceConfig)    
    {
        // Run if we need a configuration
        //No configuramos timeout al modulo
        wm.setConfigPortalBlocking(true); //Hacemos que el portal SI bloquee el firmware
        drawSetupScreen();
        mMonitor.NerdStatus = NM_Connecting;
        wm.startConfigPortal(apName, DEFAULT_AP_WIFIPW);

        if (shouldSaveConfig)
        {
            //Could be break forced after edditing, so save new config
            Serial.println("failed to connect and hit timeout");
            Settings.PoolAddress = pool_text_box.getValue();
            Settings.PoolPort = atoi(port_text_box_num.getValue());
            normalizePoolEndpoint(&Settings);
            strncpy(Settings.PoolApiBase, pool_api_text_box.getValue(), sizeof(Settings.PoolApiBase));
            strncpy(Settings.PoolPassword, password_text_box.getValue(), sizeof(Settings.PoolPassword));
            strncpy(Settings.PayoutWalletHcash, addr_text_box.getValue(), sizeof(Settings.PayoutWalletHcash));
            Settings.PayoutWalletHcash[sizeof(Settings.PayoutWalletHcash) - 1] = '\0';
            strncpy(Settings.BtcWallet, Settings.PayoutWalletHcash, sizeof(Settings.BtcWallet));
            Settings.BtcWallet[sizeof(Settings.BtcWallet) - 1] = '\0';
            strncpy(Settings.OwnerWalletEvm, owner_wallet_text_box.getValue(), sizeof(Settings.OwnerWalletEvm));
            Settings.OwnerWalletEvm[sizeof(Settings.OwnerWalletEvm) - 1] = '\0';
            Settings.Timezone = atoi(time_text_box_num.getValue());
            //Serial.println(save_stats_to_nvs.getValue());
            Settings.saveStats = (strncmp(save_stats_to_nvs.getValue(), "T", 1) == 0);
            #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
                Settings.invertColors = (strncmp(invertColors.getValue(), "T", 1) == 0);
            #endif
            #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
                Settings.Brightness = atoi(brightness_text_box_num.getValue());
            #endif
            nvMem.saveConfig(&Settings);
            delay(3*SECOND_MS);
            //reset and try again, or maybe put it to deep sleep
            ESP.restart();            
        };
    }
    else
    {
        //Tratamos de conectar con la configuración inicial ya almacenada
        mMonitor.NerdStatus = NM_Connecting;
        // disable captive portal redirection
        wm.setCaptivePortalEnable(true); 
        wm.setConfigPortalBlocking(true);
        wm.setEnableConfigPortal(true);
        // if (!wm.autoConnect(Settings.WifiSSID.c_str(), Settings.WifiPW.c_str()))
        if (!wm.autoConnect(apName, DEFAULT_AP_WIFIPW))
        {
            Serial.println("Failed to connect to configured WIFI, and hit timeout");
            if (shouldSaveConfig) {
                // Save new config            
                Settings.PoolAddress = pool_text_box.getValue();
                Settings.PoolPort = atoi(port_text_box_num.getValue());
                normalizePoolEndpoint(&Settings);
                strncpy(Settings.PoolApiBase, pool_api_text_box.getValue(), sizeof(Settings.PoolApiBase));
                strncpy(Settings.PoolPassword, password_text_box.getValue(), sizeof(Settings.PoolPassword));
                strncpy(Settings.PayoutWalletHcash, addr_text_box.getValue(), sizeof(Settings.PayoutWalletHcash));
            Settings.PayoutWalletHcash[sizeof(Settings.PayoutWalletHcash) - 1] = '\0';
            strncpy(Settings.BtcWallet, Settings.PayoutWalletHcash, sizeof(Settings.BtcWallet));
            Settings.BtcWallet[sizeof(Settings.BtcWallet) - 1] = '\0';
            strncpy(Settings.OwnerWalletEvm, owner_wallet_text_box.getValue(), sizeof(Settings.OwnerWalletEvm));
            Settings.OwnerWalletEvm[sizeof(Settings.OwnerWalletEvm) - 1] = '\0';
                Settings.Timezone = atoi(time_text_box_num.getValue());
                // Serial.println(save_stats_to_nvs.getValue());
                Settings.saveStats = (strncmp(save_stats_to_nvs.getValue(), "T", 1) == 0);
                #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
                Settings.invertColors = (strncmp(invertColors.getValue(), "T", 1) == 0);
                #endif
                #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
                Settings.Brightness = atoi(brightness_text_box_num.getValue());
                #endif
                nvMem.saveConfig(&Settings);
                vTaskDelay(2000 / portTICK_PERIOD_MS);      
            }        
            ESP.restart();                            
        } 
    }
    
    //Conectado a la red Wifi
    if (WiFi.status() == WL_CONNECTED) {
        //tft.pushImage(0, 0, MinerWidth, MinerHeight, MinerScreen);
        Serial.println("");
        Serial.println("WiFi connected");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());


        // Lets deal with the user config values

        // Copy the string value
        Settings.PoolAddress = pool_text_box.getValue();
        //strncpy(Settings.PoolAddress, pool_text_box.getValue(), sizeof(Settings.PoolAddress));
        Serial.print("PoolString: ");
        Serial.println(Settings.PoolAddress);

        //Convert the number value
        Settings.PoolPort = atoi(port_text_box_num.getValue());
        normalizePoolEndpoint(&Settings);
        Serial.print("portNumber: ");
        Serial.println(Settings.PoolPort);

        strncpy(Settings.PoolApiBase, pool_api_text_box.getValue(), sizeof(Settings.PoolApiBase));
        Serial.print("poolApiBase: ");
        Serial.println(Settings.PoolApiBase);

        // Copy the string value
        strncpy(Settings.PoolPassword, password_text_box.getValue(), sizeof(Settings.PoolPassword));
        Serial.print("poolPassword: ");
        Serial.println(Settings.PoolPassword);

        // Copy the payout and owner wallet values
        strncpy(Settings.PayoutWalletHcash, addr_text_box.getValue(), sizeof(Settings.PayoutWalletHcash));
        Settings.PayoutWalletHcash[sizeof(Settings.PayoutWalletHcash) - 1] = '\0';
        strncpy(Settings.BtcWallet, Settings.PayoutWalletHcash, sizeof(Settings.BtcWallet));
        Settings.BtcWallet[sizeof(Settings.BtcWallet) - 1] = '\0';
        strncpy(Settings.OwnerWalletEvm, owner_wallet_text_box.getValue(), sizeof(Settings.OwnerWalletEvm));
        Settings.OwnerWalletEvm[sizeof(Settings.OwnerWalletEvm) - 1] = '\0';
        Serial.print("payoutWalletHcash: " );
        Serial.println(Settings.PayoutWalletHcash);
        Serial.print("ownerWalletEvm: " );
        Serial.println(Settings.OwnerWalletEvm);

        //Convert the number value
        Settings.Timezone = atoi(time_text_box_num.getValue());
        Serial.print("TimeZone fromUTC: ");
        Serial.println(Settings.Timezone);

        #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
        Settings.invertColors = (strncmp(invertColors.getValue(), "T", 1) == 0);
        Serial.print("Invert Colors: ");
        Serial.println(Settings.invertColors);        
        #endif

        #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
        Settings.Brightness = atoi(brightness_text_box_num.getValue());
        Serial.print("Brightness: ");
        Serial.println(Settings.Brightness);
        #endif

    }

    // Lets deal with the user config values

    // Copy the string value
    Settings.PoolAddress = pool_text_box.getValue();
    //strncpy(Settings.PoolAddress, pool_text_box.getValue(), sizeof(Settings.PoolAddress));
    Serial.print("PoolString: ");
    Serial.println(Settings.PoolAddress);

    //Convert the number value
    Settings.PoolPort = atoi(port_text_box_num.getValue());
    normalizePoolEndpoint(&Settings);
    Serial.print("portNumber: ");
    Serial.println(Settings.PoolPort);

    strncpy(Settings.PoolApiBase, pool_api_text_box.getValue(), sizeof(Settings.PoolApiBase));
    Serial.print("poolApiBase: ");
    Serial.println(Settings.PoolApiBase);

    // Copy the string value
    strncpy(Settings.PoolPassword, password_text_box.getValue(), sizeof(Settings.PoolPassword));
    Serial.print("poolPassword: ");
    Serial.println(Settings.PoolPassword);

    // Copy the payout and owner wallet values
    strncpy(Settings.PayoutWalletHcash, addr_text_box.getValue(), sizeof(Settings.PayoutWalletHcash));
    Settings.PayoutWalletHcash[sizeof(Settings.PayoutWalletHcash) - 1] = '\0';
    strncpy(Settings.BtcWallet, Settings.PayoutWalletHcash, sizeof(Settings.BtcWallet));
    Settings.BtcWallet[sizeof(Settings.BtcWallet) - 1] = '\0';
    strncpy(Settings.OwnerWalletEvm, owner_wallet_text_box.getValue(), sizeof(Settings.OwnerWalletEvm));
    Settings.OwnerWalletEvm[sizeof(Settings.OwnerWalletEvm) - 1] = '\0';
    Serial.print("payoutWalletHcash: " );
    Serial.println(Settings.PayoutWalletHcash);
    Serial.print("ownerWalletEvm: " );
    Serial.println(Settings.OwnerWalletEvm);

    //Convert the number value
    Settings.Timezone = atoi(time_text_box_num.getValue());
    Serial.print("TimeZone fromUTC: ");
    Serial.println(Settings.Timezone);

    #ifdef ESP32_2432S028R
    Settings.invertColors = (strncmp(invertColors.getValue(), "T", 1) == 0);
    Serial.print("Invert Colors: ");
    Serial.println(Settings.invertColors);
    #endif

    // Save the custom parameters to FS
    if (shouldSaveConfig)
    {
        nvMem.saveConfig(&Settings);
        #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
         if (Settings.invertColors) ESP.restart();                
        #endif
        #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
        if (Settings.Brightness != 250) ESP.restart();
        #endif
    }
}

//----------------- MAIN PROCESS WIFI MANAGER --------------
int oldStatus = 0;

void wifiManagerProcess() {

    wm.process(); // avoid delays() in loop when non-blocking and other long running code

    int newStatus = WiFi.status();
    if (newStatus != oldStatus) {
        if (newStatus == WL_CONNECTED) {
            Serial.println("CONNECTED - Current ip: " + WiFi.localIP().toString());
        } else {
            Serial.print("[Error] - current status: ");
            Serial.println(newStatus);
        }
        oldStatus = newStatus;
    }
}
