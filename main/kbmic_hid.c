// main/kbmic_hid.c —— BLE HID keyboard (8-byte keyboard + Consumer Control map).
#include "kbmic_hid.h"
#include "kbmic_config.h"
#include "kbmic_ble_svc.h"
#include "kbmic_store.h"

#include <inttypes.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"
#include "esp_hid_common.h"
#include "esp_hidd.h"
#include "esp_hidd_gatts.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define KBMIC_BLE_DEVICE_NAME KBMIC_BT_NAME
#define KBMIC_BLE_MANUFACTURER "FoloToy"
#define KBMIC_BLE_SERIAL "AIKB0001"
#define KBMIC_TAP_HOLD_MS 35
#define KBMIC_MAP_KEYBOARD 0
#define KBMIC_MAP_CONSUMER 1

static const char *TAG = "kbmic_hid";

// 键盘输入布局固定 8 字节:[modifier][Apple Fn][6 keycodes]。
static const uint8_t s_map_keyboard[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, // Generic Desktop / Keyboard / Application
    0x05, 0x07,                           // Keyboard/Keypad Usage Page
    0x19, 0xE0, 0x29, 0xE7,              // modifier usage range
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, // 8 modifier bits
    0x05, 0xFF, 0x09, 0x03,              // Apple Top Case / KeyboardFn
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x02, // byte 1 = Fn
    0x05, 0x07,                           // restore standard key usage page!
    0x95, 0x06, 0x75, 0x08,
    0x15, 0x00, 0x25, 0xDD, 0x19, 0x00, 0x29, 0xDD,
    0x81, 0x00,                           // six keycodes
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
    0x75, 0x01, 0x95, 0x05, 0x91, 0x02, // LED output
    0x75, 0x03, 0x95, 0x01, 0x91, 0x03,
    0xC0,
};

// Consumer 控制图:自定义快捷键仍可配置 Globe (0x029D) 与媒体键。
static const uint8_t s_map_consumer[] = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01,
    0x15, 0x00, 0x26, 0xFF, 0x03,
    0x19, 0x00, 0x2A, 0xFF, 0x03,
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    {.data = s_map_keyboard, .len = sizeof(s_map_keyboard)},
    {.data = s_map_consumer, .len = sizeof(s_map_consumer)},
};

static esp_hid_device_config_t s_hid_config = {
    .vendor_id = 0x05AC,
    .product_id = 0x024F,
    .version = 0x0100,
    .device_name = KBMIC_BLE_DEVICE_NAME,
    .manufacturer_name = KBMIC_BLE_MANUFACTURER,
    .serial_number = KBMIC_BLE_SERIAL,
    .report_maps = s_report_maps,
    .report_maps_len = sizeof(s_report_maps) / sizeof(s_report_maps[0]),
};

static esp_hidd_dev_t *s_hid_dev;
static bool s_connected;
static bool s_boot_notify;
// Legacy advertising packet: Flags + HID Service UUID + Keyboard Appearance.
static uint8_t s_adv_raw[] = {
    0x02, 0x01, 0x06,        // Flags: General Discoverable, BR/EDR unsupported
    0x03, 0x03, 0x12, 0x18,  // Complete List of 16-bit Service UUIDs: HID
    0x03, 0x19, 0xC1, 0x03,  // Appearance: Keyboard
};
// Scan response: Complete Local Name "AI小键盘" (UTF-8).
static uint8_t s_scan_rsp_raw[] = {
    0x0C, 0x09, 'A', 'I',
    0xE5, 0xB0, 0x8F,        // 小
    0xE9, 0x94, 0xAE,        // 键
    0xE7, 0x9B, 0x98,        // 盘
};
static uint8_t s_raw_config_done;
static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x30,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void gap_adv_start(void)
{
    s_raw_config_done = 0x03;
    const esp_err_t adv_ret = esp_ble_gap_config_adv_data_raw(
        s_adv_raw, sizeof(s_adv_raw));
    const esp_err_t scan_ret = esp_ble_gap_config_scan_rsp_data_raw(
        s_scan_rsp_raw, sizeof(s_scan_rsp_raw));
    ESP_LOGI(TAG, "config raw adv ret=%s scan-rsp=%s",
             esp_err_to_name(adv_ret), esp_err_to_name(scan_ret));
    if (adv_ret == ESP_OK && scan_ret == ESP_OK) return;
    if (adv_ret != ESP_OK || scan_ret != ESP_OK) {
        ESP_LOGE(TAG, "raw advertising configuration failed; not advertising");
    }
}

static void gap_ble_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        s_raw_config_done &= ~0x01;
        if (!s_raw_config_done) {
            (void)esp_ble_gap_start_advertising(&s_adv_params);
        }
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        s_raw_config_done &= ~0x02;
        if (!s_raw_config_done) {
            (void)esp_ble_gap_start_advertising(&s_adv_params);
        }
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        ESP_LOGI(TAG, "配对%s", param->ble_security.auth_cmpl.success ? "成功" : "失败");
        break;
    case ESP_GAP_BLE_NC_REQ_EVT:
        ESP_LOGI(TAG, "配对码 %" PRIu32 ",自动确认", param->ble_security.key_notif.passkey);
        esp_ble_confirm_reply(param->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
        ESP_LOGW(TAG, "主机请求输入配对码:%" PRIu32, param->ble_security.key_notif.passkey);
        break;
    default:
        break;
    }
}

static esp_err_t gap_stack_init(void)
{
    esp_err_t ret;
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_IO;
    uint8_t enc_key_mask = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t key_size = 16;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) return ret;
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) return ret;
    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ret = esp_bluedroid_init_with_cfg(&bd_cfg);
    if (ret != ESP_OK) return ret;
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) return ret;

    if ((ret = esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, 1)) != ESP_OK ||
        (ret = esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, 1)) != ESP_OK ||
        (ret = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &enc_key_mask, 1)) != ESP_OK ||
        (ret = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &enc_key_mask, 1)) != ESP_OK ||
        (ret = esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, 1)) != ESP_OK) {
        ESP_LOGE(TAG, "BLE 安全参数设置失败: %s", esp_err_to_name(ret));
        return ret;
    }
    if ((ret = esp_ble_gap_register_callback(gap_ble_event)) != ESP_OK) return ret;
    return esp_ble_gap_set_device_name(KBMIC_BLE_DEVICE_NAME);
}

static void hid_event_cb(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_data;
    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "HID 就绪，广播 %s", KBMIC_BLE_DEVICE_NAME);
        gap_adv_start();
        kbmic_ble_svc_request_register();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        s_connected = true;
        ESP_LOGI(TAG, "主机已连接");
        if (s_boot_notify) {
            const kbmic_config_t *cfg = kbmic_config_current();
            kbmic_ble_svc_notify(KBMIC_EV_BOOT, cfg->active, 0,
                                 cfg->profiles[cfg->active].name);
        }
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        s_connected = false;
        ESP_LOGI(TAG, "主机断开，重新广播");
        gap_adv_start();
        break;
    default:
        break;
    }
}

esp_err_t kbmic_hid_init(void)
{
    esp_err_t ret = gap_stack_init();
    if (ret != ESP_OK) return ret;
    ret = kbmic_ble_svc_install_dispatch();
    if (ret != ESP_OK) return ret;
    ret = esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BLE, hid_event_cb, &s_hid_dev);
    if (ret == ESP_OK) s_boot_notify = true;
    return ret;
}

esp_err_t kbmic_hid_stop_for_provisioning(void)
{
    // 先撤 HID 设备；它会断开主机并清理 HIDD 的 GATT 接口。
    if (s_hid_dev) {
        (void)esp_hidd_dev_deinit(s_hid_dev);
        s_hid_dev = NULL;
    }
    s_connected = false;
    s_boot_notify = false;

    // Bluedroid/controller 的顺序必须自上而下；这里按 ESP-IDF 的 BLE-only
    // 释放示例执行。esp_bt_mem_release 不可逆，恢复键盘靠配网完成后重启。
    (void)esp_bluedroid_disable();
    (void)esp_bluedroid_deinit();
    (void)esp_bt_controller_disable();
    (void)esp_bt_controller_deinit();
    const esp_err_t ret = esp_bt_mem_release(ESP_BT_MODE_BLE);
    ESP_LOGW(TAG, "BLE unloaded for provisioning: mem_release=%s",
             esp_err_to_name(ret));
    return ret;
}

bool kbmic_hid_connected(void)
{
    return s_connected && s_hid_dev != NULL && esp_hidd_dev_connected(s_hid_dev);
}

esp_err_t kbmic_hid_key_hold(uint8_t modifier, uint8_t keycode, bool pressed)
{
    if (s_hid_dev == NULL || !kbmic_hid_connected()) return ESP_ERR_INVALID_STATE;
    uint8_t report[8] = {0};
    if (pressed) {
        report[0] = modifier;
        report[2] = keycode;
    }
    return esp_hidd_dev_input_set(s_hid_dev, KBMIC_MAP_KEYBOARD, 0, report, sizeof(report));
}

esp_err_t kbmic_hid_applefn(bool pressed)
{
    if (s_hid_dev == NULL || !kbmic_hid_connected()) return ESP_ERR_INVALID_STATE;
    uint8_t report[8] = {0};
    if (pressed) report[1] = 0x01;
    return esp_hidd_dev_input_set(s_hid_dev, KBMIC_MAP_KEYBOARD, 0, report, sizeof(report));
}

esp_err_t kbmic_hid_consumer(uint16_t usage, bool pressed)
{
    if (s_hid_dev == NULL || !kbmic_hid_connected()) return ESP_ERR_INVALID_STATE;
    uint8_t report[2] = {0};
    if (pressed) {
        report[0] = (uint8_t)(usage & 0xFF);
        report[1] = (uint8_t)(usage >> 8);
    }
    return esp_hidd_dev_input_set(s_hid_dev, KBMIC_MAP_CONSUMER, 0, report, sizeof(report));
}

esp_err_t kbmic_hid_tap(uint8_t modifier, uint8_t keycode)
{
    esp_err_t ret = kbmic_hid_key_hold(modifier, keycode, true);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(KBMIC_TAP_HOLD_MS));
    return kbmic_hid_key_hold(0, 0, false);
}

esp_err_t kbmic_hid_reset_bonds(void)
{
    int count = 0;
    if (esp_ble_get_bond_device_list(&count, NULL) != ESP_OK || count <= 0) count = 0;
    esp_ble_bond_dev_t bonds[8];
    if (count > (int)(sizeof(bonds) / sizeof(bonds[0])))
        count = (int)(sizeof(bonds) / sizeof(bonds[0]));
    if (count && esp_ble_get_bond_device_list(&count, bonds) != ESP_OK) count = 0;
    int removed = 0;
    for (int i = 0; i < count; i++) {
        if (esp_ble_remove_bond_device(bonds[i].bd_addr) == ESP_OK) removed++;
    }
    (void)esp_ble_gap_stop_advertising();
    vTaskDelay(pdMS_TO_TICKS(100));
    gap_adv_start();
    ESP_LOGW(TAG, "已清除 %d 个 BLE bond，设备重新广播", removed);
    return ESP_OK;
}
