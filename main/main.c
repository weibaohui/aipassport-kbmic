// main/main.c —— AI 小键盘 启动装配与按键状态机。
//
// 职责边界:
//   bsp         显示/LVGL、按键、电池(框架提供,本应用不改)
//   kbmic_config 键盘模式配置:内置 4 个 + 用户自定义,存 NVS
//   kbmic_hid   BLE HID 键盘 + Consumer 报告
//   kbmic_ble_svc 配置服务:MCP 通过它读写上面那份配置
//   kbmic_action 动作执行器:把配置里的一段 step 变成 HID 报告
//   kbmic_ui    渲染(主页/键盘设置子页,挂在 appfw_ui 框架上)
//
// 并发模型:只有一条 app_task 改状态(按键事件、连接状态、电量),UI 重画也在
// 同一条任务里持锁完成。状态与画面之间不存在第二份真相。LVGL 自己的渲染任务
// 由 bsp 内部管,不归这里。BSP 的按键回调跑在 esp_timer 任务里,只入队。
#include <stdarg.h>
#include <stdio.h>

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "appfw_mcp.h"
#include "appfw_ui.h"
#include "appfw_net.h"
#include "appfw_netlist.h"
#include "appfw_netlog.h"
#include "appfw_portal.h"
#include "appfw_storage.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "kbmic_action.h"
#include "kbmic_ble_svc.h"
#include "kbmic_mcp.h"
#include "kbmic_store.h"
#include "kbmic_hid.h"
#include "kbmic_ui.h"
#include "kbmic_web.h"
#include "nvs_flash.h"

static const char *TAG = "kbmic";

static QueueHandle_t s_key_queue;
static volatile bool s_keys_ready;

// ---------------------------------------------------------------------------
// 界面状态
// ---------------------------------------------------------------------------
static const char *btn_name(int b)
{
    switch (b) {
    case KBMIC_BTN_UP:   return "上";
    case KBMIC_BTN_DOWN: return "下";
    default:             return "OK";
    }
}

// 界面提示用的完整键名("上键"/"下键"/"OK")。
static const char *btn_key_name(int b)
{
    switch (b) {
    case KBMIC_BTN_UP:   return "上键";
    case KBMIC_BTN_DOWN: return "下键";
    default:             return "OK";
    }
}

static const char *slot_name(int s)
{
    switch (s) {
    case KBMIC_SLOT_DOUBLE: return "双击";
    case KBMIC_SLOT_LONG:   return "长按";
    default:                return "短按";
    }
}

static bool s_held[KBMIC_BTN_COUNT];         // 各键是否正按住(TAP 类动作)
static bool s_in_long[KBMIC_BTN_COUNT];      // 已经走过长按阈值
static int s_battery = -1;
static bool s_connected;

void kbmic_voice_refresh(void);   // app_task 兜底刷新用(定义在 home_key 后)

// 当前生效模式的某个槽。BLE 配置在外部被改过之后,这里每次都重新取,
// 不缓存 —— 缓存就会和 MCP 的写入打架。
static const kbmic_action_t *slot_of(int btn, int slot)
{
    const kbmic_config_t *cfg = kbmic_config_current();
    return &cfg->profiles[cfg->active].slots[btn][slot];
}

// ---------------------------------------------------------------------------
// 按键执行
// ---------------------------------------------------------------------------
static void run_slot(int btn, int slot)
{
    const kbmic_action_t *a = slot_of(btn, slot);
    // "进设置"只在主页长按路径生效(home_key 返回 APPFW_KEY_MENU);
    // 其余路径遇到该动作按无操作处理。
    if (kbmic_action_is_settings(a)) return;
    const esp_err_t ret = kbmic_action_run(a);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "执行动作失败: %s", esp_err_to_name(ret));
    }
}

// 主页:把 BSP 的 PRESS / CLICK / LONG 翻译成配置里的 TAP / 长按两种槽。
//
// BSP 的时序是 PRESS -> (LONG, 超过 500ms) -> CLICK。配置把一个键拆成"短按"和
// "长按"两个槽,但触发语义有三种(点一下 / 按住 / 长按触发),映射关系是:
//
//   PRESS + 短按槽是 TAP 触发  -> 立刻按住,等松手
//   CLICK + 短按槽是 CLICK 触发 -> 打一次
//   LONG  + 长按槽            -> 跑长按槽;若它是 TAP 触发则按住到松手
static void on_key_home(int btn, bsp_btn_ev_t ev)
{
    const kbmic_action_t *tap_slot = slot_of(btn, KBMIC_SLOT_TAP);
    const kbmic_action_t *long_slot = slot_of(btn, KBMIC_SLOT_LONG);

    switch (ev) {
    case BSP_BTN_PRESS:
        s_in_long[btn] = false;
        if (tap_slot->trigger == KBMIC_TRIG_TAP) {
            run_slot(btn, KBMIC_SLOT_TAP);
            s_held[btn] = true;
        }
        break;

    case BSP_BTN_CLICK:
        if (s_held[btn]) {
            s_held[btn] = false;
            kbmic_action_release();
        } else if (!s_in_long[btn] && tap_slot->trigger == KBMIC_TRIG_CLICK) {
            run_slot(btn, KBMIC_SLOT_TAP);
        }
        break;

    case BSP_BTN_LONG_UP:
        // 长按后的松开:按住类动作(TAP,如说话)的正式收尾点。
        // 此前只能等松手的 CLICK 兜底,而菜单里那次 CLICK 会被吞掉,
        // 造成"松开上键界面一直显示说话中"。
        if (s_held[btn]) {
            s_held[btn] = false;
            kbmic_action_release();
        }
        break;

    case BSP_BTN_DOUBLE:
        // 双击:跑"双击槽"里配了 DOUBLE 触发的动作(与短按互斥由 BSP 保证)。
        if (slot_of(btn, KBMIC_SLOT_DOUBLE)->trigger == KBMIC_TRIG_DOUBLE) {
            run_slot(btn, KBMIC_SLOT_DOUBLE);
        }
        break;

    case BSP_BTN_LONG: {
        s_in_long[btn] = true;
        // 长按槽有效 = 配了触发,或它是"进设置"这类纯软件动作(NONE 触发,
        // 见 kbmic_action.c;拿 NONE 当"槽没配"过滤会让长按 OK 永远进不了设置)。
        const bool long_active = (long_slot->trigger != KBMIC_TRIG_NONE) ||
                                 kbmic_action_is_settings(long_slot);
        // 长按槽真要做事,才打断按住中的 TAP("长按取消本次");槽为空时
        // 绝不能松 —— 说话就是按住 OK,在这里松了话就断了。
        if (long_active && s_held[btn]) {
            s_held[btn] = false;
            kbmic_action_release();
        }
        if (long_active) {
            if (long_slot->trigger == KBMIC_TRIG_TAP) {
                run_slot(btn, KBMIC_SLOT_LONG);
                s_held[btn] = true;
            } else {
                run_slot(btn, KBMIC_SLOT_LONG);
            }
        }
        break;
    }

    default:
        break;
    }
}

static void activate(void);

// 模式列表:上下移动,OK 选中并切过去,长按返回。列表末尾还有一行动态的
// "＋ 新建模式"(模式未满时),OK 即创建一个自定义模式并选中它 —— 不用
// AI/网页也能扩模式。改名走网页/MCP(机身没有输入法)。
static int modes_rows(void)
{
    const int n = kbmic_config_current()->count;
    return (n < KBMIC_MAX_PROFILES) ? n + 1 : n;
}

// 按键配置:6 行(3 键 × 2 槽),OK 进入该槽的动作选择,长按返回。
// 动作选择:上下选,OK 写入,长按返回。
// ---------------------------------------------------------------------------
// 界面数据
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// BSP 按键回调
//
// 跑在 button 组件的 esp_timer 任务里,绝不能在里面改状态、发 HID 报告或碰
// LVGL —— 只做一次入队。打包成 int:低 4 位按键(0..2),高 4 位事件(0..3)。
// ---------------------------------------------------------------------------
static void on_key_from_bsp(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (s_key_queue == NULL || !s_keys_ready) {
        return;
    }
    // 队列满说明应用任务被拖慢了。丢按键而不是阻塞 esp_timer 任务:
    // 阻塞整个 timer 任务会连带拖慢 LVGL 的心跳,代价远大于丢一次敲击。
    const int msg = ((int)btn & 0xF) | (((int)ev & 0xF) << 4);
    xQueueSend(s_key_queue, &msg, 0);
}

// ---------------------------------------------------------------------------
// 周期任务
// ---------------------------------------------------------------------------
static void on_config_changed(void)
{
    ESP_LOGI(TAG, "配置已更新(模式数 %u)", kbmic_config_current()->count);
}

static void app_task(void *arg)
{
    (void)arg;
    int msg;
    uint32_t loop = 0;

    for (;;) {
        while (s_key_queue && xQueueReceive(s_key_queue, &msg, 0) == pdTRUE) {
            appfw_ui_on_key(msg & 0xF, (msg >> 4) & 0xF);   // 框架统一入口
        }

        s_connected = kbmic_hid_connected();
        kbmic_voice_refresh();   // 10Hz 兜底:事件丢失也不卡"说话中"

        if (++loop % 50 == 0) {
            const int soc = bsp_battery_soc();
            if (soc != s_battery) {
                s_battery = soc;
                if (soc >= 0) {
                    kbmic_hid_set_battery(soc);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));   // 10 fps
    }
}

// ---------------------------------------------------------------------------
// 主页按键(appfw home_key 回调,锁外执行):按当前模式执行槽位动作。
// 长按 OK 若配了"进设置"返回 APPFW_KEY_MENU(打开框架设置菜单 —— WiFi
// 管理/设备信息/配网/AI 管理与"键盘设置"入口都在那里)。
// ---------------------------------------------------------------------------
static appfw_key_action_t kbmic_home_key(int btn, int ev)
{
    ESP_LOGI(TAG, "按键 %s 事件%d", btn_name(btn), ev);
    if (btn < 0 || btn >= KBMIC_BTN_COUNT) return APPFW_KEY_CONSUMED;

    const kbmic_action_t *tap_slot = slot_of(btn, KBMIC_SLOT_TAP);
    const kbmic_action_t *dbl_slot = slot_of(btn, KBMIC_SLOT_DOUBLE);
    const kbmic_action_t *long_slot = slot_of(btn, KBMIC_SLOT_LONG);

    switch ((bsp_btn_ev_t)ev) {
    case BSP_BTN_PRESS:
        s_in_long[btn] = false;
        if (tap_slot->trigger == KBMIC_TRIG_TAP) {
            run_slot(btn, KBMIC_SLOT_TAP);
            s_held[btn] = true;
        }
        break;

    case BSP_BTN_CLICK:
        if (s_held[btn]) {
            s_held[btn] = false;
            kbmic_action_release();
        } else if (!s_in_long[btn] && tap_slot->trigger == KBMIC_TRIG_CLICK) {
            run_slot(btn, KBMIC_SLOT_TAP);
        }
        break;

    case BSP_BTN_DOUBLE:
        if (dbl_slot->trigger == KBMIC_TRIG_DOUBLE) {
            run_slot(btn, KBMIC_SLOT_DOUBLE);
        }
        break;

    case BSP_BTN_LONG: {
        s_in_long[btn] = true;
        const bool long_active = (long_slot->trigger != KBMIC_TRIG_NONE) ||
                                 kbmic_action_is_settings(long_slot);
        // 长按槽真有动作才打断按住中的 TAP(说话);槽空则说话持续。
        if (long_active && s_held[btn]) {
            s_held[btn] = false;
            kbmic_action_release();
        }
        if (long_active) {
            if (kbmic_action_is_settings(long_slot)) return APPFW_KEY_MENU;
            if (long_slot->trigger == KBMIC_TRIG_TAP) {
                run_slot(btn, KBMIC_SLOT_LONG);
                s_held[btn] = true;
            } else {
                run_slot(btn, KBMIC_SLOT_LONG);
            }
        }
        break;
    }

    case BSP_BTN_LONG_UP:
        // 长按后的松开:按住类动作(说话)的正式收尾
        if (s_held[btn]) {
            s_held[btn] = false;
            kbmic_action_release();
        }
        break;

    default:
        break;
    }

    kbmic_voice_refresh();
    return APPFW_KEY_CONSUMED;
}

// 语音状态统一计算:找"TAP 触发且带 Consumer/Globe 或 AppleFn 步"的槽,
// 返回其按键名;是否按住看 s_held。home_key 即时调用,app_task 周期兜底 ——
// 即使某个按键事件丢失(表现为界面卡"说话中"),100ms 内自愈。
void kbmic_voice_refresh(void)
{
    const kbmic_config_t *cfg = kbmic_config_current();
    const char *voice_btn = NULL;
    bool active = false;
    for (int b = 0; b < KBMIC_BTN_COUNT && !voice_btn; b++) {
        for (int s = 0; s < KBMIC_SLOT_COUNT && !voice_btn; s++) {
            const kbmic_action_t *a = &cfg->profiles[cfg->active].slots[b][s];
            if (a->trigger != KBMIC_TRIG_TAP || kbmic_action_is_settings(a)) continue;
            bool hit = false;
            for (int k = 0; k < a->step_count; k++) {
                if (a->steps[k].kind == KBMIC_STEP_CONSUMER ||
                    a->steps[k].kind == KBMIC_STEP_APPLEFN) hit = true;
            }
            if (hit) {
                voice_btn = btn_key_name(b);
                active = s_held[b];
            }
        }
    }
    kbmic_ui_set_voice(active, voice_btn);
}

// ---------------------------------------------------------------------------
// 键盘设置导航子页(框架菜单入口)
// ---------------------------------------------------------------------------
static const appfw_menu_nav_t k_navs[] = {{
    .label = "键盘设置",
    .enter = kbmic_nav_enter,
    .build = kbmic_nav_build,
    .poll  = kbmic_nav_poll,
    .key   = kbmic_nav_key,
}};

// ---------------------------------------------------------------------------
// MCP 模拟触发回调
//
// 把合成按键事件投进 s_key_queue,与真人按键走完全同一条路径(状态机、HID、
// 界面反馈一致)。kind 0=短按:PRESS→CLICK;1=长按:PRESS→LONG→CLICK;
// 2=双击:单发 DOUBLE。
// ---------------------------------------------------------------------------
static void simulate_inject(int btn, int kind)
{
    if (s_key_queue == NULL || !s_keys_ready) {
        return;
    }
    if (kind == 2) {   // 双击:单发 DOUBLE 事件(手势判定在 BSP 侧)
        const int dbl = (btn & 0xF) | ((int)BSP_BTN_DOUBLE << 4);
        xQueueSend(s_key_queue, &dbl, 0);
        return;
    }
    const int press = (btn & 0xF) | ((int)BSP_BTN_PRESS << 4);
    const int act = (btn & 0xF) |
                    (((int)(kind == 1 ? BSP_BTN_LONG : BSP_BTN_CLICK)) << 4);
    const int rel = (btn & 0xF) | ((int)BSP_BTN_CLICK << 4);
    xQueueSend(s_key_queue, &press, 0);
    xQueueSend(s_key_queue, &act, 0);
    if (kind == 1) {
        xQueueSend(s_key_queue, &rel, 0);
    }
}

// ---------------------------------------------------------------------------
// 启动
// ---------------------------------------------------------------------------
void app_main(void)
{
    ESP_LOGI(TAG, "AI 小键盘启动");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    bsp_i2c_init();
    (void)bsp_battery_init();   // 失败不阻塞:电量显示降级为 --

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,应用无法继续");
        return;
    }
    bsp_display_backlight(100);

    kbmic_config_load();
    kbmic_config_set_hook(on_config_changed);
    const kbmic_config_t *cfg = kbmic_config_current();
    ESP_LOGI(TAG, "当前模式 %s,共 %u 个", cfg->profiles[cfg->active].name, cfg->count);

    // GATTS 全局分发回调的注册在 kbmic_hid_init() 里完成(必须在 Bluedroid
    // enable 之后、esp_hidd_dev_init 的 app 注册之前,见 kbmic_hid.c)。

    // BLE 失败不 return:屏幕仍要起来,用户才知道出了什么事,也能在有屏的情况下
    // 用界面改配置,而不是对着黑屏。
    // 配置服务(kbmic_ble_svc_init)在 ESP_HIDD_START_EVENT 里才注册:
    // 必须等 esp_hid 内部的建表链走完,否则并发建表 GATT 返回 133(见 kbmic_hid.c)。
    if (kbmic_hid_init() != ESP_OK) {
        ESP_LOGE(TAG, "BLE HID 初始化失败,蓝牙键盘不可用");
    }

    // 网络:WiFi 引擎 + 配网门户 + 设备 MCP(8080)。门户配置必须在
    // appfw_net_init 之前注入 —— 空表时 init 内部会立即自动开配网,
    // 晚了门户就用默认形状起来了(只有首次配网才自动开,之后走菜单项)。
    static const appfw_prov_cfg_t pcfg = {
        .app_config_html = kbmic_web_html,
        .on_httpd_ready  = kbmic_web_register,
    };
    appfw_prov_configure(&pcfg);
    appfw_netlist_t list;
    if (!appfw_store_netlist_load(&list)) appfw_netlist_reset(&list);
    const int net_err = appfw_net_init(&list, true);
    if (net_err != 0) {
        ESP_LOGW(TAG, "WiFi 初始化返回 %d", net_err);
    }
    appfw_netlog_init();          // 网络日志环形缓冲 + get_recent_logs 等诊断工具
    kbmic_mcp_set_simulate(simulate_inject);
    kbmic_mcp_init();

    // 界面:框架 appfw_ui(主页+设置菜单+键盘设置子页)。
    // 长按 OK 配的"进设置"在 home_key 里返回 APPFW_KEY_MENU 打开框架菜单。
    const appfw_ui_cfg_t ucfg = {
        .home_title = "AI小键盘",
        .home_build = kbmic_home_build,
        .home_poll  = kbmic_home_poll,
        .home_key   = kbmic_home_key,
        // 框架设置菜单:WiFi 管理/设备信息/配网/AI 管理/亮度(刷新周期无意义)
        .menu_show_mask = APPFW_MENU_ITEM_ALL & ~APPFW_MENU_ITEM_REFRESH_PERIOD,
        .menu_navs = k_navs,
        .menu_navs_count = 1,
        .menu_open_btn = 0xFF,   // 主页按键全被 home_key 接管
    };
    if (bsp_lvgl_lock(1000)) {
        appfw_ui_init(&ucfg);
        bsp_lvgl_unlock();
    }

    s_key_queue = xQueueCreate(16, sizeof(int));
    if (!s_key_queue ||
        xTaskCreate(app_task, "app", 4096, NULL, 5, NULL) != pdPASS ||
        bsp_button_init(on_key_from_bsp, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键或任务初始化失败");
        return;
    }
    s_keys_ready = true;

    ESP_LOGI(TAG, "启动完成");
}
