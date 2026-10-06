// main/kbmic_ui.c —— 界面渲染(appfw_ui 框架版)。
//
// 布局铁律沿用 240x320;主页与子页都画在框架给的容器里,
// 控件句柄在 build 里整体重建(框架切页会删容器,旧句柄全部失效)。
#include "kbmic_ui.h"

#include <stdio.h>

#include "bsp_battery.h"
#include "esp_log.h"
#include "kbmic_action.h"
#include "kbmic_config.h"
#include "kbmic_hid.h"
#include "kbmic_store.h"

LV_FONT_DECLARE(app_font_16);

static const char *TAG = "kbmic_ui";

#define UI_PAD_X 12
#define UI_HERO_Y 40
#define UI_HERO_H 92
#define UI_INFO_Y 148
#define UI_LINE_H 24
#define NV_ROWS_MAX 10            // 模式层 1+8+1 / 槽位层 1+9 / 目录层 1+17 滚动
#define NV_LINE_H 24

#define UI_FG   0xE8EDF2
#define UI_DIM  0x6E7A86
#define UI_ACCENT 0x07C160
#define UI_CARD 0x1A2027
#define UI_WARN 0xE6A23C

// ---------------------------------------------------------------------------
// 说话状态(main 的按键回调更新)
// ---------------------------------------------------------------------------
static bool s_voice_active;
static char s_voice_btn[8];

void kbmic_ui_set_voice(bool active, const char *btn_label)
{
    s_voice_active = active;
    snprintf(s_voice_btn, sizeof(s_voice_btn), "%s", btn_label ? btn_label : "?");
}

// ---------------------------------------------------------------------------
// 通用小控件
// ---------------------------------------------------------------------------
static void style_box(lv_obj_t *o, uint32_t bg, lv_coord_t radius)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
}

static lv_obj_t *mk_label(lv_obj_t *parent, int x, int y, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &app_font_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_pos(l, x, y);
    return l;
}

// ---------------------------------------------------------------------------
// 主页:语音大块 + 状态行
// ---------------------------------------------------------------------------
static lv_obj_t *s_hero, *s_hero_text;
static lv_obj_t *s_line_mode, *s_line_link, *s_line_batt;

void kbmic_home_build(lv_obj_t *parent)
{
    s_hero = lv_obj_create(parent);
    lv_obj_set_size(s_hero, 216, UI_HERO_H);
    lv_obj_set_pos(s_hero, UI_PAD_X, UI_HERO_Y);
    style_box(s_hero, UI_CARD, 14);

    s_hero_text = lv_label_create(s_hero);
    lv_obj_set_style_text_font(s_hero_text, &app_font_16, LV_PART_MAIN);
    lv_obj_center(s_hero_text);

    s_line_mode = mk_label(parent, UI_PAD_X, UI_INFO_Y, UI_FG);
    s_line_link = mk_label(parent, UI_PAD_X, UI_INFO_Y + UI_LINE_H, UI_DIM);
    s_line_batt = mk_label(parent, UI_PAD_X, UI_INFO_Y + UI_LINE_H * 2, UI_DIM);
}

void kbmic_home_poll(void)
{
    const kbmic_config_t *cfg = kbmic_config_current();
    const bool linked = kbmic_hid_connected();
    const int soc = bsp_battery_soc();

    if (s_voice_active) {
        style_box(s_hero, UI_ACCENT, 14);
        lv_obj_set_style_text_color(s_hero_text, lv_color_hex(0x06210F), 0);
        lv_label_set_text_fmt(s_hero_text, "说话中(松开%s结束)", s_voice_btn);
    } else if (s_voice_btn[0]) {
        style_box(s_hero, UI_CARD, 14);
        lv_obj_set_style_text_color(s_hero_text, lv_color_hex(UI_FG), 0);
        lv_label_set_text_fmt(s_hero_text, "按住%s 说话", s_voice_btn);
    } else {
        style_box(s_hero, UI_CARD, 14);
        lv_obj_set_style_text_color(s_hero_text, lv_color_hex(UI_DIM), 0);
        lv_label_set_text(s_hero_text, "语音键未配置");
    }

    lv_label_set_text_fmt(s_line_mode, "模式 %s(%u/%u)",
                          cfg->profiles[cfg->active].name,
                          (unsigned)(cfg->active + 1), (unsigned)cfg->count);
    lv_label_set_text(s_line_link, linked ? "蓝牙 已连接" : "蓝牙 等待配对");
    if (soc >= 0) lv_label_set_text_fmt(s_line_batt, "电量 %d%%", soc);
    else lv_label_set_text(s_line_batt, "电量 --");
}

// ---------------------------------------------------------------------------
// 键盘设置子页:模式列表 / 按键配置 / 动作选择
//   每层首行固定"< 返回";长按 OK 由框架统一退出整个子页。
// ---------------------------------------------------------------------------
typedef enum {
    NAV_MODES = 0,   // 模式列表(选用 / + 新建)
    NAV_KEYS,        // 当前模式 9 槽(3 键 × 短按/双击/长按)
    NAV_ACT,         // 某槽的动作选择(内置目录)
    NAV_LAYER_N
} nav_layer_t;

static nav_layer_t s_nav_view;
static int s_nav_cursor[NAV_LAYER_N];
static int s_nav_btn, s_nav_slot;      // NAV_ACT 正在改哪个槽

static lv_obj_t *s_nv_rows[NV_ROWS_MAX][2];
static lv_obj_t *s_nv_footer;

static const char *nav_btn_name(int b)
{
    switch (b) {
    case KBMIC_BTN_UP:   return "上键";
    case KBMIC_BTN_DOWN: return "下键";
    default:             return "OK";
    }
}

static const char *nav_slot_name(int s)
{
    switch (s) {
    case KBMIC_SLOT_DOUBLE: return "双击";
    case KBMIC_SLOT_LONG:   return "长按";
    default:                return "短按";
    }
}

// 各层行数(含首行"< 返回")
static int nav_rows(void)
{
    switch (s_nav_view) {
    case NAV_MODES: {
        const int n = kbmic_config_current()->count;
        const int extra = (n < KBMIC_MAX_PROFILES) ? 1 : 0;   // "+ 新建模式"
        return 1 + n + extra;
    }
    case NAV_KEYS: return 1 + KBMIC_BTN_COUNT * KBMIC_SLOT_COUNT;
    case NAV_ACT: {
        const int n = 1 + kbmic_catalog_count();
        return n > NV_ROWS_MAX ? NV_ROWS_MAX : n;
    }
    default: return 1;
    }
}

// 某层第 row 行(0=返回) 的左右列文本与选中态,写入 out(定长小缓冲)
typedef struct {
    char left[24], right[56];
    bool sel;
} nav_row_t;

static void nav_row_text(int row, nav_row_t *out)
{
    out->left[0] = out->right[0] = '\0';
    out->sel = (row == s_nav_cursor[s_nav_view]);
    const kbmic_config_t *cfg = kbmic_config_current();

    if (row == 0) {
        snprintf(out->left, sizeof(out->left), "< 返回");
        return;
    }
    switch (s_nav_view) {
    case NAV_MODES: {
        const int n = cfg->count;
        const int idx = row - 1;
        if (idx < n) {
            snprintf(out->left, sizeof(out->left), "%s", cfg->profiles[idx].name);
            snprintf(out->right, sizeof(out->right), "%s%s",
                     idx == (int)cfg->active ? "使用中" :
                     (cfg->profiles[idx].builtin ? "内置" : "自定义"),
                     idx == (int)cfg->active ? "" :
                     (cfg->profiles[idx].builtin ? "" : ""));
        } else {
            snprintf(out->left, sizeof(out->left), "+ 新建模式");
        }
        break;
    }
    case NAV_KEYS: {
        const int idx = row - 1;
        const int b = idx / KBMIC_SLOT_COUNT, s = idx % KBMIC_SLOT_COUNT;
        char name[KBMIC_ACTION_NAME_MAX];
        snprintf(out->left, sizeof(out->left), "%s/%s",
                 nav_btn_name(b), nav_slot_name(s));
        snprintf(out->right, sizeof(out->right), "%s",
                 kbmic_action_name(&cfg->profiles[cfg->active].slots[b][s],
                                   name, sizeof(name)));
        break;
    }
    case NAV_ACT: {
        const int idx = row - 1;
        if (idx < kbmic_catalog_count()) {
            snprintf(out->left, sizeof(out->left), "%s", kbmic_catalog_get((uint8_t)idx)->name);
        }
        break;
    }
    default: break;
    }
}

void kbmic_nav_enter(void)
{
    s_nav_view = NAV_MODES;
    s_nav_cursor[NAV_MODES] = kbmic_config_current()->active + 1;
    s_nav_cursor[NAV_KEYS] = 1;
    s_nav_cursor[NAV_ACT] = 1;
}

void kbmic_nav_build(lv_obj_t *parent)
{
    for (int i = 0; i < NV_ROWS_MAX; i++) {
        s_nv_rows[i][0] = mk_label(parent, UI_PAD_X, 48 + i * NV_LINE_H, UI_FG);
        s_nv_rows[i][1] = mk_label(parent, 118, 48 + i * NV_LINE_H, UI_DIM);
    }
    s_nv_footer = mk_label(parent, UI_PAD_X, 296, UI_DIM);
}

void kbmic_nav_poll(void)
{
    const int rows = nav_rows();
    static const char *const k_title[NAV_LAYER_N] = { "键盘模式", "按键配置", "选动作" };
    for (int i = 0; i < NV_ROWS_MAX; i++) {
        const bool show = i < rows;
        if (!show) {
            lv_obj_add_flag(s_nv_rows[i][0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_nv_rows[i][1], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_nv_rows[i][0], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_nv_rows[i][1], LV_OBJ_FLAG_HIDDEN);
        nav_row_t r;
        nav_row_text(i, &r);
        lv_label_set_text_fmt(s_nv_rows[i][0], "%s%s", r.sel ? "> " : "  ", r.left);
        lv_label_set_text(s_nv_rows[i][1], r.right);
        lv_obj_set_style_text_color(s_nv_rows[i][0],
                                    lv_color_hex(r.sel ? UI_ACCENT : UI_FG), 0);
    }
    lv_label_set_text_fmt(s_nv_footer, "%s  上=1行 OK=选中 长按OK=退出",
                          k_title[s_nav_view]);
}

// 返回 false 让框架退回设置菜单;层内导航自己消化。
bool kbmic_nav_key(int btn, int ev)
{
    if (ev != 0) return true;              // 只处理单击(框架约定 ev 0=CLICK)
    const int rows = nav_rows();
    int cur = s_nav_cursor[s_nav_view];

    if (btn == KBMIC_BTN_UP) {
        cur = (cur + rows - 1) % rows;
    } else if (btn == KBMIC_BTN_DOWN) {
        cur = (cur + 1) % rows;
    } else if (btn == KBMIC_BTN_OK) {
        if (cur == 0) {                    // "< 返回"
            switch (s_nav_view) {
            case NAV_MODES: return false;              // 退出子页回菜单
            case NAV_KEYS: s_nav_view = NAV_MODES; break;
            case NAV_ACT:  s_nav_view = NAV_KEYS; break;
            default: return false;
            }
        } else {
            switch (s_nav_view) {
            case NAV_MODES: {
                kbmic_config_t work = *kbmic_config_current();
                const int idx = cur - 1;
                if (idx >= work.count) {               // "+ 新建模式"
                    char name[KBMIC_NAME_MAX];
                    snprintf(name, sizeof(name), "自定义%d",
                             work.count - KBMIC_BUILTIN_MODES + 1);
                    const int ni = kbmic_config_add_profile(&work, name);
                    if (ni >= 0) {
                        work.active = (uint8_t)ni;
                        kbmic_config_commit(&work);
                        s_nav_cursor[NAV_MODES] = ni + 1;
                        ESP_LOGI(TAG, "新建模式 %s(#%d)", name, ni);
                    }
                } else {                                // 选用
                    work.active = (uint8_t)idx;
                    kbmic_config_commit(&work);
                    ESP_LOGI(TAG, "切换到模式 %s", work.profiles[idx].name);
                }
                break;
            }
            case NAV_KEYS: {
                const int idx = cur - 1;
                s_nav_btn = idx / KBMIC_SLOT_COUNT;
                s_nav_slot = idx % KBMIC_SLOT_COUNT;
                s_nav_view = NAV_ACT;
                s_nav_cursor[NAV_ACT] = 1;
                break;
            }
            case NAV_ACT: {
                kbmic_config_t work = *kbmic_config_current();
                kbmic_action_t a = kbmic_catalog_get((uint8_t)(cur - 1))->action;
                // 槽的触发语义跟槽走:长按槽=LONG,双击槽=DOUBLE
                if (s_nav_slot == KBMIC_SLOT_LONG) a.trigger = KBMIC_TRIG_LONG;
                else if (s_nav_slot == KBMIC_SLOT_DOUBLE) a.trigger = KBMIC_TRIG_DOUBLE;
                work.profiles[work.active].slots[s_nav_btn][s_nav_slot] = a;
                kbmic_config_commit(&work);
                s_nav_view = NAV_KEYS;
                ESP_LOGI(TAG, "槽 %s/%s 已设动作", nav_btn_name(s_nav_btn),
                         nav_slot_name(s_nav_slot));
                break;
            }
            default: break;
            }
        }
        return true;
    }
    s_nav_cursor[s_nav_view] = cur;
    return true;
}
