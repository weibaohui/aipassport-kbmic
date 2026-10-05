// main/kbmic_config.h —— 键盘模式的配置模型(固件与 MCP 之间的共同契约)。
//
// 这份布局是**线协议**的一部分:MCP 服务端按同样的字节序解析/生成设备配置。
// 改动任何字段都必须同步 bump KBMIC_CONFIG_VERSION,并且同步 mcp_server/protocol.py,
// 否则老客户端会按旧布局把新数据解析成一团乱码。
//
// 设计取舍:
//   * 模式:4 个内置(Mac / Windows / Android / iOS)+ 最多 4 个用户自定义,上限 8。
//   * 每个模式 3 个键 × 2 个触发槽(短按 / 长按)。为什么是 2 而不是 1:
//     "按住说话"要按下即发、松手才停,和"点一下回车"不是同一种触发语义,
//     但它们占的是同一个物理键,拆成两个槽最省事。
//   * 一个动作由最多 4 步组成,每步可以是一次带修饰键的敲击、一次 Consumer
//     用法,或一步延时。所以"Ctrl+Win"是两步,"回车回车回车"是三步。
//   * 名字用 UTF-8 存模式名(用户自定义模式要能取中文名),动作名则**不存字符串**,
//     由步骤自动拼成 ASCII 的 "Ctrl+Win" 这样的显示文本 —— 见 kbmic_action_name()。
//     这样界面字体只需要覆盖模式名,不依赖固件里一张无限增长的标签表。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 蓝牙设备名。用户在系统蓝牙列表里看到的就是它,11 字节 UTF-8。
#define KBMIC_BT_NAME "AI小键盘"

#define KBMIC_MAX_PROFILES 8    // 硬上限
#define KBMIC_BUILTIN_MODES 4   // 前 4 个是内置的,不可删除
#define KBMIC_NAME_MAX 16       // 模式名 UTF-8 字节数上限(含结尾 '\0',即最多 15 字节)
#define KBMIC_SEQ_MAX 4         // 一个动作最多 4 步
#define KBMIC_BTN_COUNT 3       // 上 / 下 / OK
#define KBMIC_SLOT_COUNT 2      // 短按 / 长按

#define KBMIC_CONFIG_VERSION 1

// ---------------------------------------------------------------------------
// HID 键码与用法码
//
// 放在这里而不是 kbmic_hid.h,因为它们描述的是**线上格式**:kbmic_model.c 拼
// 出厂默认时要用,MCP 侧也要按同样的数字解码。跟着传输层走,模型就没法在主机上
// 编译测试了(而 kbmic_hid.h 依赖 esp_err.h)。
// ---------------------------------------------------------------------------
#define KBMIC_HID_MOD_LCTRL 0x01
#define KBMIC_HID_MOD_LSHIFT 0x02
#define KBMIC_HID_MOD_LALT 0x04
#define KBMIC_HID_MOD_LGUI 0x08

#define KBMIC_HID_KEY_ENTER 0x28
#define KBMIC_HID_KEY_ESCAPE 0x29
#define KBMIC_HID_KEY_BACKSPACE 0x2A
#define KBMIC_HID_KEY_TAB 0x2B
#define KBMIC_HID_KEY_SPACE 0x2C

// Apple Accessory Design Guidelines 定义的 Globe/Fn 键编码(Consumer Page 0x0C)。
#define KBMIC_HID_USAGE_GLOBE 0x029D

// 按键索引。顺序与 kbmic_ui / BSP 的 BSP_BTN_* 一致。
typedef enum {
    KBMIC_BTN_UP = 0,
    KBMIC_BTN_DOWN,
    KBMIC_BTN_OK,
} kbmic_button_t;

// 触发槽。
typedef enum {
    KBMIC_SLOT_TAP = 0,   // 短按:按下即发,保持到松手
    KBMIC_SLOT_LONG,      // 长按:按住超过阈值时发一次
} kbmic_slot_t;

// 触发语义。存在槽里而不是"按下就发、短按发一次"里区分,是因为 TAP 与 CLICK
// 在 HID 上的区别就是"按下报告要不要等到松手才撤"。
typedef enum {
    KBMIC_TRIG_NONE = 0,   // 该槽不响应
    KBMIC_TRIG_CLICK,      // 松手时发一次完整序列
    KBMIC_TRIG_TAP,        // 按下即发,松手时补一条全零报告
    KBMIC_TRIG_LONG,       // 长按阈值到时发一次完整序列
} kbmic_trigger_t;

typedef enum {
    KBMIC_STEP_NONE = 0,
    KBMIC_STEP_KEY,        // 键盘报告:mods + keycode
    KBMIC_STEP_CONSUMER,   // Consumer 报告:16 位 usage(Globe 之类)
    KBMIC_STEP_DELAY,      // 什么都不发,只等 delay_ms
} kbmic_step_kind_t;

// 修饰键位。顺序即 HID 键盘报告第 0 字节的 bit 位。
#define KBMIC_MOD_CTRL 0x01
#define KBMIC_MOD_SHIFT 0x02
#define KBMIC_MOD_ALT 0x04
#define KBMIC_MOD_GUI 0x08

// 一步。packed:这张表要原样落到 BLE 报文和 NVS 里,不能有编译器插进来的对齐空洞,
// 否则固件与不同编译器下的客户端会读出不同布局。
typedef struct __attribute__((packed)) {
    uint8_t kind;      // kbmic_step_kind_t
    uint8_t mods;      // KBMIC_MOD_* 按位或
    uint8_t keycode;   // HID Keyboard/Keypad 用法码
    uint16_t usage;    // Consumer 用法码
    uint16_t delay_ms; // 仅 STEP_DELAY 使用
} kbmic_step_t;       // 7 字节

typedef struct __attribute__((packed)) {
    uint8_t trigger;         // kbmic_trigger_t
    uint8_t step_count;      // <= KBMIC_SEQ_MAX
    kbmic_step_t steps[KBMIC_SEQ_MAX];
} kbmic_action_t;            // 2 + 28 = 30 字节

typedef struct __attribute__((packed)) {
    char name[KBMIC_NAME_MAX]; // 模式名,UTF-8,必须以 '\0' 结尾
    uint8_t builtin;           // 1 = 内置,不可删除/覆盖其身份
    kbmic_action_t slots[KBMIC_BTN_COUNT][KBMIC_SLOT_COUNT];
} kbmic_profile_t;            // 16 + 1 + 180 = 197 字节

typedef struct __attribute__((packed)) {
    uint8_t version;   // KBMIC_CONFIG_VERSION
    uint8_t active;    // 当前生效的模式索引
    uint8_t count;     // 模式总数,<= KBMIC_MAX_PROFILES
    uint8_t reserved;  // 留空,保持 4 字节头对齐
    kbmic_profile_t profiles[KBMIC_MAX_PROFILES];
} kbmic_config_t;     // 4 + 8*197 = 1580 字节

#define KBMIC_CONFIG_SIZE ((uint16_t)sizeof(kbmic_config_t))

// 置位:把内置四模式与「按住的默认行为」装好。count 置 4,active 置 0(Mac)。
void kbmic_config_defaults(kbmic_config_t *cfg);

// 越界/字段非法的自检。MCP 写进来的数据在用它之前必须过这一关。
bool kbmic_config_valid(const kbmic_config_t *cfg);

// 追加一个空模式,返回新索引;已满返回 -1。名字按 UTF-8 截断到 15 字节。
int kbmic_config_add_profile(kbmic_config_t *cfg, const char *name);

// 删除一个**非内置**模式;成功返回 0,内置或越界返回 -1。
int kbmic_config_delete_profile(kbmic_config_t *cfg, uint8_t index);

// 把某个模式重置回该索引的内置默认值(仅索引 < KBMIC_BUILTIN_MODES 时有意义)。
int kbmic_config_reset_profile(kbmic_config_t *cfg, uint8_t index);

// 从步骤反推一个 ASCII 显示名,例如 "Ctrl+Win"、"Globe"、"Enter Enter"。
// 写进 buf(至少 KBMIC_ACTION_NAME_MAX 字节),返回 buf。动作名不落 NVS,就是这个原因。
#define KBMIC_ACTION_NAME_MAX 40
char *kbmic_action_name(const kbmic_action_t *a, char *buf, size_t cap);
