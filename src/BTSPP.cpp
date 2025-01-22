#include <BTSPP.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp32-hal-bt.h>
#include <string.h>

#define BT_SPP_TAG "BT_SPP"

BTSPP *BTSPP::self = 0;

BTSPP::BTSPP(const std::string& name) {
    this->name = name;
}

bool BTSPP::init() {
    if (self == 0) {
        ESP_LOGD(BT_SPP_TAG, "Initializing SPP");
        self = this;
        err = 0;
        err = ESP_OK;
        initDone = false;

        btStart();

        esp_bt_mode_t esp_bt_mode;
        esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        cfg.mode = ESP_BT_MODE_CLASSIC_BT;
        esp_bt_controller_mem_release(ESP_BT_MODE_BLE);

        if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
            if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
                if ((err = esp_bt_controller_init(&cfg)) != ESP_OK) {
                    log_e("initialize controller failed: %s", esp_err_to_name(err));
                    errMsg = "initialize controller failed: ";
                    errMsg += esp_err_to_name(err);
                    return false;
                }
                while (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {}
            }
            if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
                if ((err = esp_bt_controller_enable(esp_bt_mode)) != ESP_OK) {
                    log_e("BT Enable failed %s", esp_err_to_name(err));
                    errMsg = "BT Enable failed: ";
                    errMsg += esp_err_to_name(err);
                    return false;
                }
            }
            if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
                errMsg = "Controller not enabled: ";
                errMsg += esp_bt_controller_get_status();
                return false;
            }
        }

        if ((err = esp_bluedroid_init()) != ESP_OK) {
            errMsg = "Bluedroid initialization failed: ";
            errMsg += esp_err_to_name(err);
            return false;
        }

        if ((err = esp_bluedroid_enable()) != ESP_OK) {
            errMsg = "Failed to enable bluedroid: ";
            errMsg += esp_err_to_name(err);
            return false;
        }

        ESP_LOGD(BT_SPP_TAG, "Enabled bluedroid");

        if ((err = esp_bt_dev_set_device_name(name.c_str())) != ESP_OK) {
            errMsg = "Failed to set device name: ";
            errMsg += esp_err_to_name(err);
            return false;
        }

        if ((err = esp_spp_register_callback(btSPPCallbackC)) != ESP_OK) {
            errMsg = "Failed to register callback: ";
            errMsg += esp_err_to_name(err);
            return false;
        }
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        esp_spp_cfg_t bt_spp_cfg = {
            .mode = ESP_SPP_MODE_CB,
            .enable_l2cap_ertm = true,
        };

        if ((err = esp_spp_enhanced_init(&bt_spp_cfg)) != ESP_OK) {
            errMsg = esp_err_to_name(err);
            return false;
        }
#else
        if ((err = esp_spp_init(ESP_SPP_MODE_CB)) != ESP_OK) {
            errMsg = "Failed to init SPP: ";
            errMsg += esp_err_to_name(err);
            return false;
        }
#endif
        ESP_LOGD(BT_SPP_TAG, "Inited SPP");
    }

    ESP_LOGD(BT_SPP_TAG, "SPP init returning true");
    return true;
}

static char *bda2str(uint8_t * bda, char *str, size_t size)
{
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }

    uint8_t *p = bda;
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            p[0], p[1], p[2], p[3], p[4], p[5]);
    return str;
}

void BTSPP::startConnection(uint8_t *address) {
    connectDone = false;
    /*
    * Set default parameters for Legacy Pairing
    * Use variable pin, input pin code when pairing
    */
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code;
    esp_bt_gap_set_pin(pin_type, 0, pin_code);

    memcpy(this->address, address, ESP_BD_ADDR_LEN);
    char bda_str [18];

    bda_str[0] = 'b';

    bda2str(address, bda_str, sizeof(bda_str));
    ESP_LOGD(BT_SPP_TAG, "Connecting to %s", bda_str);

    if ((err = esp_spp_start_discovery(address)) != ESP_OK) {
        errMsg = esp_err_to_name(err);
        return;
    }
}

void BTSPP::write(const std::string& msg) {
    if (msg.length() < MAX_STRING_LENGTH) {
        strcpy(writeBuf, msg.c_str());
        ESP_LOGD(BT_SPP_TAG, "Writing message '%s' of length %d to %d", writeBuf, strlen(writeBuf), peerHandle);
        writeDone = false;
        bufPtr = writeBuf;
        if ((err = esp_spp_write(peerHandle, msg.length(), (uint8_t*)writeBuf)) != ESP_OK) {
            errMsg = esp_err_to_name(err);
        }
    } else {
        err = -1;
        errMsg = "String is too long";
    }
}

void BTSPP::btSPPCallback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
    uint8_t i = 0;
    char bda_str[18] = {0};

    switch (event) {
    case ESP_SPP_INIT_EVT:
        if (param->init.status == ESP_SPP_SUCCESS) {
            ESP_LOGD(BT_SPP_TAG, "ESP_SPP_INIT_EVT");
            initDone=true;
        } else {
            ESP_LOGE(BT_SPP_TAG, "ESP_SPP_INIT_EVT status:%d", param->init.status);
        }
        break;

    case ESP_SPP_DISCOVERY_COMP_EVT:
        if (param->disc_comp.status == ESP_SPP_SUCCESS) {
            ESP_LOGD(BT_SPP_TAG, "ESP_SPP_DISCOVERY_COMP_EVT scn_num:%d", param->disc_comp.scn_num);
            for (i = 0; i < param->disc_comp.scn_num; i++) {
                ESP_LOGD(BT_SPP_TAG, "-- [%d] scn:%d service_name:%s", i, param->disc_comp.scn[i],
                         param->disc_comp.service_name[i]);
            }
            /* We only connect to the first found server on the remote SPP acceptor here */
            esp_spp_connect(sec_mask, role_master, param->disc_comp.scn[0], address);
        } else {
            ESP_LOGE(BT_SPP_TAG, "ESP_SPP_DISCOVERY_COMP_EVT status=%d", param->disc_comp.status);
        }
        break;
    case ESP_SPP_OPEN_EVT:
        if (param->open.status == ESP_SPP_SUCCESS) {
            ESP_LOGD(BT_SPP_TAG, "ESP_SPP_OPEN_EVT handle:%d", param->open.handle);
            connectDone = true;
            peerHandle = param->open.handle;
        } else {
            ESP_LOGE(BT_SPP_TAG, "ESP_SPP_OPEN_EVT status:%d", param->open.status);
        }
        break;
    case ESP_SPP_CLOSE_EVT:
        ESP_LOGD(BT_SPP_TAG, "ESP_SPP_CLOSE_EVT status:%d handle:%d close_by_remote:%d", param->close.status,
                 param->close.handle, param->close.async);
        connectDone = false;
        break;
    case ESP_SPP_START_EVT:
        ESP_LOGD(BT_SPP_TAG, "ESP_SPP_START_EVT");
        break;
    case ESP_SPP_CL_INIT_EVT:
        if (param->cl_init.status == ESP_SPP_SUCCESS) {
            ESP_LOGD(BT_SPP_TAG, "ESP_SPP_CL_INIT_EVT handle:%d sec_id:%d", param->cl_init.handle, param->cl_init.sec_id);
        } else {
            ESP_LOGE(BT_SPP_TAG, "ESP_SPP_CL_INIT_EVT status:%d", param->cl_init.status);
        }
        break;
    case ESP_SPP_DATA_IND_EVT:
        ESP_LOGD(BT_SPP_TAG, "ESP_SPP_DATA_IND_EVT");
        break;
    case ESP_SPP_WRITE_EVT:
        if (param->write.status == ESP_SPP_SUCCESS) {
            if (bufPtr[param->write.len] == 0) {
                writeDone = true;
                ESP_LOGD(BT_SPP_TAG, "Wrote all data, len=%d", param->write.len);
            } else {
                ESP_LOGD(BT_SPP_TAG, "Wrote %d bytes, cong=%d", param->write.len, param->write.cong);
                /*
                 * Means the previous data packet only sent partially due to lower layer congestion, resend the
                 * remainning data.
                 */
                bufPtr += param->write.len;
                if (!param->write.cong) {
                    /* The lower layer is not congested, you can send the next data packet now. */
                    if (*bufPtr == 0) {
                        writeDone = true;
                        ESP_LOGD(BT_SPP_TAG, "All data was really sent");
                        return;   // All sent anyway
                    }
                    ESP_LOGD(BT_SPP_TAG, "Attempting to write remainder");
                    esp_spp_write(peerHandle, strlen(bufPtr), (uint8_t*)bufPtr);
                }
            }
        } else {
            /* Means the prevous data packet is not sent at all, need to send the whole data packet again. */
            err = param->write.status;
            errMsg = "Write failed";
            ESP_LOGE(BT_SPP_TAG, "ESP_SPP_WRITE_EVT status:%d", param->write.status);
        }
        break;
    case ESP_SPP_CONG_EVT:
        ESP_LOGD(BT_SPP_TAG, "ESP_SPP_CONG_EVT cong:%d", param->cong.cong);
        if (param->cong.cong == 0) {
            /* Send the previous (partial) data packet or the next data packet. */
            if (*bufPtr == 0) {
                writeDone = true;
                return;
            }
            esp_spp_write(peerHandle, strlen(bufPtr), (uint8_t*)bufPtr);
        }
        break;
    case ESP_SPP_SRV_OPEN_EVT:
        ESP_LOGD(BT_SPP_TAG, "ESP_SPP_SRV_OPEN_EVT");
        break;
    case ESP_SPP_UNINIT_EVT:
        ESP_LOGD(BT_SPP_TAG, "ESP_SPP_UNINIT_EVT");
        break;
    default:
        break;
    }
}

void BTSPP::btSPPCallbackC(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
    self->btSPPCallback(event, param);
}
