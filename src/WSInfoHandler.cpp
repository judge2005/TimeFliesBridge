#include <WSInfoHandler.h>
// #include <Uptime.h>
#include <ArduinoJson.h>
#include <AsyncWebSocket.h>
extern "C" {
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "esp_heap_caps.h"
}

static uint32_t sketchSize(sketchSize_t response) {
    esp_image_metadata_t data;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return 0;
    const esp_partition_pos_t running_pos  = {
        .offset = running->address,
        .size = running->size,
    };
    data.start_addr = running_pos.offset;
    esp_image_verify(ESP_IMAGE_VERIFY, &running_pos, &data);
    if (response) {
        return running_pos.size - data.image_len;
    } else {
        return data.image_len;
    }
}
    
void WSInfoHandler::handle(AsyncWebSocketClient *client, const char *data) {
	cbFunc();

	// static Uptime uptime;
	JsonDocument doc;

	doc["type"] = "sv.init.info";
	size_t freeHeap = ESP.getFreeHeap();
	size_t free8bitHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
	size_t freeIRAMHeap = freeHeap - free8bitHeap;

	doc["value"]["esp_free_iram_heap"] = freeIRAMHeap;
	doc["value"]["esp_free_heap"] = free8bitHeap;
	doc["value"]["esp_free_heap_min"] = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
	doc["value"]["esp_max_alloc_heap"] = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
	doc["value"]["esp_sketch_size"] = sketchSize(SKETCH_SIZE_TOTAL);
	doc["value"]["esp_sketch_space"] = sketchSize(SKETCH_SIZE_FREE);

	doc["value"]["esp_chip_id"] = String(ESP.getChipRevision(), HEX);

	doc["value"]["wifi_ip_address"] = WiFi.localIP().toString();
	doc["value"]["wifi_mac_address"] = WiFi.macAddress();
	doc["value"]["wifi_ssid"] = WiFi.SSID();
	doc["value"]["wifi_ap_ssid"] = ssid;
	doc["value"]["hostname"] = hostname;
	doc["value"]["software_revision"] = revision;

    doc["value"]["fs_size"] = fsSize;
    doc["value"]["fs_free"] = fsFree;
	doc["value"]["brightness"] = brightness;
	doc["value"]["triggered"] = triggered;
	doc["value"]["clock_on"] = clockOn;

	// value["up_time"] = uptime.uptime();
	doc["value"]["sync_time"] = lastUpdateTime;
	doc["value"]["sync_failed_msg"] = lastFailedMessage;
	doc["value"]["sync_failed_cnt"] = failedCount;

	// if (pBlankingMonitor) {
	// 	value["on_time"] = pBlankingMonitor->onTime();
	// 	value["off_time"] = pBlankingMonitor->offTime();
	// }

	String serializedJSON;
	serializeJson(doc, serializedJSON);
	client->text(serializedJSON);
}


