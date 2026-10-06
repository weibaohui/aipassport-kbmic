// FoloToy AI Passport:统一快捷键模式 + 屏幕提示 + BLE HID 配置。
// Wi-Fi/HTTP 是可选项：只有 BLE/UI/按键就绪后仍有足够连续内存时才启动；
// 否则保留轻量 BLE 配置服务，桌面端 mcp_server 仍可通过 GATT 配置快捷键。
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "appfw_mcp.h"
#include "appfw_net.h"
#include "appfw_netlist.h"
#include "appfw_portal.h"
#include "appfw_storage.h"
#include "appfw_ui.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "kbmic_action.h"
#include "kbmic_ble_svc.h"
#include "kbmic_config.h"
#include "kbmic_hid.h"
#include "kbmic_mcp.h"
#include "kbmic_store.h"
#include "kbmic_ui.h"
#include "kbmic_web.h"
#include "nvs_flash.h"

#define KEY_QUEUE_LEN          16
#define KEY_TASK_STACK         4096
#define KEY_TASK_PRIORITY      5
#define WIFI_HTTP_MIN_FREE     (180 * 1024)
#define WIFI_HTTP_MIN_LARGEST  (64 * 1024)

typedef struct {
    bsp_btn_t button;
    bsp_btn_ev_t event;
} key_msg_t;

static const char *TAG = "kbmic";
static QueueHandle_t s_key_queue;
static volatile bool s_keys_ready;
static bool s_held[KBMIC_BTN_COUNT];
static bool s_in_long[KBMIC_BTN_COUNT];
static bool s_ui_ready;
static uint32_t s_dropped;

static const char *button_label(int button)
{
    switch (button) {
    case KBMIC_BTN_UP: return "上键";
    case KBMIC_BTN_DOWN: return "下键";
    default: return "OK键";
    }
}

static const kbmic_action_t *slot_of(int button, int slot)
{
    const kbmic_config_t *cfg = kbmic_config_current();
    return &cfg->profiles[cfg->active].slots[button][slot];
}

static void run_slot(int button, int slot)
{
    const kbmic_action_t *action = slot_of(button, slot);
    if (kbmic_action_is_settings(action)) return;
    const esp_err_t err = kbmic_action_run(action);
    char name[KBMIC_ACTION_NAME_MAX];
    char msg[sizeof(name) + 16];
    kbmic_action_name(action, name, sizeof(name));
    snprintf(msg, sizeof(msg), "%s：%.32s", button_label(button), name);
    kbmic_ui_set_feedback(msg, err == ESP_OK);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "%s，HID 错误: %s", msg, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "%s", msg);
    }
}

static void release_held(int button)
{
    if (!s_held[button]) return;
    s_held[button] = false;
    const esp_err_t err = kbmic_action_release();
    kbmic_ui_set_feedback(err == ESP_OK ? "语音键已松开" : "释放按键失败",
                          err == ESP_OK);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "按住动作释放失败: %s", esp_err_to_name(err));
    }
}

// appfw_ui_on_key 会规整事件：0=单击、2=双击、3=长按；4 保留给 LONG_UP。
// 这里不再接受 BSP_BTN_CLICK=1，否则框架规整后的单击会被静默丢弃。
static bool on_home_key(int button, int event)
{
    if (button < 0 || button >= KBMIC_BTN_COUNT) return true;
    const kbmic_action_t *tap = slot_of(button, KBMIC_SLOT_TAP);
    const kbmic_action_t *dbl = slot_of(button, KBMIC_SLOT_DOUBLE);
    const kbmic_action_t *lng = slot_of(button, KBMIC_SLOT_LONG);

    switch (event) {
    case 0: // appfw 单击
        if (s_held[button]) {
            release_held(button);
        } else if (!s_in_long[button] && tap->trigger == KBMIC_TRIG_CLICK) {
            run_slot(button, KBMIC_SLOT_TAP);
        }
        break;
    case 2: // appfw 双击
        if (dbl->trigger == KBMIC_TRIG_DOUBLE) run_slot(button, KBMIC_SLOT_DOUBLE);
        break;
    case 3: { // appfw 长按
        s_in_long[button] = true;
        if (s_held[button]) break; // 某些长按会重复上报，不能反复释放/按下
        const bool settings = kbmic_action_is_settings(lng);
        const bool active = lng->trigger != KBMIC_TRIG_NONE || settings;
        if (settings) {
            kbmic_ui_set_feedback("已打开设置", true);
            return false;  // 调用方要求 appfw 打开设置菜单
        }
        if (active) {
            run_slot(button, KBMIC_SLOT_LONG);
            if (lng->trigger == KBMIC_TRIG_TAP) s_held[button] = true;
        }
        break;
    }
    case BSP_BTN_LONG_UP:
        release_held(button);
        break;
    default:
        break;
    }
    return true;
}

static void press_held_slot(int button)
{
    if (button < 0 || button >= KBMIC_BTN_COUNT) return;
    s_in_long[button] = false;
    const kbmic_action_t *tap = slot_of(button, KBMIC_SLOT_TAP);
    if (tap->trigger == KBMIC_TRIG_TAP) {
        run_slot(button, KBMIC_SLOT_TAP);
        s_held[button] = true;
    }
}

static int normalized_home_event(bsp_btn_ev_t event)
{
    switch (event) {
    case BSP_BTN_CLICK: return 0;
    case BSP_BTN_DOUBLE: return 2;
    case BSP_BTN_LONG: return 3;
    case BSP_BTN_LONG_UP: return BSP_BTN_LONG_UP;
    default: return -1;
    }
}

static void refresh_voice_indicator(void)
{
    const kbmic_config_t *cfg = kbmic_config_current();
    const kbmic_profile_t *profile = &cfg->profiles[cfg->active];
    const char *voice_label = NULL;
    bool active = false;
    for (int b = 0; b < KBMIC_BTN_COUNT && voice_label == NULL; b++) {
        for (int s = 0; s < KBMIC_SLOT_COUNT && voice_label == NULL; s++) {
            const kbmic_action_t *action = &profile->slots[b][s];
            if (action->trigger != KBMIC_TRIG_TAP || kbmic_action_is_settings(action)) continue;
            for (int i = 0; i < action->step_count; i++) {
                if (action->steps[i].kind == KBMIC_STEP_APPLEFN ||
                    action->steps[i].kind == KBMIC_STEP_CONSUMER) {
                    voice_label = button_label(b);
                    active = s_held[b];
                    break;
                }
            }
        }
    }
    kbmic_ui_set_voice(active, voice_label);
}

static appfw_key_action_t home_key(int button, int event)
{
    const bool consumed = on_home_key(button, event);
    refresh_voice_indicator();
    return consumed ? APPFW_KEY_CONSUMED : APPFW_KEY_MENU;
}

static void key_callback(bsp_btn_t button, bsp_btn_ev_t event, void *user)
{
    (void)user;
    const key_msg_t msg = { .button = button, .event = event };
    if (!s_keys_ready || s_key_queue == NULL ||
        xQueueSend(s_key_queue, &msg, 0) != pdTRUE) {
        __atomic_add_fetch(&s_dropped, 1, __ATOMIC_RELAXED);
    }
}

static void simulate_key(int button, int kind)
{
    if (!s_key_queue || !s_keys_ready || button < 0 || button >= KBMIC_BTN_COUNT) return;
    const bsp_btn_ev_t seq[] = {
        BSP_BTN_PRESS,
        kind == 2 ? BSP_BTN_DOUBLE : (kind == 1 ? BSP_BTN_LONG : BSP_BTN_CLICK),
        kind == 1 ? BSP_BTN_LONG_UP : BSP_BTN_CLICK,
    };
    const int count = kind == 2 ? 1 : 3;
    for (int i = 0; i < count; i++) {
        const key_msg_t msg = { .button = (bsp_btn_t)button, .event = seq[i] };
        (void)xQueueSend(s_key_queue, &msg, 0);
    }
}

static void app_task(void *arg)
{
    (void)arg;
    key_msg_t msg;
    uint32_t ticks = 0;
    for (;;) {
        while (xQueueReceive(s_key_queue, &msg, 0) == pdTRUE) {
            // appfw 会丢弃 PRESS 和 LONG_UP，但按住类动作必须在这两个事件上启停。
            if (msg.event == BSP_BTN_PRESS) {
                press_held_slot(msg.button);
            } else if (msg.event == BSP_BTN_LONG_UP) {
                (void)on_home_key(msg.button, BSP_BTN_LONG_UP);
            } else if (s_ui_ready) {
                appfw_ui_on_key(msg.button, msg.event);
            } else {
                const int event = normalized_home_event(msg.event);
                if (event >= 0) (void)on_home_key(msg.button, event);
            }
        }
        refresh_voice_indicator();
        const uint32_t dropped = __atomic_exchange_n(&s_dropped, 0, __ATOMIC_RELAXED);
        if (dropped) ESP_LOGW(TAG, "按键事件队列溢出，丢弃=%" PRIu32, dropped);
        if (++ticks % 250 == 0) {
            ESP_LOGD(TAG, "BLE=%s heap=%u largest=%u",
                     kbmic_hid_connected() ? "connected" : "advertising",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void config_changed(void)
{
    ESP_LOGI(TAG, "快捷键配置已保存，模式=%s",
             kbmic_config_current()->profiles[kbmic_config_current()->active].name);
    kbmic_ui_set_feedback("快捷键配置已保存", true);
}

static const appfw_menu_nav_t s_menu_navs[] = {{
    .label = "键盘快捷键",
    .enter = kbmic_nav_enter,
    .build = kbmic_nav_build,
    .poll = kbmic_nav_poll,
    .key = kbmic_nav_key,
}};

static bool network_start_if_headroom(void)
{
    const size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "BLE/UI 就绪后 heap: free=%u largest=%u",
             (unsigned)free_heap, (unsigned)largest);
    if (free_heap < WIFI_HTTP_MIN_FREE || largest < WIFI_HTTP_MIN_LARGEST) {
        ESP_LOGW(TAG, "内存保留给 BLE/显示/按键；跳过 Wi-Fi/HTTP，使用 BLE MCP 配置服务");
        return false;
    }

    static const appfw_prov_cfg_t portal_cfg = {
        .app_config_html = kbmic_web_html,
        .on_httpd_ready = kbmic_web_register,
    };
    appfw_prov_configure(&portal_cfg);
    appfw_netlist_t list;
    const bool has_saved_wifi = appfw_store_netlist_load(&list);
    if (!has_saved_wifi) {
        appfw_netlist_reset(&list);
        ESP_LOGI(TAG, "无已保存 Wi-Fi；跳过 Wi-Fi/SoftAP/HTTP，使用 BLE MCP 配置服务");
        return false;
    }
    const int err = appfw_net_init(&list, true);
    if (err != 0) {
        ESP_LOGW(TAG, "Wi-Fi/HTTP 未启动(%d)，BLE MCP 配置保持可用", err);
        return false;
    }
    kbmic_mcp_set_simulate(simulate_key);
    kbmic_mcp_init();
    ESP_LOGI(TAG, "Wi-Fi/HTTP 与设备侧 MCP 已启动");
    return true;
}

void app_main(void)
{
    ESP_LOGI(TAG, "AI 键盘统一快捷键模式启动");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    bsp_i2c_init();
    (void)bsp_battery_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示初始化失败；为保留键盘功能继续启动");
    } else {
        bsp_display_backlight(100);
    }

    kbmic_config_load();
    kbmic_config_set_hook(config_changed);
    const kbmic_config_t *cfg = kbmic_config_current();
    ESP_LOGI(TAG, "当前 %s 档：下短按 Enter/双击取消，OK短按退格/长按设置，上长按 Fn",
             cfg->profiles[cfg->active].name);

    s_key_queue = xQueueCreate(KEY_QUEUE_LEN, sizeof(key_msg_t));
    if (!s_key_queue ||
        xTaskCreate(app_task, "kbmic_keys", KEY_TASK_STACK, NULL,
                    KEY_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "按键队列/任务创建失败");
        return;
    }

    if (bsp_button_init(key_callback, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "ADC 三键初始化失败");
        return;
    }
    if (kbmic_hid_init() != ESP_OK) {
        ESP_LOGE(TAG, "BLE HID 初始化失败");
        return;
    }

    const appfw_ui_cfg_t ui_cfg = {
        .home_title = "AI小键盘",
        .home_build = kbmic_home_build,
        .home_poll = kbmic_home_poll,
        .home_key = home_key,
        .menu_show_mask = APPFW_MENU_ITEM_SCREEN_OFF | APPFW_MENU_ITEM_BRIGHTNESS,
        .menu_navs = s_menu_navs,
        .menu_navs_count = 1,
        .menu_open_btn = 0xFF,
        .long_press_ok = APPFW_LONG_PRESS_OPEN_MENU,
    };
    if (bsp_lvgl_lock(1000)) {
        appfw_ui_init(&ui_cfg);
        bsp_lvgl_unlock();
        s_ui_ready = true;
    }

    s_keys_ready = true;
    (void)network_start_if_headroom();
    ESP_LOGI(TAG, "启动完成：BLE 快捷键配置服务始终可用；按键图例显示于屏幕");
}
