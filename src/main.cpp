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

#include "time.h"
#include "sys/time.h"

#define TIME_FLIES_TAG "TIME_FLIES"

#define OTA
#define MAX_MSG_SIZE 40
#define SPP_QUEUE_SIZE 40

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

void broadcastUpdate(String originalKey, const BaseConfigItem& item);
void broadcastUpdate(const JsonDocument &doc);

class Uptime {
public:
	Uptime() {
		utMutex = xSemaphoreCreateMutex();
	}
	char *uptime();
	void loop();

private:
	SemaphoreHandle_t utMutex;
	unsigned long long rollover = 0;
	unsigned long lastMillis;
	char _return[32];
};

void Uptime::loop() {
	if (xSemaphoreTake(utMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
		ESP_LOGE(TIME_FLIES_TAG, "Failed to obtain utMutex");
		return;
	}

	unsigned long now = millis();
	if (lastMillis > now) {
		rollover++;
	}
	lastMillis = now;

	xSemaphoreGive(utMutex);
}

char *Uptime::uptime() {
	if (xSemaphoreTake(utMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
		ESP_LOGE(TIME_FLIES_TAG, "Failed to obtain utMutex");
		*_return = 0;
		return _return;
	}

	unsigned long long _now = (rollover << 32) + lastMillis;
	unsigned long secs = _now / 1000LL, mins = secs / 60;
	unsigned long hours = mins / 60, days = hours / 24;
	secs -= mins * 60;
	mins -= hours * 60;
	hours -= days * 24;
	sprintf(_return, "%d days %02dh %02dm %02ds", (int) days,
			(int) hours, (int) mins, (int) secs);

	xSemaphoreGive(utMutex);

	return _return;
}

Uptime uptime;

#define LOG_ENTRY_SIZE 100
#define MAX_LOG_ENTRIES 40

typedef enum {
	ERROR = 1,
	WARN,
	INFO,
	DEBUG,
	VERBOSE
} LogLevel;

class Logger {
public:
	static void log(LogLevel lvl, const char *format, ...) {
		va_list args;
		va_start(args, format);
		int tailIndex = (startLogIndex + numLogEntries) % MAX_LOG_ENTRIES;
		
		if (numLogEntries == MAX_LOG_ENTRIES) {
			startLogIndex = (startLogIndex + 1) % MAX_LOG_ENTRIES;
		}

		vsnprintf(logBuffer[tailIndex], LOG_ENTRY_SIZE, format, args);

		numLogEntries = min(++numLogEntries, MAX_LOG_ENTRIES);

		switch (lvl) {
			case ERROR:
				ESP_LOGE(TIME_FLIES_TAG, "%s", logBuffer[tailIndex]);
				break;

			case WARN:
				ESP_LOGW(TIME_FLIES_TAG, "%s", logBuffer[tailIndex]);
				break;

			case INFO:
				ESP_LOGI(TIME_FLIES_TAG, "%s", logBuffer[tailIndex]);
				break;

			case DEBUG:
				ESP_LOGD(TIME_FLIES_TAG, "%s", logBuffer[tailIndex]);
				break;

			case VERBOSE:
				ESP_LOGV(TIME_FLIES_TAG, "%s", logBuffer[tailIndex]);
				break;
		}

		ESP_LOGD(TIME_FLIES_TAG, "After log: startIndex=%d, tailIndex=%d, numEntries=%d", startLogIndex, tailIndex, numLogEntries);

		broadcastUpdate();

		va_end(args); 
	}

	static String escape_json(const char *s) {
		String ret("\"");
		ret.reserve(strlen(s) + 10);
		const char *start = s;
		const char *end = start + strlen(s);
		for (const char *c = start; c != end; c++) {
			switch (*c) {
			case '"': ret += "\\\""; break;
			case '\\': ret += "\\\\"; break;
			case '\b': ret += "\\b"; break;
			case '\f': ret += "\\f"; break;
			case '\n': ret += "\\n"; break;
			case '\r': ret += "\\r"; break;
			case '\t': ret += "\\t"; break;
			default:
				if ('\x00' <= *c && *c <= '\x1f') {
					char buf[10];
					sprintf(buf, "\\u%04x", (int)*c);
					ret += buf;
				} else {
					ret += *c;
				}
			}
		}
		ret += "\"";

		return ret;
	}

	static String getSerializedJsonLog() {
		char *comma = ",";
		String s = ",\"console_data\":[";
		char *sep = "";
		if (numLogEntries > 0) {
			for (int count=0, index=startLogIndex; count < numLogEntries; count++, index = (index+1) % MAX_LOG_ENTRIES) {
				s += sep;
				s += escape_json(logBuffer[index]);
				sep = comma;
			}

			s += "]";
		}
		return s;
	}

	static void broadcastUpdate() {
		if (numLogEntries > 0) {
			JsonDocument doc;
			doc["type"] = "sv.update";
			for (int count=0, index=startLogIndex; count < numLogEntries; count++, index = (index+1) % MAX_LOG_ENTRIES) {
		
				doc["value"]["console_data"][count] = logBuffer[index];
			}

			::broadcastUpdate(doc);
		}
	}

private:
	static int startLogIndex;
	static int numLogEntries;
	static char logBuffer[MAX_LOG_ENTRIES][LOG_ENTRY_SIZE];
};

int Logger::startLogIndex = 0;
int Logger::numLogEntries = 0;
char Logger::logBuffer[MAX_LOG_ENTRIES][LOG_ENTRY_SIZE] = {};

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
	{CONNECTED, "CONNECTED"},
	{SEARCHING, "SEARCHING"},
	{DISCONNECTING, "DISCONNECTING"}
};

StringConfigItem hostName("hostname", 63, "timefliesbridge");

AsyncWebServer server(80);
AsyncWebSocket ws("/ws"); // access at ws://[esp ip]/ws
DNSServer dns;
AsyncWiFiManager wifiManager(&server, &dns);
ASyncOTAWebUpdate otaUpdater(Update, "update", "secretsauce");
AsyncWiFiManagerParameter *hostnameParam;
EspSNTPTimeSync *timeSync;
TimeFliesClock timeFliesClock;
int tzOffset = 0;

TaskHandle_t commitEEPROMTask;
TaskHandle_t sppTask;
TaskHandle_t clockTask;
TaskHandle_t wifiManagerTask;

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

BaseConfigItem* rootConfigSet[] = {
    &globalConfig,
	&clockConfig,
	&ledsConfig,
	&extraConfig,
    0
};

CompositeConfigItem rootConfig("root", 0, rootConfigSet);

EEPROMConfig config(rootConfig);

// Declare some functions
void setWiFiCredentials(const char *ssid, const char *password);
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

void calculateTZOffset() {
	const time_t epoch_plus_11h = 60 * 60 * 11;
	const int local_time = localtime(&epoch_plus_11h)->tm_hour;
	const int gm_time = gmtime(&epoch_plus_11h)->tm_hour;
	tzOffset = local_time - gm_time;
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

void asyncTimeSetCallback(String time) {
	ESP_LOGD(TIME_FLIES_TAG, "Time: %s", time.c_str());

	struct tm now;
	suseconds_t uSec;

	timeSync->getLocalTime(&now, &uSec);
	calculateTZOffset();

	char msg[128] = {0};
	// Set timezone offset - 0x13,$PSU,6,4,20,6*** values 1-12 are added, 13-23 are subtracted -12 so 13 becomes -1.
	// EST                   0x13,$PSU,6,4,17,6*** i.e. TZ offset = 12 - 17 = -5
	int tzo = tzOffset;
	if (tzOffset < 0) {
		tzo = 12 - tzOffset;
	}

	//	"0x13,$TIM,22,40,45,16,01,25***"
	snprintf(msg, 128, "0x13,$PSU,6,4,%d,6***;0x13,$TIM,%2.2d,%2.2d,%2.2d,%2.2d,%2.2d,%2.2d***",
		tzo, now.tm_hour, now.tm_min, now.tm_sec, now.tm_mday, now.tm_mon, now.tm_year);
	
	sendCommands(msg);
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
	calculateTZOffset();

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

#define RXD 16
#define TXD 17
#define COMMAND_PIN 18
String OK_RESPONSE("OK\r");

bool verifySPPCommand(String command) {
	Serial1.println(command);
	String response = Serial1.readStringUntil('\n');
	bool ret = response.equals(OK_RESPONSE);
	if (!ret) {
		ESP_LOGE(TIME_FLIES_TAG, "Unexpected response: %s", response.c_str());
	}

	return ret;
}

void getSPPState() {
	Serial1.println("AT+STATE");
	String result = Serial1.readStringUntil('\n');
	int status = result.charAt(0) - '0';
	if (status >= 0 && status <= 9 && connectionStatus != status) {
		connectionStatus = (SPPConnectionState)status;
		Logger::log(INFO, "SPP Connection status=%s", state2string[connectionStatus].c_str());
	}
	// Read OK\r\n
	result = Serial1.readStringUntil('\n');
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

	while(true) {
		BaseType_t result = xQueuePeek(sppQueue, msg, pdMS_TO_TICKS(maxWait));
		uptime.loop();

		getSPPState();

		if (result == pdTRUE) {
			if (connectionStatus == CONNECTED) {
				// If we are connected, just drain the queue
				lastConnectedTime = millis();
				xQueueReceive(sppQueue, msg, 0);
				delay(delayNextMsg);
				Logger::log(INFO, "Sending command %s", msg);
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
#ifdef DISCONNECT_ON_DILE
			if (millis() - lastConnectedTime > 30000) {
				Logger::log(INFO, "Disconnecting");
				closeConnection();
			}
#endif
		}

		if (connectionStatus == NOT_CONNECTED) {
			initiateConnection();
		}
	}
}

String* items[] {
	&WSMenuHandler::clockMenu,
	&WSMenuHandler::ledsMenu,
	&WSMenuHandler::extraMenu,
	&WSMenuHandler::infoMenu,
	0
};

WSMenuHandler wsMenuHandler(items);
WSConfigHandler wsClockHandler(rootConfig, "clock");
WSConfigHandler wsLEDsHandler(rootConfig, "leds");
WSConfigHandler wsExtrasHandler(rootConfig, "extra", Logger::getSerializedJsonLog);
WSInfoHandler wsInfoHandler(infoCallback);

// Order of this needs to match the numbers in WSMenuHandler.cpp
WSHandler* wsHandlers[] {
	&wsMenuHandler,
	&wsClockHandler,
	&wsLEDsHandler,
	&wsExtrasHandler,
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
		} else if (_key == "wifi_ap") {
			setWiFiAP(value == "true" ? true : false);
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

	pinMode(COMMAND_PIN, OUTPUT);
  	Serial1.begin(38400, SERIAL_8N1, RXD, TXD);

	wsMutex = xSemaphoreCreateMutex();
    sppQueue = xQueueCreate(SPP_QUEUE_SIZE, MAX_MSG_SIZE);
 
	createSSID();

	EEPROM.begin(2048);
	initFromEEPROM();

	LittleFS.begin();

	calculateTZOffset();
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

    Logger::log(DEBUG, "setup() running on core %d", xPortGetCoreID());

    // vTaskDelete(NULL);	// Delete this task (so loop() won't be called)
}

void loop() {
  // put your main code here, to run repeatedly:
}
