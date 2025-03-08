#ifndef _UPTIME_H
#define _UPTIME_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

extern const char* TIME_FLIES_TAG;

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
    
#endif