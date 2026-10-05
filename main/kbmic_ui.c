// main/kbmic_ui.c —— 界面渲染。见 kbmic_ui.h 的职责划分说明。
//
// 布局模式与框架 appfw_ui(收音机/GLM 同款,真机验证过)保持一致:
// 自建 screen + lv_screen_load,底色直接铺在 screen 上;子控件用
// lv_obj_set_pos 定位。不要改回"默认屏 + style x/y"的写法 —— 真机上
// 出现过整屏只刷左上角、其余全白的问题(2026-10-05)。
#include "kbmic_ui.h"

#include <stdio.h>

#include "bsp_display.h"
#include "esp_log.h"
#include "lvgl.h"

LV_FONT_DECLARE(app_font_16);

static const char *TAG = "kbmic_ui";

// 屏幕 240x320。下面的 y 坐标都按这个来,别改成"差不多"的值 —— 改之前先量。
#define UI_PAD_X 12
#define UI_TITLE_Y 8
#define UI_HERO_Y 40
#define UI_HERO_H 84
#define UI_MODE_Y 134
#define UI_LIST_Y 172
#define UI_LINE_H 26
#define UI_FOOTER_Y 296

#define UI_BG lv_color_hex(0x101418)
#define UI_FG lv_color_hex(0xE8EDF2)
#define UI_DIM lv_color_hex(0x6E7A86)
#define UI_ACCENT lv_color_hex(0x07C160)   // 微信绿
#define UI_CARD lv_color_hex(0x1A2027)
#define UI_CARD_HI lv_color_hex(0x2A333D)
#define UI_WARN lv_color_hex(0xE6A23C)

static lv_obj_t *s_scr;
static lv_obj_t *s_title;
static lv_obj_t *s_dot;
static lv_obj_t *s_status;
static lv_obj_t *s_hero;
static lv_obj_t *s_hero_text;
static lv_obj_t *s_mode;
static lv_obj_t *s_batt;
static lv_obj_t *s_rows[KBMIC_UI_LINES_MAX][2];   // [行][0]=标签 [1]=值
static lv_obj_t *s_footer;

// 记住上一次重画时的行数与选中行。行数变少时要把多出来的行藏掉,
// 否则会残留上一个页面的文字 —— 这是"控件复用"最常见的坑。
static int s_last_count = -1;
static int s_last_cursor = -1;
static bool s_last_voice;

static void style_box(lv_obj_t *o, lv_color_t bg, lv_coord_t radius)
{
    lv_obj_set_style_bg_color(o, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
}

static void style_text(lv_obj_t *o, lv_color_t c)
{
    lv_obj_set_style_text_font(o, &app_font_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(o, c, LV_PART_MAIN);
}

static lv_obj_t *mk_label(lv_obj_t *parent, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *l = lv_label_create(parent);
    style_text(l, UI_FG);
    lv_obj_set_pos(l, x, y);
    return l;
}

static void set_text(lv_obj_t *l, const char *s)
{
    // LVGL 会拷贝字符串内容,传 NULL 会被当成空串处理,不会崩。
    lv_label_set_text(l, s ? s : "");
}

static void show(lv_obj_t *o, bool on)
{
    if (on) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

esp_err_t kbmic_ui_init(void)
{
    if (!bsp_lvgl_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    // 自建 screen 并加载:底色铺满整个屏幕,不经过中间容器。
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, UI_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_screen_load(s_scr);

    s_title = mk_label(s_scr, UI_PAD_X, UI_TITLE_Y);

    s_dot = lv_obj_create(s_scr);
    lv_obj_set_size(s_dot, 12, 12);
    lv_obj_set_pos(s_dot, 196, UI_TITLE_Y + 4);
    style_box(s_dot, UI_DIM, LV_RADIUS_CIRCLE);

    s_status = mk_label(s_scr, 144, UI_TITLE_Y + 2);
    style_text(s_status, UI_DIM);

    // 语音面板:主页上的大圆角块。按住 OK 时整块变绿。
    s_hero = lv_obj_create(s_scr);
    lv_obj_set_size(s_hero, 216, UI_HERO_H);
    lv_obj_set_pos(s_hero, UI_PAD_X, UI_HERO_Y);
    style_box(s_hero, UI_CARD, 14);

    s_hero_text = lv_label_create(s_hero);
    style_text(s_hero_text, UI_DIM);
    lv_obj_center(s_hero_text);

    s_mode = mk_label(s_scr, UI_PAD_X, UI_MODE_Y);
    s_batt = mk_label(s_scr, 150, UI_MODE_Y);

    for (int i = 0; i < KBMIC_UI_LINES_MAX; i++) {
        s_rows[i][0] = mk_label(s_scr, UI_PAD_X, UI_LIST_Y + i * UI_LINE_H);
        s_rows[i][1] = mk_label(s_scr, 108, UI_LIST_Y + i * UI_LINE_H);
        style_text(s_rows[i][0], UI_DIM);
    }

    s_footer = mk_label(s_scr, UI_PAD_X, UI_FOOTER_Y);
    style_text(s_footer, UI_DIM);

    bsp_lvgl_unlock();
    ESP_LOGI(TAG, "界面创建完成");
    return ESP_OK;
}

// 列表区能放下几行。留一行给视觉间隔,免得最后一条贴着 footer。
#define UI_VISIBLE_ROWS ((UI_FOOTER_Y - 12 - UI_LIST_Y) / UI_LINE_H)

void kbmic_ui_render(const kbmic_ui_state_t *st)
{
    if (s_scr == NULL || st == NULL) {
        return;
    }

    const bool is_home = (st->view == KBMIC_VIEW_HOME);

    set_text(s_title, st->title);
    style_text(s_title, UI_FG);

    // 连接状态只在主页显示:其他页面空间紧张,且用户已经进设置了就说明连着。
    if (is_home) {
        lv_obj_set_style_bg_color(s_dot, st->connected ? UI_ACCENT : UI_DIM, LV_PART_MAIN);
        set_text(s_status, st->connected ? "已连接" : "等待配对");
        style_text(s_status, st->connected ? UI_ACCENT : UI_WARN);
        show(s_dot, true);
        show(s_status, true);
    } else {
        show(s_dot, false);
        show(s_status, false);
    }

    // --- 语音面板 ---
    if (is_home) {
        show(s_hero, true);
        if (st->voice_active) {
            style_box(s_hero, UI_ACCENT, 14);
            set_text(s_hero_text, "说话中");
            style_text(s_hero_text, lv_color_hex(0x06210F));
        } else {
            style_box(s_hero, UI_CARD, 14);
            // 点明是哪个键:说话就是"按住 OK"(TAP 触发,按多久说多久)。
            set_text(s_hero_text, st->voice_available ? "按住 OK 说话" : "按住 无动作");
            style_text(s_hero_text, st->voice_available ? UI_FG : UI_DIM);
        }
        s_last_voice = st->voice_active;

        char buf[KBMIC_UI_VALUE_MAX];
        snprintf(buf, sizeof(buf), "模式 %s", st->mode_name ? st->mode_name : "-");
        set_text(s_mode, buf);
        style_text(s_mode, UI_FG);
        if (st->battery >= 0) {
            snprintf(buf, sizeof(buf), "电量 %d%%", st->battery);
        } else {
            snprintf(buf, sizeof(buf), "电量 --");
        }
        set_text(s_batt, buf);
        style_text(s_batt, UI_DIM);
        show(s_mode, true);
        show(s_batt, true);
    } else {
        show(s_hero, false);
        show(s_mode, false);
        show(s_batt, false);
    }

    // --- 行 ---
    // 行数变少:上一屏多出来的行必须显式隐藏,不能靠"这次没画它"。
    if (st->line_count < s_last_count) {
        for (int i = st->line_count; i < s_last_count; i++) {
            show(s_rows[i][0], false);
            show(s_rows[i][1], false);
        }
    }
    s_last_count = st->line_count;
    s_last_cursor = st->cursor;

    for (int i = 0; i < st->line_count; i++) {
        // 行数超过一屏时让选中行始终可见:窗口随光标滑动。
        int first = 0;
        if (st->line_count > UI_VISIBLE_ROWS) {
            first = st->cursor - UI_VISIBLE_ROWS + 1;
            if (first < 0) {
                first = 0;
            }
            const int max_first = st->line_count - UI_VISIBLE_ROWS;
            if (first > max_first) {
                first = max_first;
            }
        }
        const bool visible = (i >= first && i < first + UI_VISIBLE_ROWS);
        show(s_rows[i][0], visible);
        show(s_rows[i][1], visible);
        if (!visible) {
            continue;
        }

        const int slot = i - first;
        lv_obj_set_pos(s_rows[i][0], UI_PAD_X, UI_LIST_Y + slot * UI_LINE_H);
        lv_obj_set_pos(s_rows[i][1], 108, UI_LIST_Y + slot * UI_LINE_H);

        char label[KBMIC_UI_LABEL_MAX + 4];
        if (st->show_cursor && i == st->cursor) {
            snprintf(label, sizeof(label), "▸%s", st->lines[i].label);
        } else {
            snprintf(label, sizeof(label), " %s", st->lines[i].label);
        }
        set_text(s_rows[i][0], label);
        set_text(s_rows[i][1], st->lines[i].value);

        const bool sel = st->show_cursor && i == st->cursor;
        style_text(s_rows[i][0], sel ? UI_ACCENT : UI_DIM);
        style_text(s_rows[i][1], sel ? UI_ACCENT : UI_FG);
    }

    set_text(s_footer, st->footer);
    style_text(s_footer, UI_DIM);
    show(s_footer, st->footer[0] != '\0');
}
