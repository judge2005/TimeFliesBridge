#include <BTGAP.h>
#include <string.h>
#include <esp32-hal-log.h>

#define BT_GAP_TAG "BT_GAP"

BTGAP *BTGAP::self = 0;

bool BTGAP::init() {
    if (self == 0) {
        ESP_LOGD(BT_GAP_TAG, "Initializing GAP");
        self = this;
        done = false;
        err = 0;
        peers.clear();
        err = ESP_OK;

        if ((err = esp_bt_gap_register_callback(btGapCallbackC)) != ESP_OK) {
            errMsg = "Failed to register GAP callback: ";
            errMsg += esp_err_to_name(err);
            return false;
        }
    }

    return true;
}

bool BTGAP::startInquiry() {
    ESP_LOGD(BT_GAP_TAG, "Starting inquiry");
    done = false;
    if ((err = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE)) != ESP_OK) {
        errMsg = esp_err_to_name(err);
        return false;
    }

    if ((err = esp_bt_gap_start_discovery(inqMode, inqLen, inqNumResp)) != ESP_OK) {
        errMsg = esp_err_to_name(err);
        return false;
    }

    return true;
}

bool BTGAP::inquiryDone() {
    return done;
}

uint8_t* BTGAP::getAddress(const char* name) {
    if (peers.find(name) != peers.end()) {
        return peers[name].address;
    } else {
        return 0;
    }
}

void BTGAP::btGapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    char bda_str[18] = {0};
    uint8_t peer_bdname_len;
    char peer_bdname[ESP_BT_GAP_MAX_BDNAME_LEN + 1];

    switch(event) {
    case ESP_BT_GAP_DISC_RES_EVT:
        // ESP_LOGD(BT_GAP_TAG, "ESP_BT_GAP_DISC_RES_EVT");
        /* Find the target peer device name in the EIR data */
        for (int i = 0; i < param->disc_res.num_prop; i++) {
            BTPeerInfo peer;
            // ESP_LOGD(BT_GAP_TAG, "type=%d", param->disc_res.prop[i].type);
            if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_BDNAME) {           
                peer_bdname_len = param->disc_res.prop[i].len;
                memcpy(peer_bdname, param->disc_res.prop[i].val, peer_bdname_len);
                peer_bdname[peer_bdname_len] = 0;
                peer.name = peer_bdname;
                memcpy(peer.address, param->disc_res.bda, ESP_BD_ADDR_LEN);
                peers[peer.name] = peer;
                ESP_LOGD(BT_GAP_TAG, "name='%s'", peer.name.c_str());
            }
            if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_EIR
                && getNameFromEIR(param->disc_res.prop[i].val, peer_bdname, &peer_bdname_len)) {
                memcpy(peer.address, param->disc_res.bda, ESP_BD_ADDR_LEN);
                peer.name = peer_bdname;
                peers[peer.name] = peer;
                ESP_LOGD(BT_GAP_TAG, "name='%s'", peer.name.c_str());
           }
        }
        break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        ESP_LOGD(BT_GAP_TAG, "ESP_BT_GAP_DISC_STATE_CHANGED_EVT state:%d", param->disc_st_chg.state);
        done = param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED;
        break;
    case ESP_BT_GAP_RMT_SRVCS_EVT:
        ESP_LOGD(BT_GAP_TAG, "ESP_BT_GAP_RMT_SRVCS_EVT");
        break;
    case ESP_BT_GAP_RMT_SRVC_REC_EVT:
        ESP_LOGD(BT_GAP_TAG, "ESP_BT_GAP_RMT_SRVC_REC_EVT");
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:{
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGD(BT_GAP_TAG, "authentication success: %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGE(BT_GAP_TAG, "authentication failed, status:%d", param->auth_cmpl.stat);
        }
        break;
    }
    case ESP_BT_GAP_PIN_REQ_EVT:{
        ESP_LOGD(BT_GAP_TAG, "ESP_BT_GAP_PIN_REQ_EVT min_16_digit:%d", param->pin_req.min_16_digit);
        if (param->pin_req.min_16_digit) {
            ESP_LOGD(BT_GAP_TAG, "Input pin code: 0000 0000 0000 0000");
            esp_bt_pin_code_t pin_code = {0};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
        } else {
            ESP_LOGD(BT_GAP_TAG, "Input pin code: 1234");
            esp_bt_pin_code_t pin_code;
            pin_code[0] = '1';
            pin_code[1] = '2';
            pin_code[2] = '3';
            pin_code[3] = '4';
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        }
        break;
    }

    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGD(BT_GAP_TAG, "ESP_BT_GAP_MODE_CHG_EVT mode:%d", param->mode_chg.mode);
        break;

    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
        ESP_LOGD(BT_GAP_TAG, "ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT");
        break;

    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
        ESP_LOGD(BT_GAP_TAG, "ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT");
        break;

    default:
        ESP_LOGD(BT_GAP_TAG, "Unknown event: %d", event);
        break;
    }
}

bool BTGAP::getNameFromEIR(void *eir, char *nameOut, uint8_t *lenOut) {
    uint8_t *rmt_bdname = NULL;
    uint8_t rmt_bdname_len = 0;

    if (!eir) {
        return false;
    }

    rmt_bdname = esp_bt_gap_resolve_eir_data((uint8_t*)eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
    if (!rmt_bdname) {
        rmt_bdname = esp_bt_gap_resolve_eir_data((uint8_t*)eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
    }

    if (rmt_bdname) {
        if (rmt_bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN) {
            rmt_bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;
        }

        if (nameOut) {
            memcpy(nameOut, rmt_bdname, rmt_bdname_len);
            nameOut[rmt_bdname_len] = '\0';
        }
        if (lenOut) {
            *lenOut = rmt_bdname_len;
        }
        return true;
    }

    return false;
}


void BTGAP::btGapCallbackC(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    self->btGapCallback(event, param);
}
