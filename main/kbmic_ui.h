// main/kbmic_ui.h —— 界面渲染。
//
// 分工:本模块只负责"把状态画出来",不持有任何业务状态,也不知道什么是模式、
// 什么是长按。谁处于哪个页面、每一行显示什么文字,全部由 main.c 的状态机算好
// 填进 kbmic_ui_state_t。这样加页面不用碰渲染代码,改渲染也不用碰状态机。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// 五个页面。顺序即返回栈的深度。
typedef enum {
    KBMIC_VIEW_HOME = 0,   // 主页:3 行,各显示一个键当前发什么
    KBMIC_VIEW_MENU,       // 设置菜单
    KBMIC_VIEW_MODES,      // 键盘模式选择
    KBMIC_VIEW_KEYS,       // 按键配置(3 键 × 2 槽)
    KBMIC_VIEW_ACT,        // 某个槽的动作选择
} kbmic_view_t;

#define KBMIC_UI_LINES_MAX 8
#define KBMIC_UI_LABEL_MAX 14
#define KBMIC_UI_VALUE_MAX 28

typedef struct {
    char label[KBMIC_UI_LABEL_MAX];              // 左列,如 "上 / 短按"
    char value[KBMIC_UI_VALUE_MAX];              // 右列,如 "Ctrl+Win"
} kbmic_ui_line_t;

// 标题/底注都做成定长数组而不是 const char *:状态是由调用方在栈上临时拼出来的,
// 存指针进去就是悬垂引用,而"标题里带个计数"又天生容易把定长缓冲写溢出。
// 定长字段把这两类问题都变成编译期可见。
typedef struct {
    kbmic_view_t view;
    bool connected;
    int battery;                       // <0 表示未知
    const char *mode_name;             // 主页右侧显示的当前模式名
    char title[KBMIC_UI_VALUE_MAX];
    char footer[KBMIC_UI_VALUE_MAX];   // 空串表示不显示底注
    bool voice_active;                 // 主页:按住"说话键"期间
    bool voice_available;              // 主页:存在"按住说话"槽(TAP 触发的语音键)
    const char *voice_btn;             // 说话键显示名("上键"/"OK",main.c 静态串)
    kbmic_ui_line_t lines[KBMIC_UI_LINES_MAX];
    int line_count;
    int cursor;                        // 选中行,仅列表页有意义
    bool show_cursor;
} kbmic_ui_state_t;

// 建一次控件。之后只反复调 kbmic_ui_render,内部不新建对象。
esp_err_t kbmic_ui_init(void);

// 重画。调用方必须已持有 LVGL 锁(本函数自己不抢锁)。
void kbmic_ui_render(const kbmic_ui_state_t *st);
