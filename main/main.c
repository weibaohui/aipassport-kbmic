// main/main.c —— AI 小键盘 启动装配与按键状态机。
//
// 职责边界:
//   bsp         显示/LVGL、按键、电池(框架提供,本应用不改)
//   kbmic_config 键盘模式配置:内置 4 个 + 用户自定义,存 NVS
//   kbmic_hid   BLE HID 键盘 + Consumer 报告
//   kbmic_ble_svc 配置服务:MCP 通过它读写上面那份配置
//   kbmic_action 动作执行器:把配置里的一段 step 变成 HID 报告
//   kbmic_ui    渲染;只认 kbmic_ui_state_t,不知道业务
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
typedef enum {
    ST_HOME = 0,   // 主页:3 行,各显示一个键当前发什么
    ST_MENU,       // 设置菜单
    ST_MODES,      // 键盘模式选择
    ST_KEYS,       // 按键配置:3 键 × 2 槽
    ST_ACT,        // 某个槽的动作选择
} view_t;

// 设置菜单的条目数,要与 build_view() 里 ST_MENU 分支的顺序一致。
#define MENU_ITEMS 4

static view_t s_view = ST_HOME;
static int s_cursor[ST_ACT];                 // 每个页面各自记住光标
static uint8_t s_act_btn, s_act_slot;        // 动作选择页正在改哪一个槽
static bool s_held[KBMIC_BTN_COUNT];         // 各键是否正按住(TAP 类动作)
static bool s_in_long[KBMIC_BTN_COUNT];      // 已经走过长按阈值
static int s_battery = -1;
static bool s_connected;

static const char *btn_name(int b)
{
    switch (b) {
    case KBMIC_BTN_UP:   return "上";
    case KBMIC_BTN_DOWN: return "下";
    default:             return "OK";
    }
}

static const char *slot_name(int s)
{
    return (s == KBMIC_SLOT_LONG) ? "长按" : "短按";
}

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
static void enter_settings(void)
{
    s_view = ST_MENU;
    s_cursor[ST_MENU] = 0;
}

static void run_slot(int btn, int slot)
{
    const kbmic_action_t *a = slot_of(btn, slot);
    if (kbmic_action_is_settings(a)) {
        enter_settings();
        return;
    }
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

    case BSP_BTN_LONG:
        s_in_long[btn] = true;
        if (s_held[btn]) {
            // 短按按住的东西先放掉,再跑长按动作。
            // 语音键就是靠这一步实现"长按取消本次"。
            s_held[btn] = false;
            kbmic_action_release();
        }
        if (long_slot->trigger != KBMIC_TRIG_NONE) {
            if (long_slot->trigger == KBMIC_TRIG_TAP) {
                run_slot(btn, KBMIC_SLOT_LONG);
                s_held[btn] = true;
            } else {
                run_slot(btn, KBMIC_SLOT_LONG);
            }
        }
        break;

    default:
        break;
    }
}

static void go_back(void)
{
    switch (s_view) {
    case ST_ACT:  s_view = ST_KEYS;  break;
    case ST_KEYS: s_view = ST_MENU;  break;
    case ST_MODES:s_view = ST_MENU;  break;
    case ST_MENU: s_view = ST_HOME;  break;
    default:      s_view = ST_HOME;  break;
    }
}

static void activate(void);

static void on_key_menu(int ev_btn, bsp_btn_ev_t ev)
{
    if (ev == BSP_BTN_LONG && ev_btn == KBMIC_BTN_OK) {
        go_back();
        return;
    }
    if (ev != BSP_BTN_CLICK) {
        return;   // 菜单里不响应双击;长按非 OK 键也只当没按
    }
    if (ev_btn == KBMIC_BTN_UP) {
        s_cursor[ST_MENU] = (s_cursor[ST_MENU] + MENU_ITEMS - 1) % MENU_ITEMS;
    } else if (ev_btn == KBMIC_BTN_DOWN) {
        s_cursor[ST_MENU] = (s_cursor[ST_MENU] + 1) % MENU_ITEMS;
    } else if (ev_btn == KBMIC_BTN_OK) {
        activate();
    }
}

// 模式列表:上下移动,OK 选中并切过去,长按返回。
static void on_key_modes(int btn, bsp_btn_ev_t ev)
{
    kbmic_config_t work = *kbmic_config_current();
    const int n = work.count;
    if (n <= 0) {
        return;
    }

    if (ev == BSP_BTN_LONG && btn == KBMIC_BTN_OK) {
        go_back();
        return;
    }
    if (ev != BSP_BTN_CLICK) {
        return;
    }

    if (btn == KBMIC_BTN_UP) {
        s_cursor[ST_MODES] = (s_cursor[ST_MODES] + n - 1) % n;
    } else if (btn == KBMIC_BTN_DOWN) {
        s_cursor[ST_MODES] = (s_cursor[ST_MODES] + 1) % n;
    } else if (btn == KBMIC_BTN_OK) {
        work.active = (uint8_t)s_cursor[ST_MODES];
        kbmic_config_commit(&work);
        s_view = ST_HOME;
        ESP_LOGI(TAG, "切换到模式 %s", work.profiles[work.active].name);
    }
}

// 按键配置:6 行(3 键 × 2 槽),OK 进入该槽的动作选择,长按返回。
static void on_key_keys(int btn, bsp_btn_ev_t ev)
{
    const int rows = KBMIC_BTN_COUNT * KBMIC_SLOT_COUNT;
    if (ev == BSP_BTN_LONG && btn == KBMIC_BTN_OK) {
        go_back();
        return;
    }
    if (ev != BSP_BTN_CLICK) {
        return;
    }
    if (btn == KBMIC_BTN_UP) {
        s_cursor[ST_KEYS] = (s_cursor[ST_KEYS] + rows - 1) % rows;
    } else if (btn == KBMIC_BTN_DOWN) {
        s_cursor[ST_KEYS] = (s_cursor[ST_KEYS] + 1) % rows;
    } else if (btn == KBMIC_BTN_OK) {
        s_act_btn = (uint8_t)(s_cursor[ST_KEYS] / KBMIC_SLOT_COUNT);
        s_act_slot = (uint8_t)(s_cursor[ST_KEYS] % KBMIC_SLOT_COUNT);
        s_cursor[ST_ACT] = 0;
        s_view = ST_ACT;
    }
}

// 动作选择:上下选,OK 写入,长按返回。
static void on_key_act(int btn, bsp_btn_ev_t ev)
{
    const int n = kbmic_catalog_count();
    if (ev == BSP_BTN_LONG && btn == KBMIC_BTN_OK) {
        go_back();
        return;
    }
    if (ev != BSP_BTN_CLICK) {
        return;
    }
    if (btn == KBMIC_BTN_UP) {
        s_cursor[ST_ACT] = (s_cursor[ST_ACT] + n - 1) % n;
    } else if (btn == KBMIC_BTN_DOWN) {
        s_cursor[ST_ACT] = (s_cursor[ST_ACT] + 1) % n;
    } else if (btn == KBMIC_BTN_OK) {
        kbmic_config_t work = *kbmic_config_current();
        kbmic_action_t a = kbmic_catalog_get((uint8_t)s_cursor[ST_ACT])->action;
        // 动作选择页按的是"某一个槽"。目录里的动作自带触发语义(Enter 是点一下、
        // Globe 是按住),但长按槽必须一直是长按触发,否则 500ms 之后没人收尾,
        // 按住不放会一直重复发。
        if (s_act_slot == KBMIC_SLOT_LONG) {
            a.trigger = KBMIC_TRIG_LONG;
        }
        work.profiles[work.active].slots[s_act_btn][s_act_slot] = a;
        kbmic_config_commit(&work);
        s_view = ST_KEYS;
    }
}

static void activate(void)
{
    switch (s_cursor[ST_MENU]) {
    case 0:
        s_view = ST_MODES;
        s_cursor[ST_MODES] = kbmic_config_current()->active;
        break;
    case 1:
        s_view = ST_KEYS;
        s_cursor[ST_KEYS] = 0;
        break;
    case 2: {
        // 恢复默认:只对内置模式有意义,自定义模式没有"出厂值"可回。
        kbmic_config_t work = *kbmic_config_current();
        if (work.active < KBMIC_BUILTIN_MODES) {
            kbmic_config_reset_profile(&work, work.active);
            kbmic_config_commit(&work);
            ESP_LOGI(TAG, "模式 %s 已恢复默认", work.profiles[work.active].name);
        } else {
            ESP_LOGW(TAG, "自定义模式没有出厂默认,未改动");
        }
        break;
    }
    case 3:
        // 开启配网:WiFi 切热点模式(断 STA),手机连上访问 192.168.4.1。
        // 门户里点"保存并连接"后由框架自动关热点回 STA(与收音机同款模型)。
        ESP_LOGI(TAG, "开启配网门户(菜单项)");
        appfw_net_start_portal();
        break;
    default:
        break;
    }
}

static void handle_key(int btn, bsp_btn_ev_t ev)
{
    // 按键诊断:键盘应用事件频率是人手速,INFO 级不构成刷屏。
    // 真机排障(如"长按无反应")时先看这条有没有出,再谈状态机。
    ESP_LOGI(TAG, "按键 %s 事件%d @视图%d", btn_name(btn), (int)ev, (int)s_view);
    switch (s_view) {
    case ST_HOME: on_key_home(btn, ev); break;
    case ST_MENU: on_key_menu(btn, ev); break;
    case ST_MODES: on_key_modes(btn, ev); break;
    case ST_KEYS: on_key_keys(btn, ev); break;
    case ST_ACT: on_key_act(btn, ev); break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// 界面数据
// ---------------------------------------------------------------------------
static void set_title(kbmic_ui_state_t *st, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st->title, sizeof(st->title), fmt, ap);
    va_end(ap);
}

static void set_footer(kbmic_ui_state_t *st, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st->footer, sizeof(st->footer), fmt, ap);
    va_end(ap);
}

static void set_line(kbmic_ui_state_t *st, int i, const char *label, const char *value)
{
    if (i < 0 || i >= KBMIC_UI_LINES_MAX) {
        return;
    }
    snprintf(st->lines[i].label, KBMIC_UI_LABEL_MAX, "%s", label);
    snprintf(st->lines[i].value, KBMIC_UI_VALUE_MAX, "%s", value ? value : "-");
}

static void build_view(kbmic_ui_state_t *st)
{
    const kbmic_config_t *cfg = kbmic_config_current();
    char name[KBMIC_ACTION_NAME_MAX];
    char buf[KBMIC_UI_VALUE_MAX];

    st->view = (kbmic_view_t)s_view;
    st->connected = s_connected;
    st->battery = s_battery;
    st->mode_name = cfg->profiles[cfg->active].name;
    st->line_count = 0;
    st->cursor = s_cursor[s_view];
    st->show_cursor = (s_view != ST_HOME);
    st->title[0] = '\0';
    st->footer[0] = '\0';
    st->voice_available = false;

    switch (s_view) {
    case ST_HOME: {
        set_title(st, "AI小键盘");
        st->voice_active = s_held[KBMIC_BTN_OK];
        const kbmic_action_t *ok = &cfg->profiles[cfg->active].slots[KBMIC_BTN_OK][KBMIC_SLOT_TAP];
        st->voice_available = !kbmic_action_is_settings(ok) && ok->trigger == KBMIC_TRIG_TAP;

        // 主页三行:每行一个键,右边是该键当前实际发送的东西。
        for (int b = 0; b < KBMIC_BTN_COUNT; b++) {
            kbmic_action_name(&cfg->profiles[cfg->active].slots[b][KBMIC_SLOT_TAP],
                              name, sizeof(name));
            set_line(st, b, btn_name(b), name);
        }
        st->line_count = KBMIC_BTN_COUNT;
        st->show_cursor = false;

        // OK 的长按槽若被配成了"进设置",主页就把这条提示写出来;
        // 否则用户根本不知道还能这么进设置菜单。
        const kbmic_action_t *ok_long =
            &cfg->profiles[cfg->active].slots[KBMIC_BTN_OK][KBMIC_SLOT_LONG];
        if (kbmic_action_is_settings(ok_long)) {
            set_footer(st, "长按 OK 进入设置");
        } else {
            set_footer(st, "长按 OK: %s", kbmic_action_name(ok_long, name, sizeof(name)));
        }

        // 配网热点开着时,主页底注让出位置给配网指引(存着配置也允许手动开)。
        appfw_net_status_t st_net;
        appfw_net_get_status(&st_net);
        if (st_net.portal_active) {
            set_footer(st, "配网中:连 %s 访问 192.168.4.1", st_net.ap_ssid);
        }
        break;
    }

    case ST_MENU: {
        set_title(st, "设置");
        static const char *items[MENU_ITEMS] = {"键盘模式", "按键配置", "恢复默认", "开启配网"};
        for (int i = 0; i < MENU_ITEMS; i++) {
            set_line(st, i, items[i],
                     i == 0 ? "" : (i == 2 ? "仅内置" : (i == 3 ? "连手机配" : "")));
        }
        st->line_count = MENU_ITEMS;
        set_footer(st, "OK 选中   长按 OK 返回");
        break;
    }

    case ST_MODES: {
        set_title(st, "键盘模式 %d/%d", s_cursor[ST_MODES] + 1, cfg->count);
        for (uint8_t i = 0; i < cfg->count && i < KBMIC_UI_LINES_MAX; i++) {
            set_line(st, i, cfg->profiles[i].name, i == cfg->active ? "使用中" : (cfg->profiles[i].builtin ? "内置" : "自定义"));
        }
        st->line_count = cfg->count > KBMIC_UI_LINES_MAX ? KBMIC_UI_LINES_MAX : cfg->count;
        set_footer(st, "OK 选用   长按 OK 返回");
        break;
    }

    case ST_KEYS: {
        set_title(st, "按键配置");
        st->mode_name = cfg->profiles[cfg->active].name;
        int row = 0;
        for (int b = 0; b < KBMIC_BTN_COUNT; b++) {
            for (int s = 0; s < KBMIC_SLOT_COUNT; s++) {
                kbmic_action_name(&cfg->profiles[cfg->active].slots[b][s], name, sizeof(name));
                snprintf(buf, sizeof(buf), "%s/%s", btn_name(b), slot_name(s));
                set_line(st, row++, buf, name);
            }
        }
        st->line_count = row;
        set_footer(st, "OK 更改   长按 OK 返回");
        break;
    }

    case ST_ACT: {
        set_title(st, "%s/%s 选动作", btn_name(s_act_btn), slot_name(s_act_slot));
        const int n = kbmic_catalog_count();
        for (int i = 0; i < n && i < KBMIC_UI_LINES_MAX; i++) {
            set_line(st, i, kbmic_catalog_get((uint8_t)i)->name, "");
        }
        st->line_count = n < KBMIC_UI_LINES_MAX ? n : KBMIC_UI_LINES_MAX;
        set_footer(st, "OK 选定   长按 OK 返回");
        break;
    }

    default:
        set_title(st, "?");
        break;
    }
}

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
    // 配置可能来自 MCP,把游标夹回合法范围,免得 MCP 删掉模式后光标停在越界行上。
    const kbmic_config_t *cfg = kbmic_config_current();
    if (s_cursor[ST_MODES] >= cfg->count) {
        s_cursor[ST_MODES] = cfg->count ? cfg->count - 1 : 0;
    }
    if (s_cursor[ST_MODES] < 0) {
        s_cursor[ST_MODES] = 0;
    }
}

static void app_task(void *arg)
{
    (void)arg;
    int msg;
    uint32_t loop = 0;

    for (;;) {
        while (s_key_queue && xQueueReceive(s_key_queue, &msg, 0) == pdTRUE) {
            handle_key(msg & 0xF, (bsp_btn_ev_t)((msg >> 4) & 0xF));
        }

        s_connected = kbmic_hid_connected();

        if (++loop % 50 == 0) {
            const int soc = bsp_battery_soc();
            if (soc != s_battery) {
                s_battery = soc;
                if (soc >= 0) {
                    kbmic_hid_set_battery(soc);
                }
            }
        }

        kbmic_ui_state_t st;
        build_view(&st);
        if (bsp_lvgl_lock(200)) {
            kbmic_ui_render(&st);
            bsp_lvgl_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(100));   // 10 fps
    }
}

// ---------------------------------------------------------------------------
// MCP 模拟触发回调
//
// 把合成按键事件投进 s_key_queue,与真人按键走完全同一条路径(状态机、HID、
// 界面反馈一致)。序列:PRESS → LONG/CLICK → CLICK(收尾,放掉按住的 TAP)。
// ---------------------------------------------------------------------------
static void simulate_inject(int btn, bool long_press)
{
    if (s_key_queue == NULL || !s_keys_ready) {
        return;
    }
    const int press = (btn & 0xF) | ((int)BSP_BTN_PRESS << 4);
    const int act = (btn & 0xF) |
                    (((int)(long_press ? BSP_BTN_LONG : BSP_BTN_CLICK)) << 4);
    const int rel = (btn & 0xF) | ((int)BSP_BTN_CLICK << 4);
    xQueueSend(s_key_queue, &press, 0);
    xQueueSend(s_key_queue, &act, 0);
    if (long_press) {
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

    kbmic_ui_init();   // 内部自行加解锁,调用方不要再套一层

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
