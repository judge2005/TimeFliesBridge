#include "esp_log.h"
#include "Logger.h"

#define LOG_ENTRY_SIZE 100
#define MAX_LOG_ENTRIES 40

extern const char* TIME_FLIES_TAG;

void Logger::setUpdateCallback(std::function<void(const JsonDocument&)> updateCallback) {
    this->updateCallback = updateCallback;
}

void Logger::log(LogLevel lvl, const char *format, ...) {
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
    
String Logger::escape_json(const char *s) {
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
    
String Logger::getSerializedJsonLog() {
    char *comma = ",";
    String s = "\"console_data\":[";
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
    
void Logger::broadcastUpdate() {
    if (numLogEntries > 0) {
        JsonDocument doc;
        doc["type"] = "sv.update";
        for (int count=0, index=startLogIndex; count < numLogEntries; count++, index = (index+1) % MAX_LOG_ENTRIES) {    
            doc["value"]["console_data"][count] = logBuffer[index];
        }

        updateCallback(doc);
    }
}
