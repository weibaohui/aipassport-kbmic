// main/kbmic_action.c —— 动作执行器与内置动作目录。
#include "kbmic_action.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kbmic_hid.h"

static const char *TAG = "kbmic_action";

// TAP 动作按下报告到松开报告之间的保持时长。太短部分主机收不到,
// 太长用户会觉得"粘",20 ms 是通用取值。
#define ACTION_HOLD_MS 20

static bool s_key_held;
static bool s_consumer_held;
static bool s_applefn_held;

// ---------------------------------------------------------------------------
// 内置动作目录
//
// 这是"手指点得到"的那一小撮;想要任意组合(比如 Ctrl+Alt+Delete)由 MCP 写。
// 目录里的每一项最终仍然只是一段 step,所以两条路径执行时没有区别。
// ---------------------------------------------------------------------------
#define ACT_NONE     0
#define ACT_ENTER    1
#define ACT_BACK     2
#define ACT_TAB      3
#define ACT_SPACE    4
#define ACT_ESC      5
#define ACT_GLOBE    6
#define ACT_CTRL_WIN 7
#define ACT_UP       8
#define ACT_DOWN     9
#define ACT_LEFT     10
#define ACT_RIGHT    11
#define ACT_ENTER3   12
#define ACT_F1       13
#define ACT_F2       14
#define ACT_SETTINGS 15
#define ACT_APPLEFN  16

// 目录表必须能用在 static 初始化里,所以不能靠构造函数拼 —— 那不是常量表达式。
// 代价是每一条都要写满四层花括号(steps 是结构体数组),少一层就触发
// -Wmissing-braces,写错了编译器也不一定拦得住。用下面这几个宏把重复部分收掉,
// 并且**永远写满 4 步**:靠"少写几个再补零"省事,恰恰是漏初始化最常见的来源。
#define S_EMPTY   {KBMIC_STEP_NONE, 0, 0, 0, 0}
#define S_KEY(mods, keycode) {KBMIC_STEP_KEY, (mods), (keycode), 0, 0}
#define S_CONSUMER(usage)    {KBMIC_STEP_CONSUMER, 0, 0, (usage), 0}
// "进设置"这种纯软件动作:kind=NONE 且 mods 的高位 bit4 是保留标记。
#define S_SETTINGS           {KBMIC_STEP_NONE, 0xF0, 0, 0, 0}
#define S_APPLEFN            {KBMIC_STEP_APPLEFN, 0, 0, 0, 0}

// 一个动作:(触发方式, 步数, s0..s3)
#define ACT(trig, n, s0, s1, s2, s3) {(trig), (n), {s0, s1, s2, s3}}
// 目录项。action 展开后是一整个大括号组,外面**不能**再加括号 ——
// ({...}) 在 C 里是语句表达式,只在函数体内合法,用在文件作用域会报
// "braced-group within expression allowed only inside a function"。
#define ITEM(id, label, action) [id] = {label, action}

static const kbmic_catalog_item_t s_catalog[] = {
    ITEM(ACT_NONE, "-", ACT(KBMIC_TRIG_NONE, 0, S_EMPTY, S_EMPTY, S_EMPTY, S_EMPTY)),
    ITEM(ACT_ENTER, "Enter", ACT(KBMIC_TRIG_CLICK, 1, S_KEY(0, KBMIC_HID_KEY_ENTER), S_EMPTY, S_EMPTY, S_EMPTY)),
    ITEM(ACT_BACK, "Back", ACT(KBMIC_TRIG_CLICK, 1, S_KEY(0, KBMIC_HID_KEY_BACKSPACE), S_EMPTY, S_EMPTY, S_EMPTY)),
    ITEM(ACT_TAB, "Tab", ACT(KBMIC_TRIG_CLICK, 1, S_KEY(0, 0x2B), S_EMPTY, S_EMPTY, S_EMPTY)),
    ITEM(ACT_SPACE, "Space", ACT(KBMIC_TRIG_CLICK, 1, S_KEY(0, KBMIC_HID_KEY_SPACE), S_EMPTY, S_EMPTY, S_EMPTY)),
    ITEM(ACT_ESC, "Esc", ACT(KBMIC_TRIG_CLICK, 1, S_KEY(0, KBMIC_HID_KEY_ESCAPE), S_EMPTY, S_EMPTY, S_EMPTY)),
    ITEM(ACT_GLOBE, "Globe", ACT(KBMIC_TRIG_TAP, 1, S_CONSUMER(KBMIC_HID_USAGE_GLOBE), S_EMPTY, S_EMPTY, S_EMPTY)),
    ITEM(ACT_CTRL_WIN, "Ctrl+Win", ACT(KBMIC_TRIG_TAP, 1, S_KEY(KBMIC_MOD_CTRL | KBMIC_MOD_GUI, 0), S_EMPTY, S_EMPTY, S_EMPTY)),
    ITEM(ACT_UP, "Up", ACT(KBMIC_TRIG_CLICK, 1, S_KEY(0, 0x52), S_EMPTY, S_EMPTY, S_EMPTY)),
    ITEM(ACT_DOWN, "Down", ACT(KBMIC_TRIG_CLICK, 1, S_KEY(0, 0x51), S_EMPTY, S_EMPTY, S_EMPTY)),
    ITEM(ACT_LEFT, "Left", ACT(KBMIC_TRIG_CLICK, 1, S_KEY(0, 0x50), S_EMPTY, S_EMPTY, S_EMPTY)),
    ITEM(ACT_RIGHT, "Right", ACT(KBMIC_TRIG_CLICK, 1, S_KEY(0, 0x4F), S_EMPTY, S_EMPTY, S_EMPTY)),
    ITEM(ACT_ENTER3, "Enter x3", ACT(KBMIC_TRIG_CLICK, 3, S_KEY(0, KBMIC_HID_KEY_ENTER), S_KEY(0, KBMIC_HID_KEY_ENTER), S_KEY(0, KBMIC_HID_KEY_ENTER), S_EMPTY)),
    ITEM(ACT_F1, "F1", ACT(KBMIC_TRIG_CLICK, 1, S_KEY(0, 0x3A), S_EMPTY, S_EMPTY, S_EMPTY)),
    ITEM(ACT_F2, "F2", ACT(KBMIC_TRIG_CLICK, 1, S_KEY(0, 0x3B), S_EMPTY, S_EMPTY, S_EMPTY)),
    ITEM(ACT_SETTINGS, "Settings", ACT(KBMIC_TRIG_NONE, 1, S_SETTINGS, S_EMPTY, S_EMPTY, S_EMPTY)),
    ITEM(ACT_APPLEFN, "Apple Fn (hold)", ACT(KBMIC_TRIG_TAP, 1, S_APPLEFN, S_EMPTY, S_EMPTY, S_EMPTY)),
};

uint8_t kbmic_catalog_count(void)
{
    return (uint8_t)(sizeof(s_catalog) / sizeof(s_catalog[0]));
}

const kbmic_catalog_item_t *kbmic_catalog_get(uint8_t id)
{
    if (id >= kbmic_catalog_count()) {
        id = ACT_NONE;
    }
    return &s_catalog[id];
}

// ---------------------------------------------------------------------------
// 执行
// ---------------------------------------------------------------------------
bool kbmic_action_is_settings(const kbmic_action_t *a)
{
    return a != NULL && a->step_count > 0 && a->steps[0].kind == KBMIC_STEP_NONE &&
           (a->steps[0].mods & 0xF0) != 0;
}

esp_err_t kbmic_action_run(const kbmic_action_t *a)
{
    if (a == NULL || a->trigger == KBMIC_TRIG_NONE) {
        return ESP_OK;   // 没配就是什么都不做,不是错误
    }
    if (kbmic_action_is_settings(a)) {
        return ESP_ERR_NOT_SUPPORTED;   // 由 main 拦截并切界面
    }

    for (uint8_t i = 0; i < a->step_count; i++) {
        const kbmic_step_t *s = &a->steps[i];
        switch (s->kind) {
        case KBMIC_STEP_KEY:
            if (a->trigger == KBMIC_TRIG_TAP) {
                s_key_held = true;
                kbmic_hid_key_hold(s->mods, s->keycode, true);
            } else {
                kbmic_hid_tap(s->mods, s->keycode);
            }
            break;

        case KBMIC_STEP_CONSUMER:
            if (a->trigger == KBMIC_TRIG_TAP) {
                s_consumer_held = true;
                kbmic_hid_consumer(s->usage, true);
            } else {
                kbmic_hid_consumer(s->usage, true);
                vTaskDelay(pdMS_TO_TICKS(ACTION_HOLD_MS));
                kbmic_hid_consumer(s->usage, false);
            }
            break;

        case KBMIC_STEP_APPLEFN:
            // 按住 Apple Fn:AppleVendor Top Case usage 0x03 放入键盘报告第 2 字节。
            if (a->trigger == KBMIC_TRIG_TAP) {
                s_applefn_held = true;
                kbmic_hid_applefn(true);
            } else {
                kbmic_hid_applefn(true);
                vTaskDelay(pdMS_TO_TICKS(ACTION_HOLD_MS));
                kbmic_hid_applefn(false);
            }
            break;

        case KBMIC_STEP_DELAY:
            // 延时步在 TAP 动作里没有意义(按住期间本来就一直在发状态),
            // 但也不该让整条动作失败,照睡即可。
            vTaskDelay(pdMS_TO_TICKS(s->delay_ms ? s->delay_ms : 1));
            break;

        default:
            break;
        }
    }
    return ESP_OK;
}

esp_err_t kbmic_action_release(void)
{
    if (s_key_held) {
        s_key_held = false;
        kbmic_hid_key_hold(0, 0, false);
    }
    if (s_consumer_held) {
        s_consumer_held = false;
        kbmic_hid_consumer(0, false);
    }
    if (s_applefn_held) {
        s_applefn_held = false;
        kbmic_hid_applefn(false);
    }
    return ESP_OK;
}
