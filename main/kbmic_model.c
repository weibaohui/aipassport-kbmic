// main/kbmic_model.c —— 配置模型的纯逻辑:出厂默认、合法性、增删模式、动作名反推。
//
// 这个文件刻意**不依赖任何 ESP-IDF 头**。它做的事全是数据变换,没有硬件、没有
// 平台、没有时钟,所以可以在主机上直接编译并跑单元测试
// (tests/test_kbmic_model.c)。持久化那一半在 kbmic_config.c,那边要 NVS 和
// esp_log,没法在主机上链接 —— 混在一起就等于这部分逻辑永远只能靠烧板子验证。
//
// 动作名反推尤其需要这个测试:MCP 侧的 mcp_server/catalog.py 有一份同样的实现,
// 两边算出来的字符串必须逐字一致,否则设备屏幕和 MCP 返回值会显示不同的名字。
#include "kbmic_config.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// 构造辅助
// ---------------------------------------------------------------------------
static kbmic_step_t step_key(uint8_t mods, uint8_t keycode)
{
    return (kbmic_step_t){
        .kind = KBMIC_STEP_KEY,
        .mods = mods,
        .keycode = keycode,
        .usage = 0,
        .delay_ms = 0,
    };
}

static kbmic_step_t step_consumer(uint16_t usage)
{
    return (kbmic_step_t){
        .kind = KBMIC_STEP_CONSUMER,
        .mods = 0,
        .keycode = 0,
        .usage = usage,
        .delay_ms = 0,
    };
}

// 把若干步装配成一个 CLICK 动作。步数超上限时截断,多出来的静默丢掉。
static kbmic_action_t action_click(const kbmic_step_t *steps, uint8_t n)
{
    kbmic_action_t a = {0};
    a.trigger = KBMIC_TRIG_CLICK;
    a.step_count = (n > KBMIC_SEQ_MAX) ? KBMIC_SEQ_MAX : n;
    for (uint8_t i = 0; i < a.step_count; i++) {
        a.steps[i] = steps[i];
    }
    return a;
}

// TAP 动作:按下即发,保持到松手。语音输入走的就是它。
static kbmic_action_t action_tap(kbmic_step_t step)
{
    kbmic_action_t a = {0};
    a.trigger = KBMIC_TRIG_TAP;
    a.step_count = 1;
    a.steps[0] = step;
    return a;
}

// "进入设置"是个纯软件动作:不发任何 HID 报告,由 main 拦截。
// 放进配置里是为了让它可以被 MCP 改到别的键上,而不是硬编码"长按 OK"。
//
// step_count 必须是 1 而不是 0:kbmic_config_valid 与 kbmic_action_name 都先看
// step_count,写成 0 的话这个标记会被当成"空动作",界面上显示成 "-"。
static kbmic_action_t action_settings(void)
{
    kbmic_action_t a = {0};
    a.trigger = KBMIC_TRIG_NONE;
    a.step_count = 1;
    a.steps[0].kind = KBMIC_STEP_NONE;
    a.steps[0].mods = 0xF0;   // 高位借用:bit4 = 系统保留动作标记
    return a;
}

// OK 长按留空:说话就是"按住 OK"(TAP 触发,按多久说多久),而长按阈值 500ms
// 会在说话途中必然触发 —— 长按槽若再配设置,说话必被切断、设置突然弹出。
// "进设置"因此默认放在 长按下键(见上),两者不打架;用户可经 MCP/网页改回。
static kbmic_action_t action_none(void)
{
    return (kbmic_action_t){0};
}

// 四个内置模式的公共键位:上=回车(长按连发三次)、下=退格(长按进设置)、
// OK=语音(按平台不同,按住说话)。
static void fill_common_slots(kbmic_profile_t *p, kbmic_step_t voice_step)
{
    p->slots[KBMIC_BTN_UP][KBMIC_SLOT_TAP] =
        action_click((kbmic_step_t[]){step_key(0, KBMIC_HID_KEY_ENTER)}, 1);
    p->slots[KBMIC_BTN_UP][KBMIC_SLOT_LONG] = action_click(
        (kbmic_step_t[]){step_key(0, KBMIC_HID_KEY_ENTER), step_key(0, KBMIC_HID_KEY_ENTER),
                         step_key(0, KBMIC_HID_KEY_ENTER)},
        3);
    p->slots[KBMIC_BTN_DOWN][KBMIC_SLOT_TAP] =
        action_click((kbmic_step_t[]){step_key(0, KBMIC_HID_KEY_BACKSPACE)}, 1);
    p->slots[KBMIC_BTN_DOWN][KBMIC_SLOT_LONG] = action_settings();
    p->slots[KBMIC_BTN_OK][KBMIC_SLOT_TAP] = action_tap(voice_step);
    p->slots[KBMIC_BTN_OK][KBMIC_SLOT_LONG] = action_none();
}

// ---------------------------------------------------------------------------
// 出厂默认
// ---------------------------------------------------------------------------
void kbmic_config_defaults(kbmic_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->version = KBMIC_CONFIG_VERSION;
    cfg->active = 0;
    cfg->count = KBMIC_BUILTIN_MODES;

    // --- Mac:微信电脑版按住 Fn 触发语音 ---
    strcpy(cfg->profiles[0].name, "Mac");
    cfg->profiles[0].builtin = 1;
    fill_common_slots(&cfg->profiles[0], step_consumer(KBMIC_HID_USAGE_GLOBE));

    // Mac 键位(用户定稿 2026-10-06,与通用键位不同):
    //   长按上键 = 按住 Fn/Globe 说话 —— 按住=Fn 按下,松开=Fn 抬起;
    //   短按下键 = 回车;短按 OK = 退格;长按 OK = 进设置。
    //   上键短按保留回车、下键长按保留进设置(用户未指定,沿用通用值)。
    //   注意说话在上键、设置在 OK 长按:OK 短按是 CLICK 触发的退格,
    //   按住超 500ms 走长按进设置,两种意图天然分开,不冲突。
    cfg->profiles[0].slots[KBMIC_BTN_UP][KBMIC_SLOT_LONG] =
        action_tap(step_consumer(KBMIC_HID_USAGE_GLOBE));
    cfg->profiles[0].slots[KBMIC_BTN_DOWN][KBMIC_SLOT_TAP] =
        action_click((kbmic_step_t[]){step_key(0, KBMIC_HID_KEY_ENTER)}, 1);
    cfg->profiles[0].slots[KBMIC_BTN_OK][KBMIC_SLOT_TAP] =
        action_click((kbmic_step_t[]){step_key(0, KBMIC_HID_KEY_BACKSPACE)}, 1);
    cfg->profiles[0].slots[KBMIC_BTN_OK][KBMIC_SLOT_LONG] = action_settings();

    // --- Windows:微信电脑版按住 Ctrl+Win ---
    strcpy(cfg->profiles[1].name, "Windows");
    cfg->profiles[1].builtin = 1;
    fill_common_slots(&cfg->profiles[1],
                      step_key(KBMIC_MOD_CTRL | KBMIC_MOD_GUI, 0));

    // --- Android:微信输入法长按空格 ---
    strcpy(cfg->profiles[2].name, "Android");
    cfg->profiles[2].builtin = 1;
    fill_common_slots(&cfg->profiles[2], step_key(0, KBMIC_HID_KEY_SPACE));

    // --- iOS:先按 Globe 试 ---
    // ⚠️ 未经实机验证。iOS 没有公开的第三方硬件键全局语音输入接口,
    // 这里沿用 Globe(与 Mac 相同报文)作为尝试值,并保持完全可改。
    // 实测不通就直接用 MCP 把这一档改成别的组合键,不必动固件。
    strcpy(cfg->profiles[3].name, "iOS");
    cfg->profiles[3].builtin = 1;
    fill_common_slots(&cfg->profiles[3], step_consumer(KBMIC_HID_USAGE_GLOBE));
}

// ---------------------------------------------------------------------------
// 合法性
// ---------------------------------------------------------------------------
static bool name_ok(const char *name)
{
    const size_t n = strnlen(name, KBMIC_NAME_MAX);
    return n > 0 && n < KBMIC_NAME_MAX;   // 0 长度或没有结尾 '\0' 都算非法
}

static bool action_ok(const kbmic_action_t *a)
{
    if (a->trigger > KBMIC_TRIG_LONG) {
        return false;
    }
    if (a->step_count > KBMIC_SEQ_MAX) {
        return false;
    }
    for (uint8_t i = 0; i < a->step_count; i++) {
        if (a->steps[i].kind > KBMIC_STEP_DELAY) {
            return false;
        }
    }
    return true;
}

bool kbmic_config_valid(const kbmic_config_t *cfg)
{
    if (cfg == NULL) {
        return false;
    }
    if (cfg->version != KBMIC_CONFIG_VERSION) {
        return false;
    }
    if (cfg->count == 0 || cfg->count > KBMIC_MAX_PROFILES) {
        return false;
    }
    if (cfg->active >= cfg->count) {
        return false;
    }
    for (uint8_t i = 0; i < cfg->count; i++) {
        if (!name_ok(cfg->profiles[i].name)) {
            return false;
        }
        // 前 count 个里的前 4 个必须标记为内置,否则 MCP 删掉模式之后
        // "恢复默认"会失去锚点,界面上的模式列表也会错位。
        if (i < KBMIC_BUILTIN_MODES && cfg->profiles[i].builtin != 1) {
            return false;
        }
        for (int b = 0; b < KBMIC_BTN_COUNT; b++) {
            for (int s = 0; s < KBMIC_SLOT_COUNT; s++) {
                if (!action_ok(&cfg->profiles[i].slots[b][s])) {
                    return false;
                }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 增删改
// ---------------------------------------------------------------------------
int kbmic_config_add_profile(kbmic_config_t *cfg, const char *name)
{
    if (cfg == NULL || cfg->count >= KBMIC_MAX_PROFILES) {
        return -1;
    }
    const uint8_t idx = cfg->count;
    kbmic_profile_t *p = &cfg->profiles[idx];
    memset(p, 0, sizeof(*p));
    p->builtin = 0;
    // strncpy 不补 '\0',名字超长时必须手动收尾,否则 name_ok 会判非法。
    strncpy(p->name, (name && *name) ? name : "自定义", KBMIC_NAME_MAX - 1);
    p->name[KBMIC_NAME_MAX - 1] = '\0';

    // 新模式不是空壳:三个键都从当前模式拷一份,用户改起来才有起点。
    const kbmic_profile_t *src = &cfg->profiles[cfg->active];
    for (int b = 0; b < KBMIC_BTN_COUNT; b++) {
        for (int s = 0; s < KBMIC_SLOT_COUNT; s++) {
            p->slots[b][s] = src->slots[b][s];
        }
    }
    cfg->count = idx + 1;
    return idx;
}

int kbmic_config_delete_profile(kbmic_config_t *cfg, uint8_t index)
{
    if (cfg == NULL || index >= cfg->count || cfg->profiles[index].builtin) {
        return -1;
    }
    // 往前挪一格。被删的是 active 的话,active 跟着退到前一个。
    for (uint8_t i = index; i + 1 < cfg->count; i++) {
        cfg->profiles[i] = cfg->profiles[i + 1];
    }
    cfg->count--;
    if (cfg->active >= cfg->count) {
        cfg->active = cfg->count - 1;
    } else if (cfg->active > index) {
        cfg->active--;
    }
    memset(&cfg->profiles[cfg->count], 0, sizeof(cfg->profiles[0]));
    return 0;
}

int kbmic_config_reset_profile(kbmic_config_t *cfg, uint8_t index)
{
    if (cfg == NULL || index >= cfg->count || index >= KBMIC_BUILTIN_MODES) {
        return -1;
    }
    kbmic_config_t def;
    kbmic_config_defaults(&def);
    cfg->profiles[index] = def.profiles[index];
    return 0;
}

// ---------------------------------------------------------------------------
// 由步骤反推显示名
//
// 动作名不落盘:8 个模式 × 6 个槽如果都存字符串,每次改键都要同步两处,迟早
// 对不上。改成从步骤即时拼 ASCII 名,数据只有一个真相来源。中文模式名仍然存
// 字符串 —— 那是用户输入,没法反推。
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t code;
    const char *name;
} keyname_t;

// 只列真正会被用到的键。表外的键码走 "0x%02X" 兜底,不会显示成空。
static const keyname_t s_keynames[] = {
    {0x04, "A"}, {0x05, "B"}, {0x06, "C"}, {0x07, "D"}, {0x08, "E"},
    {0x09, "F"}, {0x0A, "G"}, {0x0B, "H"}, {0x0C, "I"}, {0x0D, "J"},
    {0x0E, "K"}, {0x0F, "L"}, {0x10, "M"}, {0x11, "N"}, {0x12, "O"},
    {0x13, "P"}, {0x14, "Q"}, {0x15, "R"}, {0x16, "S"}, {0x17, "T"},
    {0x18, "U"}, {0x19, "V"}, {0x1A, "W"}, {0x1B, "X"}, {0x1C, "Y"},
    {0x1D, "Z"},
    {0x1E, "1"}, {0x1F, "2"}, {0x20, "3"}, {0x21, "4"}, {0x22, "5"},
    {0x23, "6"}, {0x24, "7"}, {0x25, "8"}, {0x26, "9"}, {0x27, "0"},
    {0x28, "Enter"}, {0x29, "Esc"}, {0x2A, "Back"}, {0x2B, "Tab"},
    {0x2C, "Space"}, {0x2D, "Minus"}, {0x2E, "Equal"},
    {0x3A, "F1"}, {0x3B, "F2"}, {0x3C, "F3"}, {0x3D, "F4"},
    {0x3E, "F5"}, {0x3F, "F6"}, {0x40, "F7"}, {0x41, "F8"},
    {0x42, "F9"}, {0x43, "F10"}, {0x44, "F11"}, {0x45, "F12"},
    {0x49, "Insert"}, {0x4A, "Home"}, {0x4B, "PgUp"}, {0x4C, "Del"},
    {0x4D, "End"}, {0x4E, "PgDn"}, {0x4F, "Right"}, {0x50, "Left"},
    {0x51, "Down"}, {0x52, "Up"},
};

static const char *key_name(uint8_t keycode)
{
    for (size_t i = 0; i < sizeof(s_keynames) / sizeof(s_keynames[0]); i++) {
        if (s_keynames[i].code == keycode) {
            return s_keynames[i].name;
        }
    }
    return NULL;
}

// 追加修饰键,每个后面都跟一个 '+'。纯修饰键组合(没有主键可跟)时由调用方
// 剪掉**最后那一个** '+' —— Ctrl+Win+ 剪成 Ctrl+Win。
//
// 注意别写成"按名字逐个判断该剪哪一段":那样 Ctrl+Win 只会剪掉 Win 剩下的
// "Ctrl+"。这里剪的是字符串末尾的单个字符,和修饰键有几个无关。
static void append_mods(char *buf, size_t cap, size_t *len, uint8_t mods)
{
    if (mods & KBMIC_MOD_CTRL) {
        *len += snprintf(buf + *len, cap - *len, "Ctrl+");
    }
    if (mods & KBMIC_MOD_SHIFT) {
        *len += snprintf(buf + *len, cap - *len, "Shift+");
    }
    if (mods & KBMIC_MOD_ALT) {
        *len += snprintf(buf + *len, cap - *len, "Alt+");
    }
    if (mods & KBMIC_MOD_GUI) {
        *len += snprintf(buf + *len, cap - *len, "Win+");
    }
}

char *kbmic_action_name(const kbmic_action_t *a, char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return buf;
    }
    buf[0] = '\0';
    if (a == NULL) {
        snprintf(buf, cap, "-");
        return buf;
    }

    size_t len = 0;
    const bool is_settings = (a->step_count > 0 && a->steps[0].kind == KBMIC_STEP_NONE &&
                              (a->steps[0].mods & 0xF0) != 0);

    if (is_settings) {
        snprintf(buf, cap, "Settings");
        return buf;
    }
    if (a->trigger == KBMIC_TRIG_NONE || a->step_count == 0) {
        snprintf(buf, cap, "-");
        return buf;
    }

    for (uint8_t i = 0; i < a->step_count && len + 1 < cap; i++) {
        const kbmic_step_t *s = &a->steps[i];
        if (i > 0 && len + 1 < cap) {
            buf[len++] = ' ';
        }
        switch (s->kind) {
        case KBMIC_STEP_KEY: {
            // keycode 为 0 表示这一次只按修饰键,没有主键可跟,
            // 这时把末尾那个 '+' 剪掉:Ctrl+Win 而不是 Ctrl+Win+ / Ctrl+。
            const bool mods_only = (s->keycode == 0);
            append_mods(buf, cap, &len, s->mods);
            if (mods_only && len > 0) {
                buf[len - 1] = '\0';
                len--;
            }
            if (len + 1 >= cap) {
                break;
            }
            const char *kn = key_name(s->keycode);
            if (kn) {
                len += snprintf(buf + len, cap - len, "%s", kn);
            } else if (!mods_only) {
                // 表外的键码:退回十六进制,别显示成空白
                len += snprintf(buf + len, cap - len, "0x%02X", s->keycode);
            }
            break;
        }
        case KBMIC_STEP_CONSUMER:
            if (s->usage == 0x029D) {
                len += snprintf(buf + len, cap - len, "Globe");
            } else {
                len += snprintf(buf + len, cap - len, "C:0x%04X", s->usage);
            }
            break;
        case KBMIC_STEP_DELAY:
            len += snprintf(buf + len, cap - len, "+%ums", s->delay_ms);
            break;
        default:
            break;
        }
    }

    if (len == 0) {
        snprintf(buf, cap, "-");
        return buf;
    }
    // TAP 类(按住)加个后缀,界面上和"点一下"一眼能分开。
    if (a->trigger == KBMIC_TRIG_TAP && len + 8 < cap) {
        len += snprintf(buf + len, cap - len, " (hold)");
    }
    return buf;
}
