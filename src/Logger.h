#ifndef _LOGGER_H
#define _LOGGER_H

#include <Arduino.h>
#include <ArduinoJson.h>


#define LOG_ENTRY_SIZE 100
#define MAX_LOG_ENTRIES 40

class Logger {
public:
    typedef enum {
        ERROR = 1,
        WARN,
        INFO,
        DEBUG,
        VERBOSE
    } LogLevel;
        
    void log(LogLevel lvl, const char *format, ...);    
    String getSerializedJsonLog();
    void setUpdateCallback(std::function<void(const JsonDocument&)> updateCallback);

private:
    void broadcastUpdate();
    String escape_json(const char *s);

    std::function<void(const JsonDocument &doc)> updateCallback;
    int startLogIndex = 0;
    int numLogEntries = 0;
    char logBuffer[MAX_LOG_ENTRIES][LOG_ENTRY_SIZE] = {};
};

#endif