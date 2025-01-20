#ifndef BT_BTSPP_H
#define BT_BTSPP_H
#include <esp_bt_defs.h>
#include <esp_gap_bt_api.h>
#include <esp_spp_api.h>
#include <string>

#define MAX_STRING_LENGTH 200

class BTSPP {
public:
    BTSPP(const std::string& name);

    bool init();
    bool inited() { return initDone; }
    void startConnection(uint8_t *address);
    bool connectionDone() { return connectDone; }
    void write(const std::string& msg);

    bool isError() { return err != ESP_OK; }
    const std::string& getErrMessage() { return errMsg; }

private:
    void btSPPCallback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param);

    static void btSPPCallbackC(esp_spp_cb_event_t event, esp_spp_cb_param_t *param);

    static BTSPP *self;

    esp_spp_sec_t sec_mask = ESP_SPP_SEC_AUTHENTICATE;
    esp_spp_role_t role_master = ESP_SPP_ROLE_MASTER;
    esp_bd_addr_t address = {0};

    std::string name;
    bool initDone = false;
    bool connectDone = false;
    uint32_t peerHandle = 0;
    char writeBuf[MAX_STRING_LENGTH];
    char *bufPtr;
    bool writeDone = true;

    esp_err_t err;
    std::string errMsg;
};

#endif