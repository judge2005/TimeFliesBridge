#ifndef BT_GAP_H
#define BT_GAP_H
#include <esp_bt_defs.h>
#include <esp_gap_bt_api.h>
#include <esp_spp_api.h>
#include <string>
#include <unordered_map>

class BTPeerInfo {
public:
    esp_bd_addr_t address = {0};
    std::string name;
};

class BTGAP {
public:
    bool init();
    bool startInquiry();
    bool inquiryDone();
    uint8_t*  getAddress(const char* name);
    bool isError() { return err != ESP_OK; }
    const std::string& getErrMessage() { return errMsg; }

private:
    bool getNameFromEIR(void *eir, char *nameOut, uint8_t *lenOut);

    void btGapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);

    static void btGapCallbackC(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);

    static BTGAP *self;

    bool done = false;
    esp_err_t err;

    esp_bt_inq_mode_t inqMode = ESP_BT_INQ_MODE_GENERAL_INQUIRY;
    uint8_t inqLen = 10;    // Run for 10 * 1.28 secs
    uint8_t inqNumResp = 0; // Handle any number of responses

    std::string errMsg;
    std::unordered_map<std::string, BTPeerInfo> peers;
};

#endif