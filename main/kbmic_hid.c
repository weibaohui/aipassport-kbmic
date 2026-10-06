// main/kbmic_hid.c —— BLE HID 设备实现。
//
// 分三层:
//   1. esp_hid 组件的 BLE 传输层负责 HID 服务(0x1812)、报告特征与电量服务;
//   2. 本文件自带的 GAP 薄层负责 BT controller / Bluedroid 启动、配对与广播
//      (官方例程里叫 esp_hid_gap.c,但那份同时含经典蓝牙扫描与 HID host,
//       本应用只需要广播这一侧,所以只取必要部分自己写);
//   3) 对外的发送接口,内部把 8 字节键盘报告 / 2 字节 Consumer 报告发到
//      各自那张 report map。
#include "kbmic_hid.h"

#include "kbmic_config.h"
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
#include "kbmic_ble_svc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 设备名即产品名,用户在手机/电脑的蓝牙列表里看到的就是它。
// 定义在 kbmic_config.h —— 那里是配置模型的单一定义处,别在这里再抄一份。
#define KBMIC_BLE_DEVICE_NAME KBMIC_BT_NAME
#define KBMIC_BLE_MANUFACTURER "FoloToy"
#define KBMIC_BLE_SERIAL "AIKB0001"

// 设备名 "AI小键盘" 的 UTF-8 是 11 字节,加上 flags + appearance 会超过
// 31 字节的广播包。所以广播包只放 16 位 HID 服务 UUID(0x1812),名字走
// scan response。
//
// ⚠️ esp_ble_gap_config_adv_data 的 service_uuid 字段有两个反直觉的约定:
//   1. p_service_uuid 是**每 16 字节一格**的数组,步长固定 LEN_UUID_128,
//      不是紧凑的 2 字节数组。下游用 btc128_to_bta_uuid() 逐格解析,
//      service_uuid_len 必须是 16 的倍数。
//   2. 16 位与否由 uuidType() 判定:非 [12][13] 的 12 个字节必须与标准
//      BASE_UUID 完全一致才判成 16 位(此时 uuid16 = [13]<<8|[12])。
//      把值放在 [0][1] 会被判成 128 位自定义 UUID → 18 字节 AD 字段,
//      广播包超 31 字节,BTM 只写入部分数据且广播出去的是错误 UUID
//      (2026-10-05 真机踩坑,日志表现为 "Partial data write into ADV")。
// 所以下面是 0x1812 的规范小端 128 位布局:BASE_UUID 前 12 字节 +
// 0x12、0x18 放在 [12][13],末尾两字节补零。
#define KBMIC_HID_SERVICE_UUID16 0x1812
#define KBMIC_SVC_UUID_SLOT 16
static const uint8_t s_hid_service_uuid[KBMIC_SVC_UUID_SLOT] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,   // BASE_UUID 前 8 字节
    0x00, 0x10, 0x00, 0x00,                            // 后 4 字节
    KBMIC_HID_SERVICE_UUID16 & 0xFF,         // [12] = 0x12 → uuid16 低位
    (KBMIC_HID_SERVICE_UUID16 >> 8) & 0xFF,  // [13] = 0x18 → uuid16 高位
    0, 0,
};

// HID 报告引脚:按下保持时长。低于 30ms 少数主机会漏掉这一击,高于 60ms
// 用户会感到"粘",20ms 是通用取值。
#define KBMIC_TAP_HOLD_MS 20

static const char *TAG = "kbmic_hid";

// ---------------------------------------------------------------------------
// 报告描述符
// ---------------------------------------------------------------------------

// report map 0:标准 8 字节键盘报告(boot 布局:1B 修饰键 + 1B 保留 + 6B 键码)。
// 报告 ID 与描述符里的 0x85 项一致;esp_hid 按 id 定位要发送的特征。
#define KBMIC_REPORT_ID_KEYBOARD 1
#define KBMIC_REPORT_ID_CONSUMER 2

static const uint8_t s_map_keyboard[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1) —— 两个报告并存必须各带 ID,否则 macOS
                       //   会把两个都绑到先解析到的那个特征上(真机踩坑:
                       //   没有 ID 时 Consumer 报告被发进键盘特征,Globe/Fn 不识别)
    0x05, 0x07,        //   Usage Page (Key Codes)

    0x19, 0xE0,        //   Usage Minimum (224 = LeftControl)
    0x29, 0xE7,        //   Usage Maximum (231 = RightGUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)      -> 修饰键字节

    // 原来的保留字节私有化为 Apple Fn:AppleVendor Top Case 页(0xFF),
    // Usage 0x03(KeyboardFn)。macOS 由此识别 Fn/Globe(系统听写、微信
    // 按住说话都认它),普通 6KRO 报告长度不变(8 字节)。参考:
    // NordicBTKeyBridge / QMK AppleVendor Top Case 社区实现。
    0x05, 0xFF,        //   Usage Page (AppleVendor Top Case)
    0x09, 0x03,        //   Usage (KeyboardFn)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)      -> Apple Fn 字节

    // 这里**不能**再插 5 bit + 3 bit 的填充项。那是鼠标描述符的尾巴,
    // 键盘不需要:1 字节 modifier + 1 字节 reserved 已经把字节对齐了,
    // 加上它们会让 Input 变成 9 字节。esp_hid 的描述符解析器会直接报
    // "INPUT report does not amount to full bytes" 然后 panic —— 表现是
    // 开机无限重启,屏幕一直闪。别照抄带填充的鼠标描述符。

    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0xDD,        //   Logical Maximum (221)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0xDD,        //   Usage Maximum (221)
    0x81, 0x00,        //   Input (Data,Array)        -> 6 个键码

    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (1 = NumLock)
    0x29, 0x05,        //   Usage Maximum (5 = Kana)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x05,        //   Report Count (5)
    0x91, 0x02,        //   Output (Data,Var,Abs)     -> 主机发来的 LED 状态
    //                        ↑ 必须是 0x91(Output),不是 0x81(Input)。写成 0x81
    //                          会把这 5 bit 算进 Input 报告,Input 变成 69 bit
    //                          而非 64 bit —— 同样表现为解析失败 + panic 重启。

    0x75, 0x03,        //   Report Size (3)
    0x95, 0x01,        //   Report Count (1)
    0x91, 0x03,        //   Output (Cnst,Var,Abs)     -> 3 bit 填充

    0xC0               // End Collection
};

// report map 1:Consumer Control 报告。这里只声明 Globe 0x029D 这一个 16 位 usage。
//
// ⚠️ 16 位 usage 的编码陷阱:HID 的 Usage Min/Max 按 item 的低 2 位定长度
// (0→0 字节、1→1 字节、2→2 字节、3→4 字节)。Usage Maximum 的三种编码是
// 0x29=1 字节 / 0x2A=2 字节 / 0x2B=4 字节。写成 0x29, 0x9D, 0x02 的话,
// 解析器只吃掉 0x9D,后面的 0x02 会被当成下一个 item 的命令码,整个描述符
// 从此错位 —— 表现为 esp_hid 报 "INPUT report does not amount to full bytes"
// 然后 panic,设备开机无限重启。要 16 位值就得用 0x1A / 0x2A。
//
// 用 16 位 Array 布局而不是官方例程那种位域:Globe 是单值 usage,Array 把 usage
// 原样放进报告,主机侧不需要理解任何自定义位含义。
static const uint8_t s_map_consumer[] = {
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x15, 0x00,        //   Logical Minimum (0)        —— 0x15 = 1 字节形式
    0x26, 0x9D, 0x02,  //   Logical Maximum (0x029D)   —— 0x26 = 2 字节形式
    0x1A, 0x01, 0x00,  //   Usage Minimum (0x0001)     —— 0x1A = 2 字节形式
    0x2A, 0x9D, 0x02,  //   Usage Maximum (0x029D)     —— 0x2A = 2 字节形式
    0x75, 0x10,        //   Report Size (16)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x00,        //   Input (Data,Array)       -> 16 位 usage
    0xC0               // End Collection
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    { .data = s_map_keyboard, .len = sizeof(s_map_keyboard) },
    { .data = s_map_consumer, .len = sizeof(s_map_consumer) },
};

#define KBMIC_MAP_KEYBOARD 0
#define KBMIC_MAP_CONSUMER 1

static esp_hid_device_config_t s_hid_config = {
    .vendor_id = 0x16C0,   // pid.codes 公共 VID,避免冒用任何厂商的 ID
    .product_id = 0x27DB,
    .version = 0x0100,
    .device_name = KBMIC_BLE_DEVICE_NAME,
    .manufacturer_name = KBMIC_BLE_MANUFACTURER,
    .serial_number = KBMIC_BLE_SERIAL,
    .report_maps = s_report_maps,
    .report_maps_len = sizeof(s_report_maps) / sizeof(s_report_maps[0]),
};

static esp_hidd_dev_t *s_hid_dev;
static bool s_connected;
// 配置服务还没起来时(它依赖 esp_hidd_dev_init 先跑完)不发事件,避免空指针。
static bool s_boot_notify;

// ---------------------------------------------------------------------------
// GAP 薄层:广播与配对
// ---------------------------------------------------------------------------
static void gap_ble_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

static void gap_adv_start(void)
{
    static const esp_ble_adv_params_t adv_params = {
        .adv_int_min = 0x20,
        .adv_int_max = 0x30,
        .adv_type = ADV_TYPE_IND,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .channel_map = ADV_CHNL_ALL,
        .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    };
    esp_ble_gap_start_advertising(&adv_params);
}

static esp_err_t gap_ble_init(void)
{
    esp_err_t ret;

    // 键盘要能配对加密(主机要求),并且要有 IO 能力以完成 MITM。
    const esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    const esp_ble_io_cap_t iocap = ESP_IO_CAP_IO;
    const uint8_t enc_key_mask = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    const uint8_t key_size = 16;

    if ((ret = esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, 1)) != ESP_OK ||
        (ret = esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, 1)) != ESP_OK ||
        (ret = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &enc_key_mask, 1)) != ESP_OK ||
        (ret = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &enc_key_mask, 1)) != ESP_OK ||
        (ret = esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, 1)) != ESP_OK) {
        ESP_LOGE(TAG, "安全参数设置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    if ((ret = esp_ble_gap_register_callback(gap_ble_event)) != ESP_OK) {
        ESP_LOGE(TAG, "GAP 回调注册失败: %s", esp_err_to_name(ret));
        return ret;
    }

    if ((ret = esp_ble_gap_set_device_name(KBMIC_BLE_DEVICE_NAME)) != ESP_OK) {
        ESP_LOGE(TAG, "设备名设置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 广播包:flags + appearance + txpower + 16 位 HID 服务 UUID。
    // 名字塞不下(11 字节 UTF-8 + 其它字段会超 31),走 scan response。
    const esp_ble_adv_data_t adv_data = {
        .set_scan_rsp = false,
        .include_name = false,
        .include_txpower = true,
        .min_interval = 0x0006,  // 7.5ms
        .max_interval = 0x0010,  // 20ms
        .appearance = ESP_HID_APPEARANCE_KEYBOARD,
        .service_uuid_len = sizeof(s_hid_service_uuid),   // 16,必须是 16 的倍数
        .p_service_uuid = (uint8_t *)s_hid_service_uuid,
        .flag = 0x6,
    };
    if ((ret = esp_ble_gap_config_adv_data(&adv_data)) != ESP_OK) {
        ESP_LOGE(TAG, "广播数据配置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // scan response 只放名字。appearance 已经在广播包里了,再放一遍纯属浪费
    // 那 31 字节里最紧张的位置,留给更长的名字更划算。
    const esp_ble_adv_data_t scan_rsp = {
        .set_scan_rsp = true,
        .include_name = true,     // 设备名("AI小键盘",11 字节 UTF-8)走这里
        .include_txpower = false,
    };
    if ((ret = esp_ble_gap_config_adv_data(&scan_rsp)) != ESP_OK) {
        ESP_LOGE(TAG, "扫描响应配置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

static esp_err_t gap_stack_init(void)
{
    esp_err_t ret;

    // IDF 5.5 里 esp_bt_controller_config_t 没有 mode 字段了(5.2 之前有)。
    // 纯 BLE 由 sdkconfig 的 CONFIG_BT_CTRL_MODE_BLE_ONLY=y 决定,配置本身
    // 直接用默认宏,这也是官方 esp_hid_device 例程的写法。
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "BT controller 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    if ((ret = esp_bt_controller_enable(ESP_BT_MODE_BLE)) != ESP_OK) {
        ESP_LOGE(TAG, "BT controller 使能失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // IDF 5.5 拆成了两个入口:esp_bluedroid_init(void) 用默认配置,
    // esp_bluedroid_init_with_cfg(cfg) 才收参数。ssp_en/sc_en 都是经典蓝牙的
    // 配对开关,BLE 侧由上面的安全参数决定,这里照默认走。
    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    if ((ret = esp_bluedroid_init_with_cfg(&bd_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }
    if ((ret = esp_bluedroid_enable()) != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid 使能失败: %s", esp_err_to_name(ret));
        return ret;
    }

    return gap_ble_init();
}

static void gap_ble_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (!param->ble_security.auth_cmpl.success) {
            ESP_LOGE(TAG, "配对失败,原因 0x%x", param->ble_security.auth_cmpl.fail_reason);
        } else {
            ESP_LOGI(TAG, "配对成功");
        }
        break;
    case ESP_GAP_BLE_NC_REQ_EVT:
        // 本机无屏幕,直接确认数字配对码比对;真实产品可改为把 passkey 显示到屏上。
        ESP_LOGI(TAG, "数字配对码 %" PRIu32, param->ble_security.key_notif.passkey);
        esp_ble_confirm_reply(param->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
        // 主机在等本机输入 passkey。本机没有输入数字键盘,交给上层提示,
        // 这里只把码打出来,避免静默卡在配对流程。
        ESP_LOGW(TAG, "主机请求输入配对码: %" PRIu32, param->ble_security.key_notif.passkey);
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// HID 事件
// ---------------------------------------------------------------------------
static void hid_event_cb(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    esp_hidd_event_data_t *param = event_data;

    switch (event) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "HID 协议栈就绪,开始广播 \"%s\"", KBMIC_BLE_DEVICE_NAME);
        gap_adv_start();
        // 配置服务在这里才注册:esp_hid 内部电池/设备信息/HID 三个服务的
        // 建表链到本事件才走完,更早注册会与它并发建表,GATT 返回 133
        // (2026-10-05 真机踩坑)。注册本身投递到 svc 任务执行 —— 本回调
        // 跑在 esp_hid 的 4KB 事件任务里,深调用链会把栈压穿。失败不阻塞:
        // 键盘照常用,只是 MCP 配不了。
        kbmic_ble_svc_request_register();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        s_connected = true;
        ESP_LOGI(TAG, "主机已连接");
        // 主动报一次当前模式:订阅了事件通道的客户端(通常是 MCP)一连上就知道
        // 设备现在是什么状态,不用先发一轮读。
        if (s_boot_notify) {
            const kbmic_config_t *cfg = kbmic_config_current();
            kbmic_ble_svc_notify(KBMIC_EV_BOOT, cfg->active, 0, cfg->profiles[cfg->active].name);
        }
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        s_connected = false;
        ESP_LOGI(TAG, "主机断开,重新广播");
        gap_adv_start();
        break;
    case ESP_HIDD_PROTOCOL_MODE_EVENT:
        ESP_LOGI(TAG, "协议模式[%u]: %s", param->protocol_mode.map_index,
                 param->protocol_mode.protocol_mode ? "REPORT" : "BOOT");
        break;
    case ESP_HIDD_CONTROL_EVENT:
        break;
    case ESP_HIDD_OUTPUT_EVENT:
        // 主机回送的 LED(大小写/数字锁定)状态。本机不显示这些,忽略即可。
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
esp_err_t kbmic_hid_init(void)
{
    esp_err_t ret = gap_stack_init();
    if (ret != ESP_OK) {
        return ret;
    }

    // GATTS 全局分发回调必须装在 Bluedroid enable 之后(已在 gap_stack_init
    // 里起来)、任何 GATTS app 注册之前 —— esp_hidd_dev_init 内部马上会注册
    // 3 个 app,晚一步 REG/建表事件就无人接收,HID 永远起不来。
    ret = kbmic_ble_svc_install_dispatch();
    if (ret != ESP_OK) {
        return ret;
    }

    if ((ret = esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BLE, hid_event_cb, &s_hid_dev)) != ESP_OK) {
        ESP_LOGE(TAG, "HID 设备初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }
    s_boot_notify = true;
    return ESP_OK;
}

bool kbmic_hid_connected(void)
{
    return s_connected && esp_hidd_dev_connected(s_hid_dev);
}

esp_err_t kbmic_hid_set_battery(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (s_hid_dev == NULL) return ESP_ERR_INVALID_STATE;
    return esp_hidd_dev_battery_set(s_hid_dev, (uint8_t)percent);
}

esp_err_t kbmic_hid_key_hold(uint8_t modifier, uint8_t apple_fn, uint8_t keycode, bool pressed)
{
    if (s_hid_dev == NULL || !kbmic_hid_connected()) return ESP_ERR_INVALID_STATE;
    uint8_t report[8] = {0};
    if (pressed) {
        report[0] = modifier;
        report[1] = apple_fn ? 0x01 : 0x00;   // Apple Fn 字节(见描述符)
        report[2] = keycode;
    }
    return esp_hidd_dev_input_set(s_hid_dev, KBMIC_MAP_KEYBOARD, KBMIC_REPORT_ID_KEYBOARD, report, sizeof(report));
}

esp_err_t kbmic_hid_tap(uint8_t modifier, uint8_t keycode)
{
    esp_err_t ret = kbmic_hid_key_hold(modifier, 0, keycode, true);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(KBMIC_TAP_HOLD_MS));
    return kbmic_hid_key_hold(0, 0, 0, false);
}

esp_err_t kbmic_hid_consumer(uint16_t usage, bool pressed)
{
    if (s_hid_dev == NULL || !kbmic_hid_connected()) return ESP_ERR_INVALID_STATE;
    uint8_t report[2] = {0};
    if (pressed) {
        report[0] = (uint8_t)(usage & 0xFF);        // 16 位小端
        report[1] = (uint8_t)(usage >> 8);
    }
    return esp_hidd_dev_input_set(s_hid_dev, KBMIC_MAP_CONSUMER, KBMIC_REPORT_ID_CONSUMER, report, sizeof(report));
}
