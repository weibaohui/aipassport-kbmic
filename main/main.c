// main/main.c —— 当前调试固件：仅运行 Mac BLE 键盘四项基础功能。
//
// 固定映射：短按下键=Enter；短按 OK=Backspace；长按上键=Apple Fn 按下；
// 上键长按后松开=Apple Fn 释放。此精简固件不启动显示、Wi-Fi、配网、MCP、
// 配置存储或电量服务任务，避免无关功能挤占 ESP32-C3 的 BLE/按键运行内存。
#include <inttypes.h>
#include <stdint.h>

#include "bsp_button.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "kbmic_hid.h"
#include "nvs_flash.h"

#define KEY_EVENT_QUEUE_LENGTH 8
#define KEY_TASK_STACK_SIZE    3072
#define KEY_TASK_PRIORITY      5

#define HID_KEY_ENTER          0x28
#define HID_KEY_BACKSPACE      0x2A

typedef struct {
    bsp_btn_t button;
    bsp_btn_ev_t event;
} key_event_t;

static const char *TAG = "kbmic_min";
static QueueHandle_t s_key_queue;
static bool s_fn_held;
static uint32_t s_dropped_key_events;

// BSP 回调运行在共享 esp_timer 任务：只投递短消息，不执行蓝牙报告或延时。
static void on_button_event(bsp_btn_t button, bsp_btn_ev_t event, void *user)
{
    (void)user;
    const key_event_t key_event = { .button = button, .event = event };
    if (s_key_queue == NULL || xQueueSend(s_key_queue, &key_event, 0) != pdTRUE) {
        __atomic_add_fetch(&s_dropped_key_events, 1, __ATOMIC_RELAXED);
    }
}

static void report_result(const char *action, esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s HID 报告失败: %s (connected=%d)", action,
                 esp_err_to_name(err), kbmic_hid_connected());
    }
}

static void key_task(void *arg)
{
    (void)arg;
    key_event_t event;

    for (;;) {
        if (xQueueReceive(s_key_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (event.button == BSP_BTN_DOWN && event.event == BSP_BTN_CLICK) {
            ESP_LOGI(TAG, "下键短按 -> Enter");
            report_result("Enter", kbmic_hid_tap(0, HID_KEY_ENTER));
        } else if (event.button == BSP_BTN_OK && event.event == BSP_BTN_CLICK) {
            ESP_LOGI(TAG, "OK 短按 -> Backspace");
            report_result("Backspace", kbmic_hid_tap(0, HID_KEY_BACKSPACE));
        } else if (event.button == BSP_BTN_UP && event.event == BSP_BTN_LONG) {
            ESP_LOGI(TAG, "上键长按 -> Apple Fn 按下");
            const esp_err_t err = kbmic_hid_applefn(true);
            s_fn_held = (err == ESP_OK);
            report_result("Apple Fn 按下", err);
        } else if (event.button == BSP_BTN_UP && event.event == BSP_BTN_LONG_UP) {
            if (s_fn_held) {
                ESP_LOGI(TAG, "上键松开 -> Apple Fn 释放");
                s_fn_held = false;
                report_result("Apple Fn 释放", kbmic_hid_applefn(false));
            }
        }

        const uint32_t dropped = __atomic_exchange_n(&s_dropped_key_events, 0, __ATOMIC_RELAXED);
        if (dropped != 0) {
            ESP_LOGW(TAG, "按键事件队列曾满，累计丢弃 %" PRIu32 " 个", dropped);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "启动 Mac 键盘最小模式");

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return;
    }

    s_key_queue = xQueueCreate(KEY_EVENT_QUEUE_LENGTH, sizeof(key_event_t));
    if (s_key_queue == NULL) {
        ESP_LOGE(TAG, "按键队列创建失败");
        return;
    }

    if (xTaskCreate(key_task, "kbmic_keys", KEY_TASK_STACK_SIZE, NULL,
                    KEY_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "按键任务创建失败");
        return;
    }

    err = bsp_button_init(on_button_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC 三键初始化失败: %s", esp_err_to_name(err));
        return;
    }

    err = kbmic_hid_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE HID 初始化失败: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "最小模式就绪：下=Enter，OK=Backspace，上长按/松开=Apple Fn");
}
