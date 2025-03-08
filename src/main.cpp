#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#if ESP_ARDUINO_VERSION_MAJOR >= 3
#include "esp_mac.h"
#endif
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
#include <unordered_map>
#include "WSHandler.h"
#include "WSMenuHandler.h"
#include "WSInfoHandler.h"
#include "WSConfigHandler.h"
#include "TimeFliesClock.h"
#include "LEDs.h"
#include "MovementSensor.h"
#include "Uptime.h"
#include "Logger.h"

#include "time.h"
#include "sys/time.h"

#define OTA
#define MAX_MSG_SIZE 40
#define SPP_QUEUE_SIZE 40

const char *manifest[]{
    // Firmware name
    "Time Flies Bridge",
    // Firmware version
    "0.2.0",
    // Hardware chip/variant
    "ESP32",
    // Device name
    "Time Flies"
};

const char* TIME_FLIES_TAG = "TIME_FLIES";

void broadcastUpdate(String originalKey, const BaseConfigItem& item);
void broadcastUpdate(const JsonDocument &doc);

Uptime uptime;
Logger logger;

typedef enum {
	NOT_INITIALIZED = 0,
    NOT_CONNECTED,
    SEARCHING,
    CONNECTING,
    CONNECTED,
	DISCONNECTING
} SPPConnectionState;

std::unordered_map<SPPConnectionState, std::string> state2string = {
	{NOT_INITIALIZED, "NOT_INITIALIZED"},
    {NOT_CONNECTED, "NOT_CONNECTED"},
	{SEARCHING, "SEARCHING"},
	{CONNECTING, "CONNECTING"},
	{CONNECTED, "CONNECTED"},
	{DISCONNECTING, "DISCONNECTING"}
};

StringConfigItem hostName("hostname", 63, "timefliesbridge");

AsyncWebServer server(80);
AsyncWebSocket ws("/ws"); // access at ws://[esp ip]/ws
DNSServer dns;
AsyncWiFiManager wifiManager(&server, &dns);
ASyncOTAWebUpdate otaUpdater(Update, "update", "secretsauce");
WiFiUDP syncBus;

AsyncWiFiManagerParameter *hostnameParam;
EspSNTPTimeSync *timeSync;
TimeFliesClock timeFliesClock;
MovementSensor mov;

TaskHandle_t commitEEPROMTask;
TaskHandle_t sppTask;
TaskHandle_t wifiManagerTask;
TaskHandle_t syncBusTask;

SemaphoreHandle_t wsMutex;
QueueHandle_t sppQueue;

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
	&TimeFliesClock::getEffect(),
	&TimeFliesClock::getRippleDirection(),
	&TimeFliesClock::getRippleSpeed(),
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

BaseConfigItem* extraSet[] {
	&TimeFliesClock::getCommand(),
	0
};

CompositeConfigItem extraConfig("extra", 0, extraSet);

// Global configuration
BaseConfigItem* configSetGlobal[] = {
	&hostName,
	0
};

CompositeConfigItem globalConfig("global", 0, configSetGlobal);

// Sync config values
IntConfigItem sync_port("sync_port", 4920);
BooleanConfigItem sync_role("sync_role", false);	// false = no sync, true = slave (remote sync)
ByteConfigItem mov_delay("mov_delay", 20);

BaseConfigItem* syncSet[] {
	&sync_port,
	&sync_role,
	&mov_delay,
	0
};

CompositeConfigItem syncConfig("sync", 0, syncSet);

BaseConfigItem* rootConfigSet[] = {
    &globalConfig,
	&clockConfig,
	&ledsConfig,
	&extraConfig,
	&syncConfig,
    0
};

CompositeConfigItem rootConfig("root", 0, rootConfigSet);

EEPROMConfig config(rootConfig);

// Declare some functions
void setWiFiAP(bool);
void infoCallback();

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

String wifiCallback() {
	String wifiStatus = "\"wifi_ap\":";

	if ((WiFi.getMode() & WIFI_MODE_AP) != 0) {
		wifiStatus += "true";
	} else {
		wifiStatus += "false";
	}

	wifiStatus += ",";
	wifiStatus += "\"hostname\":\"";
	wifiStatus += hostName.value;
	wifiStatus += "\"";

	return wifiStatus;
}

uint16_t syncBusPort = 0;
unsigned long lastMoved = 0;

void writeSyncBus(const char msg[]) {
	IPAddress broadcastIP(~WiFi.subnetMask() | WiFi.gatewayIP());
	syncBusPort = sync_port;
	syncBus.beginPacket(broadcastIP, syncBusPort);
	syncBus.write((unsigned char*)msg, strlen(msg));
	syncBus.endPacket();
}

void announceSlave() {
	static char syncMsg[] = "slave";
	if (sync_role) {
		writeSyncBus(syncMsg);
	}
}

void readSyncBus() {
	static char incomingMsg[10];

	int size = syncBus.parsePacket();

	if (size) {
		int len = syncBus.read(incomingMsg, 9);
		if (len > 0 && len < 10) {
			incomingMsg[len] = 0;

			if (strncmp("mov", incomingMsg, 3) == 0) {
				lastMoved = millis();
				mov.trigger();
			}
		}
	}
}

void syncBusTaskFn(void *pArg) {
	static bool currentRole = false;

	mov.setOnTime(millis());

	while (true) {
		mov.setDelay(mov_delay);
		mov.setEnabled(sync_role);

		if (sync_role) {
			// If port has changed, set new port and announce status
			if (syncBusPort != sync_port) {
				syncBusPort = sync_port;
				currentRole = false;	// Force a begin
			}
		} else {
			// We aren't in a sync group, stop the sync bus
			currentRole = sync_role;
			syncBus.stop();
		}

		if (currentRole != sync_role) {
			if (sync_role) {
				// We used to not be in a sync group, or the sync group has changed
				syncBus.begin(syncBusPort);
			}

			currentRole = sync_role;

			if (currentRole) {
				// We have become a slave
				announceSlave();
			}
		}

		readSyncBus();
		delay(10);
	}
}

uint32_t cmdDelay = 1000;

void pushAllValues() {
	for (int i=0; ledsSet[i] != 0; i++) {
		ledsSet[i]->notify();
	}

	for (int i=0; clockSet[i] != 0; i++) {
		clockSet[i]->notify();
	}
}

void sendCommands(const char *commands) {
	static char buf[256 + MAX_MSG_SIZE];
	if (strchr(commands, ';')) {
		strncpy(buf, commands, 255);
		char *token;
		const char *delimiters = ";";

		// Get the first token
		token = strtok(buf, delimiters);

		// Loop through the rest of the tokens
		while (token != NULL) {
			ESP_LOGD(TIME_FLIES_TAG, "Queueing command %s", token);
			xQueueSend(sppQueue, token, 0);
			token = strtok(NULL, delimiters);
		}
	} else {
		ESP_LOGD(TIME_FLIES_TAG, "Queueing command %s", commands);
		xQueueSend(sppQueue, commands, 0);
	}
}

void sendCurrentTime() {
	struct tm now;
	suseconds_t uSec;

	timeSync->getLocalTime(&now, &uSec);

	char msg[128] = {0};
	// Set timezone offset - 0x13,$PSU,6,4,20,6*** values 1-12 are added, 13-23 are subtracted -12 so 13 becomes -1.
	// EST                   0x13,$PSU,6,4,17,6*** i.e. TZ offset = 12 - 17 = -5
	int tzo = -(_timezone / 60 / 60);

	if (tzo < 0) {
		tzo = 12 - tzo;
	}

	//	"0x13,$TIM,22,40,45,16,01,25***"
	snprintf(msg, 128, "0x13,$BIT13,%d***;0x13,$PSU,6,4,%d,6***;0x13,$TIM,%2.2d,%2.2d,%2.2d,%2.2d,%2.2d,%2.2d***",
		now.tm_isdst, tzo, now.tm_hour, now.tm_min, now.tm_sec, now.tm_mday, now.tm_mon, now.tm_year);
	
	sendCommands(msg);
}

uint8_t r_buffer[50];
uint8_t r_position = 0;

void readFromServer() {
    while (Serial1.available() > 0) {
        int c = Serial1.read();
        if (c > 0) {
            delay(1);
            if (r_position < sizeof(r_buffer)) {
                if (c == '\n') {
                    if (r_position >= 1 && r_buffer[r_position-1] == '\r') {
                        r_buffer[r_position-1] = 0;
                    } else {
                        r_buffer[r_position] = 0;
                    }

					// If there was something other than just CRLF
					if (r_position > 1) {
						logger.log(Logger::INFO, "< %s", r_buffer);
					}

                    r_position = 0;
                } else {
                    r_buffer[r_position++] = c;
                }
            } else {
                if (c == '\n') {
                    ESP_LOGE(TIME_FLIES_TAG, "Receive buffer overlow");
                    r_position = 0;
                }
            }
        }
    }
}

void asyncTimeSetCallback(String time) {
	ESP_LOGD(TIME_FLIES_TAG, "Time: %s", time.c_str());

	sendCurrentTime();
}

void onCommandChanged(ConfigItem<String> &item) {
	sendCommands(item.value.c_str());
}

void onDateFormatChanged(ConfigItem<byte> &item) {
	// TODO set date format on clock
	char msg[MAX_MSG_SIZE] = {0};
	snprintf(msg, MAX_MSG_SIZE, "0x13,$BIT12,%d***", item.value);
	sendCommands(msg);
}

void onDisplayChanged(ConfigItem<int> &item) {
	char msg[MAX_MSG_SIZE] = {0};
	if (item == 0) {
		// show hh:mm
		sendCommands("0x13,$BIT7,0***;0x13,$BIT6,0***;0x13,$BIT5,1***");
	} else if (item == 1) {
		// show mm:ss
		sendCommands("0x13,$BIT7,1***;0x13,$BIT6,0***;0x13,$BIT5,1***");
	} else if (item == 2) {
		// show day and month according to date format
		onDateFormatChanged(TimeFliesClock::getDateFormat());
		sendCommands("0x13,$BIT7,0***;0x13,$BIT6,1***;0x13,$BIT5,1***");
	} else if (item == 3) {
		// show day and year
		sendCommands("0x13,$BIT12,0***;0x13,$BIT7,1***;0x13,$BIT6,1***;0x13,$BIT5,1***");
	}
}

void onRippleSpeedChanged(ConfigItem<bool> &item) {
	if (item) {
		sendCommands("0x13,$BIT0,1***");
	} else {
		sendCommands("0x13,$BIT0,0***");
	}
}

void onRippleDirectionChanged(ConfigItem<bool> &item) {
	if (item) {
		sendCommands("0x13,$BIT1,1***");
	} else {
		sendCommands("0x13,$BIT1,0***");
	}
}

void onEffectChanged(ConfigItem<byte> &item) {
	if (item == 0) {	// No Transition effect
		sendCommands("0x13,$BIT3,1***");
	} else if (item == 1) {	// Fade transition effect - need to turn off ripple
		sendCommands("0x13,$BIT2,0***;0x13,$BIT3,0***");
	} else if (item == 2) { // Ripple transition effect - need to turn on fade
		// Set speed and direction first
		onRippleSpeedChanged(TimeFliesClock::getRippleSpeed());
		onRippleDirectionChanged(TimeFliesClock::getRippleDirection());
		sendCommands("0x13,$BIT2,1***;0x13,$BIT3,0***");
	}
}

void onHourFormatChanged(ConfigItem<boolean> &item) {
	if (item) {
		sendCommands("0x13,$BIT8,1***");
	} else {
		sendCommands("0x13,$BIT8,0***");
	}
}

void onTimezoneChanged(ConfigItem<String> &tzItem) {
	timeSync->setTz(tzItem);
	sendCurrentTime();
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
		sendCommands(msg);
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

SPPConnectionState connectionStatus = NOT_INITIALIZED;

#define LED_PIN 2
#define RXD 16
#define TXD 17
#define COMMAND_PIN 18
String OK_RESPONSE("OK\r");

bool verifySPPCommand(String command) {
	Serial1.println(command);
	String response = Serial1.readStringUntil('\n');
	bool ret = response.equals(OK_RESPONSE);
	if (!ret) {
		logger.log(Logger::WARN, "! %s", response.c_str());
	}

	return ret;
}

void getSPPState() {
	Serial1.println("AT+STATE");
	String result = Serial1.readStringUntil('\n');
	int status = result.charAt(0) - '0';
	if (status >= 0 && status <= 9) {
		if (connectionStatus != status) {
			connectionStatus = (SPPConnectionState)status;
			logger.log(Logger::INFO, "+ %s", state2string[connectionStatus].c_str());
		}
	} else {
		logger.log(Logger::WARN, "! %s", result.c_str());
	}
	// Read OK\r\n - keep going until we get OK or until 1s passes
	unsigned long start = millis();
	do
	{	
		result = Serial1.readStringUntil('\n');
		if (millis() - start > 1000) {
			// Give up after 1s
			break;
		}
	} while (!result.equals(OK_RESPONSE));
}

void setRname() {
	verifySPPCommand("AT+RNAME=Time Flies");
}

void initiateConnection() {
	verifySPPCommand("AT+CONNECT");
}

void closeConnection() {
	verifySPPCommand("AT+DISCONNECT");
}

void sppTaskFn(void *pArg) {
    static char msg[MAX_MSG_SIZE];

	ESP_LOGD(TIME_FLIES_TAG, "%s", "sppTaskFn()");

	uint32_t delayNextMsg = 1;
	bool wasOn = !timeFliesClock.clockOn();	// Force a clock state message initially
	uint32_t maxWait = 500;
	setRname();
	delay(10000);
	uint32_t lastConnectedTime = millis();
	bool ledOn = false;
	uint32_t lastLedOn = millis();

	xTaskCreatePinnedToCore(
		syncBusTaskFn, /* Function to implement the task */
		"Sync bus task", /* Name of the task */
		4096,  /* Stack size in words */
		NULL,  /* Task input parameter */
		tskIDLE_PRIORITY,  /* More than background tasks */
		&syncBusTask,  /* Task handle. */
		0
		);

	while(true) {
		BaseType_t result = xQueuePeek(sppQueue, msg, pdMS_TO_TICKS(maxWait));
		uptime.loop();

		readFromServer();	// Do this before we send a command in getSPPState();
		getSPPState();

		if (result == pdTRUE) {
			if (connectionStatus == CONNECTED) {
				// If we are connected, just drain the queue
				lastConnectedTime = millis();
				xQueueReceive(sppQueue, msg, 0);
				delay(delayNextMsg);
				logger.log(Logger::INFO, "> %s", msg);
				Serial1.println(msg);
				delayNextMsg = cmdDelay;
				continue;
			} else if (connectionStatus == NOT_CONNECTED) {
				initiateConnection();
			}
		} else {
			cmdDelay = 1000;
			delayNextMsg = 1;
		}

		timeFliesClock.setMov(mov.isOn());

		// NOTE: sendCommands(...) just puts them on the queue
		if (timeFliesClock.clockOn() != wasOn) {
			wasOn = timeFliesClock.clockOn();
			if (wasOn) {
				// Full brightness and on
				sendCommands("0x13,$BIT4,0***;0x13,$BIT15,0***");
			} else {
				if (TimeFliesClock::getOffStateOff()) {
					// Full brightness, but off
					sendCommands("0x13,$BIT15,1***;0x13,$BIT4,0***");
				} else {
					// Dim, but on
					sendCommands("0x13,$BIT4,1***;0x13,$BIT15,0***");
				}
			}
		}

		if (connectionStatus == CONNECTED) {
			ledOn = true;
#ifdef DISCONNECT_ON_DILE
			if (millis() - lastConnectedTime > 30000) {
				logger.log(INFO, "Disconnecting");
				closeConnection();
			}
#endif
		}

		if (connectionStatus == NOT_CONNECTED) {
			initiateConnection();
		}

		if (ledOn) {
			digitalWrite(LED_PIN, HIGH);
		} else {
			digitalWrite(LED_PIN, LOW);
		}

		if (millis() - lastLedOn > 1000) {
			lastLedOn = millis();
			ledOn = !ledOn;
		}
	}
}

String* items[] {
	&WSMenuHandler::clockMenu,
	&WSMenuHandler::ledsMenu,
	&WSMenuHandler::extraMenu,
	&WSMenuHandler::infoMenu,
	&WSMenuHandler::syncMenu,
	0
};

WSMenuHandler wsMenuHandler(items);
WSConfigHandler wsClockHandler(rootConfig, "clock");
WSConfigHandler wsLEDsHandler(rootConfig, "leds");
WSConfigHandler wsExtrasHandler(rootConfig, "extra", []() { return logger.getSerializedJsonLog(); });
WSConfigHandler wsSyncHandler(rootConfig, "sync", wifiCallback);
WSInfoHandler wsInfoHandler(infoCallback);

// Order of this needs to match the numbers in WSMenuHandler.cpp
WSHandler* wsHandlers[] {
	&wsMenuHandler,
	&wsClockHandler,
	&wsLEDsHandler,
	&wsExtrasHandler,
	&wsInfoHandler,
	&wsSyncHandler,
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

	wsInfoHandler.setUptime(uptime.uptime());
}

void broadcastUpdate(const JsonDocument &doc) {
	if (xSemaphoreTake(wsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
		ESP_LOGE(TIME_FLIES_TAG, "Failed to obtain wsMutex");
		return;
	}

	size_t len = measureJson(doc);

	AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
	if (buffer) {
		serializeJson(doc, (char *)buffer->get(), len);
		ws.textAll(buffer);
	}

	xSemaphoreGive(wsMutex);
}

void broadcastUpdate(String originalKey, const BaseConfigItem& item) {
	JsonDocument doc;
	doc["type"] = "sv.update";
	
	String rawJSON = item.toJSON();	// This object needs to hang around until we are done serializing.
	doc["value"][originalKey] = serialized(rawJSON.c_str());

	broadcastUpdate(doc);
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
		} else if (_key == "sync_do") {
			announceSlave();
		} else if (_key == "wifi_ap") {
			setWiFiAP(value == "true" ? true : false);
		} else if (_key == "hostname") {
			hostName = value;
			hostName.put();
			config.commit();
			ESP.restart();
		} else if (_key == "push_all_values") {
			pushAllValues();
		} else if (_key == "push_time") {
			asyncTimeSetCallback("Pushed from GUI");
		}
	} else {
		String firstKey = _key.substring(0, index);
		String nextKey = _key.substring(index+1);
		updateValue(originalKey, nextKey, value, item->get(firstKey.c_str()));
	}
}

void updateValue(int screen, String pair) {
	int index = pair.indexOf(':');
	ESP_LOGD(TIME_FLIES_TAG, "Pair: %s", pair.c_str());

	// _key has to hang around because key points to an internal data structure
	String _key = pair.substring(0, index);
	String value = pair.substring(index+1);

	updateValue(_key, _key, value, &rootConfig);
}

/*
 * Handle application protocol
 */
void handleWSMsg(AsyncWebSocketClient *client, const char *data) {
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
		ESP_LOGD(TIME_FLIES_TAG, "WS connected");
		break;
	case WS_EVT_DISCONNECT:
		ESP_LOGD(TIME_FLIES_TAG, "WS disconnected");
		break;
	case WS_EVT_ERROR:
		ESP_LOGD(TIME_FLIES_TAG, "WS Error, data: %s", (char* )data);
		break;
	case WS_EVT_PONG:
		ESP_LOGD(TIME_FLIES_TAG, "WS pong");
		break;
	case WS_EVT_DATA:	// Yay we got something!
		ESP_LOGD(TIME_FLIES_TAG, "WS data");
		AwsFrameInfo * info = (AwsFrameInfo*) arg;
		if (info->final && info->index == 0 && info->len == len) {
			//the whole message is in a single frame and we got all of it's data
			if (info->opcode == WS_TEXT) {
				ESP_LOGD(TIME_FLIES_TAG, "WS text data");
				data[len] = 0;
				handleWSMsg(client, reinterpret_cast<const char*>(data));
			} else {
				ESP_LOGD(TIME_FLIES_TAG, "WS binary data");
			}
		} else {
			ESP_LOGD(TIME_FLIES_TAG, "WS data was split up!");
		}
		break;
	}
}

void mainHandler(AsyncWebServerRequest *request) {
	ESP_LOGD(TIME_FLIES_TAG, "Got request");
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
	ESP_LOGD(TIME_FLIES_TAG, "Got favicon request");
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
	ESP_LOGD(TIME_FLIES_TAG, "connectedHandler");

	MDNS.end();
	MDNS.begin(hostName.value.c_str());
	MDNS.addService("http", "tcp", 80);
}

void apChange(AsyncWiFiManager *wifiManager) {
	ESP_LOGD(TIME_FLIES_TAG, "apChange(), isAP: %d", wifiManager->isAP());
}

void setWiFiAP(bool on) {
	if (on) {
		wifiManager.startConfigPortal(ssid.c_str(), "secretsauce");
	} else {
		wifiManager.stopConfigPortal();
	}
}

void setupServer() {
	ESP_LOGD(TIME_FLIES_TAG, "setupServer()");
	hostName = String(hostnameParam->getValue());
	hostName.put();
	config.commit();
	createSSID();
	wifiManager.setAPCredentials(ssid.c_str(), "secretsauce");
	ESP_LOGD(TIME_FLIES_TAG, "Hostname: %s", hostName.value.c_str());
	MDNS.begin(hostName.value.c_str());
	MDNS.addService("http", "tcp", 80);
}

void wifiManagerTaskFn(void *pArg) {
	ESP_LOGD(TIME_FLIES_TAG, "%s", "wifiManagerTaskFn()");

	while(true) {
		if (xSemaphoreTake(wsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
			ESP_LOGE(TIME_FLIES_TAG, "Failed to obtain wsMutex");
			continue;
		}
		wifiManager.loop();
		xSemaphoreGive(wsMutex);

		delay(50);
	}
}

void commitEEPROMTaskFn(void *pArg) {
	ESP_LOGD(TIME_FLIES_TAG, "%s", "commitEEPROMTaskFn()");
	while(true) {
		delay(60000);
		ESP_LOGD(TIME_FLIES_TAG, "Committing config");
		config.commit();
	}
}

void initFromEEPROM() {
//	config.setDebugPrint(debugPrint);
	config.init();
//	rootConfig.debug(debugPrint);
	ESP_LOGD(TIME_FLIES_TAG, "Hostname: %s", hostName.value.c_str());
	rootConfig.get();	// Read all of the config values from EEPROM
	ESP_LOGD(TIME_FLIES_TAG, "Hostname: %s", hostName.value.c_str());

	hostnameParam = new AsyncWiFiManagerParameter("Hostname", "device host name", hostName.value.c_str(), 63);
}

void setup() {
    Serial.begin(115200);
    Serial.setDebugOutput(true);
	logger.setUpdateCallback([] (const JsonDocument& doc) { broadcastUpdate(doc); });

	pinMode(COMMAND_PIN, OUTPUT);
	pinMode(LED_PIN, OUTPUT);
  	Serial1.begin(38400, SERIAL_8N1, RXD, TXD);

	wsMutex = xSemaphoreCreateMutex();
    sppQueue = xQueueCreate(SPP_QUEUE_SIZE, MAX_MSG_SIZE);
 
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

	timeFliesClock.setTimeSync(timeSync);

	TimeFliesClock::getTimeOrDate().setCallback(onDisplayChanged);
	TimeFliesClock::getDateFormat().setCallback(onDateFormatChanged);
	TimeFliesClock::getHourFormat().setCallback(onHourFormatChanged);
	TimeFliesClock::getTimeZone().setCallback(onTimezoneChanged);
	TimeFliesClock::getCommand().setCallback(onCommandChanged);
	TimeFliesClock::getEffect().setCallback(onEffectChanged);
	TimeFliesClock::getRippleDirection().setCallback(onRippleDirectionChanged);
	TimeFliesClock::getRippleSpeed().setCallback(onRippleSpeedChanged);

	LEDs::getBacklightRed().setCallback(onRedBacklightsChanged);
	LEDs::getBacklightGreen().setCallback(onGreenBacklightsChanged);
	LEDs::getBacklightBlue().setCallback(onBlueBacklightsChanged);
	LEDs::getUnderlightRed().setCallback(onRedUnderlightsChanged);
	LEDs::getUnderlightGreen().setCallback(onGreenUnderlightsChanged);
	LEDs::getUnderlightBlue().setCallback(onBlueUnderlightsChanged);
	LEDs::getBaselightRed().setCallback(onRedBaselightsChanged);
	LEDs::getBaselightGreen().setCallback(onGreenBaselightsChanged);
	LEDs::getBaselightBlue().setCallback(onBlueBaselightsChanged);

	WiFi.setSleep(false);
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

    xTaskCreatePinnedToCore(
        sppTaskFn,   /* Function to implement the task */
        "BT SPP task", /* Name of the task */
        8192,                 /* Stack size in words */
        NULL,                 /* Task input parameter */
        tskIDLE_PRIORITY + 1,     /* More than background tasks */
        &sppTask,    /* Task handle. */
        xPortGetCoreID());

    logger.log(Logger::DEBUG, "setup() running on core %d", xPortGetCoreID());

    vTaskDelete(NULL);	// Delete this task (so loop() won't be called)
}

void loop() {
  // put your main code here, to run repeatedly:
}
