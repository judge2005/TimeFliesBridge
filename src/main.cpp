#include <Arduino.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "BTSPP.h"
#include "BTGAP.h"
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <AsyncWiFiManager.h>
#include <ConfigItem.h>
#include <EEPROMConfig.h>
#include <ImprovWiFi.h>
#include <Update.h>
#include <ASyncOTAWebUpdate.h>
#include <EspSNTPTimeSync.h>
#include "WSHandler.h"
#include "WSMenuHandler.h"
#include "WSInfoHandler.h"
#include "WSConfigHandler.h"
#include "TimeFliesClock.h"
#include "LEDs.h"

#include "time.h"
#include "sys/time.h"

#define DEBUG(...) { Serial.println(__VA_ARGS__); }

#define OTA
#define MAX_MSG_SIZE 40

const char *manifest[]{
    // Firmware name
    "Time Flies Bridge",
    // Firmware version
    "0.1.0",
    // Hardware chip/variant
    "esp32-pico-devkitm-2",
    // Device name
    "Time Flies"
};

struct LongConfigItem : public ConfigItem<uint64_t> {
	LongConfigItem(const char *name, const uint64_t value)
	: ConfigItem(name, sizeof(uint64_t), value)
	{}

	virtual void fromString(const String &s) { value = strtoull(s.c_str(), nullptr, 16); }
	virtual String toJSON(bool bare = false, const char **excludes = 0) const { return String(value, 16); }
	virtual String toString(const char **excludes = 0) const { return String(value, 16); }
	LongConfigItem& operator=(const uint64_t val) { value = val; return *this; }
};
template <class T>
void ConfigItem<T>::debug(Print *debugPrint) const {
	if (debugPrint != 0) {
		debugPrint->print(name);
		debugPrint->print(":");
		debugPrint->print(value);
		debugPrint->print(" (");
		debugPrint->print(maxSize);
		debugPrint->println(")");
	}
}
template void ConfigItem<uint64_t>::debug(Print *debugPrint) const;

StringConfigItem hostName("hostname", 63, "timefliesbridge");
LongConfigItem tfAddress("address", 0);

AsyncWebServer server(80);
AsyncWebSocket ws("/ws"); // access at ws://[esp ip]/ws
DNSServer dns;
AsyncWiFiManager wifiManager(&server, &dns);
ASyncOTAWebUpdate otaUpdater(Update, "update", "secretsauce");
AsyncWiFiManagerParameter *hostnameParam;
BTSPP btSPP(hostName.toString().c_str());
BTGAP btGAP;
EspSNTPTimeSync *timeSync;
TimeFliesClock timeFliesClock;

TaskHandle_t commitEEPROMTask;
TaskHandle_t sppTask;
TaskHandle_t clockTask;
TaskHandle_t wifiManagerTask;

SemaphoreHandle_t wsMutex;
QueueHandle_t sppQueue;;

String ssid = "TFB";

BaseConfigItem* clockSet[] {
	// Clock
	&TimeFliesClock::getDateFormat(),
	&TimeFliesClock::getTimeOrDate(),
	&TimeFliesClock::getHourFormat(),
	&TimeFliesClock::getDisplayOn(),
	&TimeFliesClock::getDisplayOff(),
	&TimeFliesClock::getOffStateOff(),
	&TimeFliesClock::getTimeZone(),
	&TimeFliesClock::getCommand(),
	0
};

CompositeConfigItem clockConfig("clock", 0, clockSet);

BaseConfigItem* ledsSet[] {
	// LEDs
	&LEDs::getBacklights(),
	&LEDs::getBacklightRed(),
	&LEDs::getBacklightGreen(),
	&LEDs::getBacklightBlue(),

	&LEDs::getUnderlights(),
	&LEDs::getUnderlightRed(),
	&LEDs::getUnderlightGreen(),
	&LEDs::getUnderlightBlue(),

	&LEDs::getBaselights(),
	&LEDs::getBaselightRed(),
	&LEDs::getBaselightGreen(),
	&LEDs::getBaselightBlue(),
	0
};

CompositeConfigItem ledsConfig("leds", 0, ledsSet);

// Global configuration
BaseConfigItem* configSetGlobal[] = {
	&hostName,
    &tfAddress,
	0
};

CompositeConfigItem globalConfig("global", 0, configSetGlobal);

BaseConfigItem* rootConfigSet[] = {
    &globalConfig,
	&clockConfig,
	&ledsConfig,
    0
};

CompositeConfigItem rootConfig("root", 0, rootConfigSet);

EEPROMConfig config(rootConfig);

// Declare some functions
void setWiFiCredentials(const char *ssid, const char *password);
void setWiFiAP(bool);
void infoCallback();
void broadcastUpdate(String originalKey, const BaseConfigItem& item);

template<class T>
void onHostnameChanged(ConfigItem<T> &item) {
	config.commit();
	ESP.restart();
}

String getChipId(void)
{
	uint8_t macid[6];

    esp_efuse_mac_get_default(macid);
    String chipId = String((uint32_t)(macid[5] + (((uint32_t)(macid[4])) << 8) + (((uint32_t)(macid[3])) << 16)), HEX);
    chipId.toUpperCase();
    return chipId;
}

String chipId = getChipId();

void createSSID() {
	// Create a unique SSID that includes the hostname. Max SSID length is 32!
	ssid = (chipId + hostName).substring(0, 31);
}

uint32_t cmdDelay = 1000;

void asyncTimeSetCallback(String time) {
	DEBUG(time);

	struct tm now;
	suseconds_t uSec;

	timeSync->getLocalTime(&now, &uSec);

	// TODO set date format on clock
	char msg[MAX_MSG_SIZE] = {0};
	//	"0x13,$TIM,22,40,45,16,01,25***"
	snprintf(msg, MAX_MSG_SIZE, "0x13,$TIM,%2.2d,%2.2d,%2.2d,%2.2d,%2.2d,%2.2d***",
		now.tm_hour, now.tm_min, now.tm_sec, now.tm_mday, now.tm_mon, now.tm_year);
	xQueueSend(sppQueue, msg, 0);
}

void sendMultipleCommands(const char *commands) {
	static char buf[256];
	if (strchr(commands, ';')) {
		strncpy(buf, commands, 255);
		char *token;
		const char *delimiters = ";";

		// Get the first token
		token = strtok(buf, delimiters);

		// Loop through the rest of the tokens
		while (token != NULL) {
			xQueueSend(sppQueue, token, 0);
			token = strtok(NULL, delimiters);
		}
	} else {
		xQueueSend(sppQueue, commands, 0);
	}
}
void onCommandChanged(ConfigItem<String> &item) {
	sendMultipleCommands(item.value.c_str());
}

void onDateFormatChanged(ConfigItem<byte> &item) {
	// TODO set date format on clock
	char msg[MAX_MSG_SIZE] = {0};
	snprintf(msg, MAX_MSG_SIZE, "0x13,$BIT12,%d***", item.value);
	xQueueSend(sppQueue, msg, 0);
}

void onDisplayChanged(ConfigItem<int> &item) {
	char msg[MAX_MSG_SIZE] = {0};
	if (item == 0) {
		// show hh:mm
		sendMultipleCommands("0x13,$BIT7,0***;0x13,$BIT6,0***;0x13,$BIT5,1***");
	} else if (item == 1) {
		// show mm:ss
		sendMultipleCommands("0x13,$BIT7,1***;0x13,$BIT6,0***;0x13,$BIT5,1***");
	} else if (item == 2) {
		// show day and month according to date format
		onDateFormatChanged(TimeFliesClock::getDateFormat());
		sendMultipleCommands("0x13,$BIT7,0***;0x13,$BIT6,1***;0x13,$BIT5,1***");
	} else if (item == 3) {
		// show day and year
		sendMultipleCommands("0x13,$BIT12,0***;0x13,$BIT7,1***;0x13,$BIT6,1***;0x13,$BIT5,1***");
	}
}

void onHourFormatChanged(ConfigItem<boolean> &item) {
	if (item) {
		xQueueSend(sppQueue, "0x13,$BIT8,1***", 0);
	} else {
		xQueueSend(sppQueue, "0x13,$BIT8,0***", 0);
	}
}

void onTimezoneChanged(ConfigItem<String> &tzItem) {
	timeSync->setTz(tzItem);
	timeSync->sync();
	// Time will be pushed on sync callback
}

const char *backlightsRedTemplates[] = {
	"0x13,$LED2,R,%d***",
	"0x13,$LED3,R,%d***",
	"0x13,$LED4,R,%d***",
	"0x13,$LED5,R,%d***",
	"0x13,$LED6,R,%d***",
	0
};

const char *backlightsGreenTemplates[] = {
	"0x13,$LED2,G,%d***",
	"0x13,$LED3,G,%d***",
	"0x13,$LED4,G,%d***",
	"0x13,$LED5,G,%d***",
	"0x13,$LED6,G,%d***",
	0
};

const char *backlightsBlueTemplates[] = {
	"0x13,$LED2,B,%d***",
	"0x13,$LED3,B,%d***",
	"0x13,$LED4,B,%d***",
	"0x13,$LED5,B,%d***",
	"0x13,$LED6,B,%d***",
	0
};

const char *underlightsRedTemplates[] = {
	"0x13,$LED1,R,%d***",
	"0x13,$LED8,R,%d***",
	0
};

const char *underlightsGreenTemplates[] = {
	"0x13,$LED1,G,%d***",
	"0x13,$LED8,G,%d***",
	0
};

const char *underlightsBlueTemplates[] = {
	"0x13,$LED1,B,%d***",
	"0x13,$LED8,B,%d***",
	0
};

const char *baselightsRedTemplates[] = {
	"0x13,$LED9,R,%d***",
	"0x13,$LED10,R,%d***",
	0
};

const char *baselightsGreenTemplates[] = {
	"0x13,$LED9,G,%d***",
	"0x13,$LED10,G,%d***",
	0
};

const char *baselightsBlueTemplates[] = {
	"0x13,$LED9,B,%d***",
	"0x13,$LED10,B,%d***",
	0
};

void setLights(byte value, const char **pTemplates) {
	char msg[MAX_MSG_SIZE] = {0};
	cmdDelay = 1500;
	while(*pTemplates) {
		snprintf(msg, MAX_MSG_SIZE, *pTemplates, value);
		xQueueSend(sppQueue, msg, 0);
		pTemplates++;
	}
}

void onRedBacklightsChanged(ConfigItem<byte> &item) {
	setLights(item, backlightsRedTemplates);
}

void onGreenBacklightsChanged(ConfigItem<byte> &item) {
	setLights(item, backlightsGreenTemplates);
}

void onBlueBacklightsChanged(ConfigItem<byte> &item) {
	setLights(item, backlightsBlueTemplates);
}

void onRedUnderlightsChanged(ConfigItem<byte> &item) {
	setLights(item, underlightsRedTemplates);
}

void onGreenUnderlightsChanged(ConfigItem<byte> &item) {
	setLights(item, underlightsGreenTemplates);
}

void onBlueUnderlightsChanged(ConfigItem<byte> &item) {
	setLights(item, underlightsBlueTemplates);
}

void onRedBaselightsChanged(ConfigItem<byte> &item) {
	setLights(item, baselightsRedTemplates);
}

void onGreenBaselightsChanged(ConfigItem<byte> &item) {
	setLights(item, baselightsGreenTemplates);
}

void onBlueBaselightsChanged(ConfigItem<byte> &item) {
	setLights(item, baselightsBlueTemplates);
}

static const char* spp_data[] = {
  //"0x13,$PRO0***",  // Profile 0
  "0x13,$PSU,6,4,17,6***", //	Timezone
  "0x13,$BIT13,0***", //	DST off
  //"0x13,$BIT13,1***", //	DST on
  "0x13,$BIT8,1***", //12 hour mode
  // "0x13,$TIM,22,40,45,16,01,25***", // Time hh,mm,ss,dd,mm,yy
  "0x13,$DIM,23,59,6,0,3***", //	Dimming 3
  "0x13,$BIT12,1***", //  MM/DD
  // Tube underlights
  "0x13,$LED2,B,9***",
  "0x13,$LED2,G,12***",
  "0x13,$LED3,B,9***",
  "0x13,$LED3,G,12***",
  "0x13,$LED4,B,9***",
  "0x13,$LED4,G,12***",
  "0x13,$LED5,B,9***",
  "0x13,$LED5,G,12***",
  "0x13,$LED6,B,9***",
  "0x13,$LED6,G,12***",
  //underside
  "0x13,$LED1,B,15***",
  "0x13,$LED8,B,15***",

//   "0x13,$LED1,R,15***",
//   "0x13,$LED8,R,15***",

  "0x13,$LED1,G,15***",
  "0x13,$LED8,G,15***",

  //base
  "0x13,$LED9,B,5***",
  "0x13,$LED9,G,1***",
  "0x13,$LED9,R,0***",
  "0x13,$LED10,B,5***",
  "0x13,$LED10,G,1***",
  "0x13,$LED10,R,0***",

  0
};

static int spp_data_index = 0;

static uint8_t *s_p_data = NULL; /* data pointer of spp_data */

void initSPP() {
    static bool inited = false;

    if (!inited) {
        inited = true;
        WiFi.setSleep(true);
        if (btSPP.init()) {
            if (btGAP.init()) {
                if (tfAddress == 0ULL) {
                    if (!btGAP.startInquiry()) {
                        Serial.printf("BT peer inquiry initialization failed: %s", btGAP.getErrMessage().c_str());
                    }
                }
            } else {
                Serial.printf("GAP initialization failed: %s", btGAP.getErrMessage().c_str());
            }
        } else {
            Serial.printf("BT initialization failed: %s", btSPP.getErrMessage().c_str());
        }
    }
}

void sppTaskFn(void *pArg) {
    static char msg[MAX_MSG_SIZE];

    bool connectSent = false;
	uint32_t delayNextMsg = 1;
	uint32_t lastSendOnState = millis();

	while(true) {
		BaseType_t result = xQueueReceive(sppQueue, msg, pdMS_TO_TICKS(1000));
        if (!wifiManager.isAP()) {
            initSPP();
        }

		if (millis() - lastSendOnState >= 60000) {
			lastSendOnState = millis();

			if (timeFliesClock.clockOn()) {
				// Full brightness and on
				sendMultipleCommands("0x13,$BIT4,0***;0x13,$BIT15,0***");
			} else {
				if (TimeFliesClock::getOffStateOff()) {
					// Full brightness, but off
					sendMultipleCommands("0x13,$BIT15,1***;0x13,$BIT4,0***");
				} else {
					// Dim, but on
					sendMultipleCommands("0x13,$BIT4,1***;0x13,$BIT15,0***");
				}
			}
		}

		if (result) {
			if (*msg == 0) {
				initSPP();
			} else {
				delay(delayNextMsg);
				btSPP.write(msg);
				if (btSPP.isError()) {
					Serial.printf("Error writing message: %s", btSPP.getErrMessage());
				}
			}
			delayNextMsg = cmdDelay;
		} else {
			cmdDelay = 1000;
			delayNextMsg = 1;
		}

        if (!connectSent) {
            if (tfAddress != 0ULL) {
                connectSent = true;
                uint64_t uAddress = tfAddress;
                esp_bd_addr_t address = {0};
                address[0] = uAddress & 0xff;
                address[1] = (uAddress >> 8) & 0xff;
                address[2] = (uAddress >> 16) & 0xff;
                address[3] = (uAddress >> 24) & 0xff;
                address[4] = (uAddress >> 32) & 0xff;
                address[5] = (uAddress >> 40) & 0xff;
                
                btSPP.startConnection(address);
            } else if (btGAP.inquiryDone()) {
                uint8_t *address = btGAP.getAddress("Time Flies");
                if (address) {
                	connectSent = true;
                    tfAddress =  ((uint64_t)address[0]) |
                                ((uint64_t)address[1]) << 8 |
                                ((uint64_t)address[2]) << 16 |
                                ((uint64_t)address[3]) << 24 |
                                ((uint64_t)address[4]) << 32 |
                                ((uint64_t)address[5]) << 40
                                ;
                    tfAddress.put();
                    config.commit();
                    btSPP.startConnection(address);
                } else {
                    // Try again
                    if (!btGAP.startInquiry()) {
						Serial.printf("Error starting inqury: %s", btGAP.getErrMessage());
					}
                }
            }
        }
	}
}

String* items[] {
	&WSMenuHandler::clockMenu,
	&WSMenuHandler::ledsMenu,
	&WSMenuHandler::infoMenu,
	0
};

WSMenuHandler wsMenuHandler(items);
// WSConfigHandler wsMqttHandler(rootConfig, "mqtt");
// WSLEDConfigHandler wsLEDHandler(rootConfig, "leds", ledConfigCallback);
WSConfigHandler wsClockHandler(rootConfig, "clock");
WSConfigHandler wsLEDsHandler(rootConfig, "leds");
WSInfoHandler wsInfoHandler(infoCallback);

// Order of this needs to match the numbers in WSMenuHandler.cpp
WSHandler* wsHandlers[] {
	&wsMenuHandler,
	&wsClockHandler,
	&wsLEDsHandler,
	&wsInfoHandler,
	NULL,
	NULL,
	NULL,
	NULL
};

void infoCallback() {
	wsInfoHandler.setSsid(ssid);
	// wsInfoHandler.setBlankingMonitor(&blankingMonitor);
	wsInfoHandler.setRevision(manifest[1]);

	wsInfoHandler.setFSSize(String(LittleFS.totalBytes()));
	wsInfoHandler.setFSFree(String(LittleFS.totalBytes() - LittleFS.usedBytes()));
	TimeSync::SyncStats &syncStats = timeSync->getStats();

	wsInfoHandler.setFailedCount(syncStats.failedCount);
	wsInfoHandler.setLastFailedMessage(syncStats.lastFailedMessage);
	wsInfoHandler.setLastUpdateTime(syncStats.lastUpdateTime);
	wsInfoHandler.setHostname(hostName);
}

void broadcastUpdate(String originalKey, const BaseConfigItem& item) {
	xSemaphoreTake(wsMutex, portMAX_DELAY);

	JsonDocument doc;
	JsonObject root = doc.to<JsonObject>();

	root["type"] = "sv.update";

	JsonVariant value = root.createNestedObject("value");
	String rawJSON = item.toJSON();	// This object needs to hang around until we are done serializing.
	value[originalKey] = serialized(rawJSON.c_str());

	size_t len = measureJson(root);
	AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
	if (buffer) {
		serializeJson(root, (char *)buffer->get(), len + 1);
		ws.textAll(buffer);
	}

	xSemaphoreGive(wsMutex);
}

void updateValue(String originalKey, String _key, String value, BaseConfigItem *item) {
	int index = _key.indexOf('-');
	if (index == -1) {
		const char* key = _key.c_str();
		item = item->get(key);
		if (item != 0) {
			item->fromString(value);
			item->put();

			// Order of below is important to maintain external consistency
			broadcastUpdate(originalKey, *item);
			item->notify();
		} else if (_key == "wifi_ap") {
			setWiFiAP(value == "true" ? true : false);
		}
	} else {
		String firstKey = _key.substring(0, index);
		String nextKey = _key.substring(index+1);
		updateValue(originalKey, nextKey, value, item->get(firstKey.c_str()));
	}
}

void updateValue(int screen, String pair) {
	int index = pair.indexOf(':');
	DEBUG(pair)
	// _key has to hang around because key points to an internal data structure
	String _key = pair.substring(0, index);
	String value = pair.substring(index+1);

	updateValue(_key, _key, value, &rootConfig);
}

/*
 * Handle application protocol
 */
void handleWSMsg(AsyncWebSocketClient *client, char *data) {
	String wholeMsg(data);
	int code = wholeMsg.substring(0, wholeMsg.indexOf(':')).toInt();

	if (code < 9) {
        WSHandler* handler = wsHandlers[code];
        if (handler) {
		    handler->handle(client, data);
        }
	} else {
		String message = wholeMsg.substring(wholeMsg.indexOf(':')+1);
		int screen = message.substring(0, message.indexOf(':')).toInt();
		String pair = message.substring(message.indexOf(':')+1);
		updateValue(screen, pair);
	}
}

void wsHandler(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
	//Handle WebSocket event
	switch (type) {
	case WS_EVT_CONNECT:
		DEBUG("WS connected")
		;
		break;
	case WS_EVT_DISCONNECT:
		DEBUG("WS disconnected")
		;
		break;
	case WS_EVT_ERROR:
		DEBUG("WS error")
		;
		DEBUG((char* )data)
		;
		break;
	case WS_EVT_PONG:
		DEBUG("WS pong")
		;
		break;
	case WS_EVT_DATA:	// Yay we got something!
		DEBUG("WS data")
		;
		AwsFrameInfo * info = (AwsFrameInfo*) arg;
		if (info->final && info->index == 0 && info->len == len) {
			//the whole message is in a single frame and we got all of it's data
			if (info->opcode == WS_TEXT) {
				DEBUG("WS text data");
				data[len] = 0;
				handleWSMsg(client, (char *) data);
			} else {
				DEBUG("WS binary data");
			}
		} else {
			DEBUG("WS data was split up!");
		}
		break;
	}
}

void mainHandler(AsyncWebServerRequest *request) {
	DEBUG("Got request")
	request->send(LittleFS, "/index.html");
}

void sendUpdateForm(AsyncWebServerRequest *request) {
	request->send(LittleFS, "/update.html");
}

void sendUpdatingInfo(AsyncResponseStream *response, boolean hasError) {
  response->print("<html><head><meta http-equiv=\"refresh\" content=\"10; url=/\"></head><body>");

  hasError ?
      response->print("Update failed: please wait while the device reboots") :
      response->print("Update OK: please wait while the device reboots");

  response->print("</body></html>");
}

void sendFavicon(AsyncWebServerRequest *request) {
	DEBUG("Got favicon request")
	request->send(LittleFS, "/assets/favicon-32x32.png", "image/png");
}

void configureWebServer() {
	server.serveStatic("/", LittleFS, "/");
	server.on("/", HTTP_GET, mainHandler).setFilter(ON_STA_FILTER);
	server.on("/assets/favicon-32x32.png", HTTP_GET, sendFavicon);
	server.serveStatic("/assets", LittleFS, "/assets");
	
#ifdef OTA
	otaUpdater.init(server, "/update", sendUpdateForm, sendUpdatingInfo);
#endif

	// attach AsyncWebSocket
	ws.onEvent(wsHandler);
	server.addHandler(&ws);
	server.begin();
	ws.enable(true);
}

void connectedHandler() {
	DEBUG("connectedHandler");

	MDNS.end();
	MDNS.begin(hostName.value.c_str());
	MDNS.addService("http", "tcp", 80);
}

void apChange(AsyncWiFiManager *wifiManager) {
	DEBUG("apChange()");
	DEBUG(wifiManager->isAP());
	if (!wifiManager->isAP()) {
		xQueueSend(sppQueue, "", 0);
	}
}

void setWiFiAP(bool on) {
	if (on) {
		wifiManager.startConfigPortal(ssid.c_str(), "secretsauce");
	} else {
		wifiManager.stopConfigPortal();
	}
}

void setupServer() {
	DEBUG("setupServer()");
	hostName = String(hostnameParam->getValue());
	hostName.put();
	config.commit();
	createSSID();
	wifiManager.setAPCredentials(ssid.c_str(), "secretsauce");
	DEBUG(hostName.value.c_str());
	MDNS.begin(hostName.value.c_str());
	MDNS.addService("http", "tcp", 80);
}

void wifiManagerTaskFn(void *pArg) {
	while(true) {
		xSemaphoreTake(wsMutex, portMAX_DELAY);
		wifiManager.loop();
		xSemaphoreGive(wsMutex);

		delay(50);
	}
}

void commitEEPROMTaskFn(void *pArg) {
	while(true) {
		delay(60000);
		Serial.println("Committing config");
		config.commit();
	}
}

void initFromEEPROM() {
//	config.setDebugPrint(debugPrint);
	config.init();
//	rootConfig.debug(debugPrint);
	DEBUG(hostName);
	rootConfig.get();	// Read all of the config values from EEPROM
	DEBUG(hostName);

	hostnameParam = new AsyncWiFiManagerParameter("Hostname", "device host name", hostName.value.c_str(), 63);
}

void setup() {
    Serial.begin(115200);
    Serial.setDebugOutput(true);

    /* Initialize NVS — it is used to store PHY calibration data */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

	wsMutex = xSemaphoreCreateMutex();
    sppQueue = xQueueCreate(10, MAX_MSG_SIZE);
 
	createSSID();

	EEPROM.begin(2048);
	initFromEEPROM();

	LittleFS.begin();

	timeSync = new EspSNTPTimeSync(TimeFliesClock::getTimeZone(), asyncTimeSetCallback, NULL);
	timeSync->init();

    xTaskCreatePinnedToCore(
        commitEEPROMTaskFn,   /* Function to implement the task */
        "Commit EEPROM task", /* Name of the task */
        2048,                 /* Stack size in words */
        NULL,                 /* Task input parameter */
        tskIDLE_PRIORITY,     /* More than background tasks */
        &commitEEPROMTask,    /* Task handle. */
        xPortGetCoreID());

    xTaskCreatePinnedToCore(
        sppTaskFn,   /* Function to implement the task */
        "BT SPP task", /* Name of the task */
        4096,                 /* Stack size in words */
        NULL,                 /* Task input parameter */
        tskIDLE_PRIORITY + 1,     /* More than background tasks */
        &sppTask,    /* Task handle. */
        xPortGetCoreID());

	timeFliesClock.setTimeSync(timeSync);

	TimeFliesClock::getTimeOrDate().setCallback(onDisplayChanged);
	TimeFliesClock::getDateFormat().setCallback(onDateFormatChanged);
	TimeFliesClock::getHourFormat().setCallback(onHourFormatChanged);
	TimeFliesClock::getTimeZone().setCallback(onTimezoneChanged);
	TimeFliesClock::getCommand().setCallback(onCommandChanged);

	LEDs::getBacklightRed().setCallback(onRedBacklightsChanged);
	LEDs::getBacklightGreen().setCallback(onGreenBacklightsChanged);
	LEDs::getBacklightBlue().setCallback(onBlueBacklightsChanged);
	LEDs::getUnderlightRed().setCallback(onRedUnderlightsChanged);
	LEDs::getUnderlightGreen().setCallback(onGreenUnderlightsChanged);
	LEDs::getUnderlightBlue().setCallback(onBlueUnderlightsChanged);
	LEDs::getBaselightRed().setCallback(onRedBaselightsChanged);
	LEDs::getBaselightGreen().setCallback(onGreenBaselightsChanged);
	LEDs::getBaselightBlue().setCallback(onBlueBaselightsChanged);

    wifiManager.setDebugOutput(true);
    wifiManager.setHostname(hostName.value.c_str()); // name router associates DNS entry with
    wifiManager.setCustomOptionsHTML("<br><form action='/t' name='time_form' method='post'><button name='time' onClick=\"{var now=new Date();this.value=now.getFullYear()+','+(now.getMonth()+1)+','+now.getDate()+','+now.getHours()+','+now.getMinutes()+','+now.getSeconds();} return true;\">Set Clock Time</button></form><br><form action=\"/app.html\" method=\"get\"><button>Configure Clock</button></form>");
    wifiManager.addParameter(hostnameParam);
    wifiManager.setSaveConfigCallback(setupServer);
    wifiManager.setConnectedCallback(connectedHandler);
    wifiManager.setConnectTimeout(2000); // milliseconds
    wifiManager.setAPCallback(apChange);
    wifiManager.setAPCredentials(ssid.c_str(), "secretsauce");
    wifiManager.start();

    configureWebServer();

    xTaskCreatePinnedToCore(
        wifiManagerTaskFn,    /* Function to implement the task */
        "WiFi Manager task",  /* Name of the task */
        3000,                 /* Stack size in words */
        NULL,                 /* Task input parameter */
        tskIDLE_PRIORITY + 2, /* Priority of the task (idle) */
        &wifiManagerTask,     /* Task handle. */
        xPortGetCoreID());

	hostName.setCallback(onHostnameChanged);

    Serial.print("setup() running on core ");
    Serial.println(xPortGetCoreID());

    vTaskDelete(NULL);	// Delete this task (so loop() won't be called)
}

void loop() {
  // put your main code here, to run repeatedly:
}
