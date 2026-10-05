// tests/test_kbmic_model.c —— 配置模型纯逻辑的主机测试。
// 门禁自动把 tests/test_kbmic_model.c 与 main/kbmic_model.c 一起编译:
//   cc -std=c11 -Wall -Wextra -Werror -Imain -Itests -- test_kbmic_model.c main/kbmic_model.c
//
// 为什么必须有这个测试:mcp_server/catalog.py 里有一份**同样**的动作名渲染实现,
// 两边算出来的字符串必须逐字一致,否则同一件事在设备屏幕上和 MCP 返回值里
// 显示成不同的名字。纯 C 那边不跑主机测试,这种分叉会一直存在到有人肉眼发现。
#include "kbmic_config.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            failures++;                                                 \
        }                                                               \
    } while (0)

#define CHECK_STR(actual, expect)                                            \
    do {                                                                     \
        if (strcmp((actual), (expect)) != 0) {                               \
            printf("FAIL %s:%d: 得到 \"%s\",期望 \"%s\"\n", __FILE__,      \
                   __LINE__, (actual), (expect));                            \
            failures++;                                                      \
        }                                                                    \
    } while (0)

// 便捷构造
static kbmic_step_t sk(uint8_t mods, uint8_t keycode)
{
    return (kbmic_step_t){.kind = KBMIC_STEP_KEY, .mods = mods, .keycode = keycode};
}

static kbmic_step_t sc(uint16_t usage)
{
    return (kbmic_step_t){.kind = KBMIC_STEP_CONSUMER, .usage = usage};
}

static kbmic_action_t mk(kbmic_trigger_t trig, uint8_t n, const kbmic_step_t *s)
{
    kbmic_action_t a = {0};
    a.trigger = (uint8_t)trig;
    a.step_count = n;
    for (uint8_t i = 0; i < n && i < KBMIC_SEQ_MAX; i++) {
        a.steps[i] = s[i];
    }
    return a;
}

static void name_of(kbmic_action_t a, char *out, size_t cap)
{
    kbmic_action_name(&a, out, cap);
}

// ---------------------------------------------------------------------------
// 动作名渲染 —— 与 mcp_server/catalog.py 的 action_name() 一一对应
// ---------------------------------------------------------------------------
static void test_action_names(void)
{
    char buf[KBMIC_ACTION_NAME_MAX];

    // 纯修饰键组合。曾经的 bug:先拼 "Ctrl+Win+" 再剪尾部 '+',只能剪掉一层,
    // 结果显示成 "Ctrl+"。这是本测试最核心的一条。
    name_of(mk(KBMIC_TRIG_TAP, 1, (kbmic_step_t[]){sk(KBMIC_MOD_CTRL | KBMIC_MOD_GUI, 0)}), buf,
            sizeof(buf));
    CHECK_STR(buf, "Ctrl+Win (hold)");

    // 带主键的组合:修饰键要带 '+'。
    name_of(mk(KBMIC_TRIG_CLICK, 1, (kbmic_step_t[]){sk(KBMIC_MOD_CTRL, 0x06)}), buf, sizeof(buf));
    CHECK_STR(buf, "Ctrl+C");

    // 四个修饰键全开,主键存在。
    name_of(mk(KBMIC_TRIG_CLICK, 1,
               (kbmic_step_t[]){sk(KBMIC_MOD_CTRL | KBMIC_MOD_SHIFT | KBMIC_MOD_ALT | KBMIC_MOD_GUI,
                                  0x1D)}),
            buf, sizeof(buf));
    CHECK_STR(buf, "Ctrl+Shift+Alt+Win+Z");

    // 单个修饰键、不带主键。
    name_of(mk(KBMIC_TRIG_CLICK, 1, (kbmic_step_t[]){sk(KBMIC_MOD_ALT, 0)}), buf, sizeof(buf));
    CHECK_STR(buf, "Alt");

    // Globe 走 Consumer 报告。
    name_of(mk(KBMIC_TRIG_TAP, 1, (kbmic_step_t[]){sc(0x029D)}), buf, sizeof(buf));
    CHECK_STR(buf, "Globe (hold)");

    // 其它 Consumer 用法没有名字,退回十六进制。
    name_of(mk(KBMIC_TRIG_CLICK, 1, (kbmic_step_t[]){sc(0x00CD)}), buf, sizeof(buf));
    CHECK_STR(buf, "C:0x00CD");

    // 多步序列用空格连接。
    name_of(mk(KBMIC_TRIG_CLICK, 3,
               (kbmic_step_t[]){sk(0, 0x28), sk(0, 0x28), sk(0, 0x28)}),
            buf, sizeof(buf));
    CHECK_STR(buf, "Enter Enter Enter");

    // 延时步。
    name_of(mk(KBMIC_TRIG_CLICK, 2,
               (kbmic_step_t[]){sk(0, 0x28), {.kind = KBMIC_STEP_DELAY, .delay_ms = 120}}),
            buf, sizeof(buf));
    CHECK_STR(buf, "Enter +120ms");

    // 表外的键码不显示成空白。
    name_of(mk(KBMIC_TRIG_CLICK, 1, (kbmic_step_t[]){sk(0, 0x77)}), buf, sizeof(buf));
    CHECK_STR(buf, "0x77");

    // 空动作与 NULL 都是 "-"。
    name_of((kbmic_action_t){0}, buf, sizeof(buf));
    CHECK_STR(buf, "-");
    kbmic_action_name(NULL, buf, sizeof(buf));
    CHECK_STR(buf, "-");

    // "进设置"是纯软件动作,即使 trigger=NONE 也要显示成 Settings 而不是 "-"。
    kbmic_action_t settings = {0};
    settings.trigger = KBMIC_TRIG_NONE;
    settings.step_count = 1;
    settings.steps[0].kind = KBMIC_STEP_NONE;
    settings.steps[0].mods = 0xF0;
    name_of(settings, buf, sizeof(buf));
    CHECK_STR(buf, "Settings");
}

// ---------------------------------------------------------------------------
// 出厂默认
// ---------------------------------------------------------------------------
static void test_defaults(void)
{
    kbmic_config_t cfg;
    kbmic_config_defaults(&cfg);

    CHECK(kbmic_config_valid(&cfg));
    CHECK(cfg.count == KBMIC_BUILTIN_MODES);
    CHECK(cfg.active == 0);
    CHECK_STR(cfg.profiles[0].name, "Mac");
    CHECK_STR(cfg.profiles[1].name, "Windows");
    CHECK_STR(cfg.profiles[2].name, "Android");
    CHECK_STR(cfg.profiles[3].name, "iOS");
    for (int i = 0; i < KBMIC_BUILTIN_MODES; i++) {
        CHECK(cfg.profiles[i].builtin == 1);
    }

    char buf[KBMIC_ACTION_NAME_MAX];
    // 四个平台的语音触发各不相同,这是这个设备存在的全部理由。
    name_of(cfg.profiles[0].slots[KBMIC_BTN_OK][KBMIC_SLOT_TAP], buf, sizeof(buf));
    CHECK_STR(buf, "Globe (hold)");
    name_of(cfg.profiles[1].slots[KBMIC_BTN_OK][KBMIC_SLOT_TAP], buf, sizeof(buf));
    CHECK_STR(buf, "Ctrl+Win (hold)");
    name_of(cfg.profiles[2].slots[KBMIC_BTN_OK][KBMIC_SLOT_TAP], buf, sizeof(buf));
    CHECK_STR(buf, "Space (hold)");

    // 默认进设置挂在 OK 的长按槽上 —— 长按 OK 才是进菜单的入口。
    name_of(cfg.profiles[0].slots[KBMIC_BTN_OK][KBMIC_SLOT_LONG], buf, sizeof(buf));
    CHECK_STR(buf, "Settings");

    // 上下两个键在四个平台上是共通的。
    name_of(cfg.profiles[0].slots[KBMIC_BTN_UP][KBMIC_SLOT_TAP], buf, sizeof(buf));
    CHECK_STR(buf, "Enter");
    name_of(cfg.profiles[0].slots[KBMIC_BTN_DOWN][KBMIC_SLOT_TAP], buf, sizeof(buf));
    CHECK_STR(buf, "Back");
    name_of(cfg.profiles[0].slots[KBMIC_BTN_UP][KBMIC_SLOT_LONG], buf, sizeof(buf));
    CHECK_STR(buf, "Enter Enter Enter");
}

// ---------------------------------------------------------------------------
// 合法性
// ---------------------------------------------------------------------------
static void test_validation(void)
{
    kbmic_config_t cfg;
    kbmic_config_defaults(&cfg);
    CHECK(kbmic_config_valid(&cfg));

    kbmic_config_t bad = cfg;
    bad.version = 99;
    CHECK(!kbmic_config_valid(&bad));

    bad = cfg;
    bad.count = 0;
    CHECK(!kbmic_config_valid(&bad));

    bad = cfg;
    bad.count = KBMIC_MAX_PROFILES + 1;
    CHECK(!kbmic_config_valid(&bad));

    bad = cfg;
    bad.active = bad.count;   // 越界
    CHECK(!kbmic_config_valid(&bad));

    bad = cfg;
    bad.profiles[0].name[0] = '\0';   // 空名字
    CHECK(!kbmic_config_valid(&bad));

    // 内置标志被抹掉:否则"恢复默认"失去锚点。
    bad = cfg;
    bad.profiles[2].builtin = 0;
    CHECK(!kbmic_config_valid(&bad));

    // 触发方式越界。
    bad = cfg;
    bad.profiles[0].slots[0][0].trigger = 9;
    CHECK(!kbmic_config_valid(&bad));

    // 步数越界。
    bad = cfg;
    bad.profiles[0].slots[0][0].step_count = KBMIC_SEQ_MAX + 1;
    CHECK(!kbmic_config_valid(&bad));

    // 步类型越界。
    bad = cfg;
    bad.profiles[0].slots[0][0].steps[0].kind = 7;
    CHECK(!kbmic_config_valid(&bad));

    CHECK(!kbmic_config_valid(NULL));
}

// ---------------------------------------------------------------------------
// 增删模式
// ---------------------------------------------------------------------------
static void test_add_delete(void)
{
    kbmic_config_t cfg;
    kbmic_config_defaults(&cfg);

    const int idx = kbmic_config_add_profile(&cfg, "游戏");
    CHECK(idx == KBMIC_BUILTIN_MODES);
    CHECK(cfg.count == KBMIC_BUILTIN_MODES + 1);
    CHECK(kbmic_config_valid(&cfg));
    CHECK_STR(cfg.profiles[idx].name, "游戏");
    CHECK(cfg.profiles[idx].builtin == 0);

    // 新模式从当前模式拷一份键位,不是空壳。
    CHECK(memcmp(cfg.profiles[idx].slots, cfg.profiles[cfg.active].slots,
                 sizeof(cfg.profiles[0].slots)) == 0);

    // 超长名字按 UTF-8 截断且必须留结尾 '\0'。
    const int idx2 = kbmic_config_add_profile(&cfg, "一二三四五六七八九十十一十二十三十四");
    CHECK(idx2 == KBMIC_BUILTIN_MODES + 1);
    CHECK(kbmic_config_valid(&cfg));
    CHECK(strlen(cfg.profiles[idx2].name) < KBMIC_NAME_MAX);

    // 内置模式不可删。
    CHECK(kbmic_config_delete_profile(&cfg, 0) == -1);
    CHECK(kbmic_config_delete_profile(&cfg, (uint8_t)cfg.count) == -1);
    CHECK(cfg.count == KBMIC_BUILTIN_MODES + 2);

    // 删掉当前选中项之后,active 要退到合法位置。
    cfg.active = (uint8_t)idx2;
    CHECK(kbmic_config_delete_profile(&cfg, (uint8_t)idx2) == 0);
    CHECK(cfg.active < cfg.count);
    CHECK(kbmic_config_valid(&cfg));

    CHECK(kbmic_config_delete_profile(&cfg, (uint8_t)idx) == 0);
    CHECK(cfg.count == KBMIC_BUILTIN_MODES);
    CHECK(kbmic_config_valid(&cfg));

    // 删到上限再试,应该拿不到新槽位。
    while (cfg.count < KBMIC_MAX_PROFILES) {
        CHECK(kbmic_config_add_profile(&cfg, "x") >= 0);
    }
    CHECK(kbmic_config_add_profile(&cfg, "y") == -1);
    CHECK(kbmic_config_valid(&cfg));
}

static void test_reset(void)
{
    kbmic_config_t cfg;
    kbmic_config_defaults(&cfg);

    // 改坏第 1 个模式,再恢复。
    cfg.profiles[1].slots[KBMIC_BTN_UP][KBMIC_SLOT_TAP] =
        mk(KBMIC_TRIG_CLICK, 1, (kbmic_step_t[]){sk(0, 0x1D)});
    CHECK(kbmic_config_reset_profile(&cfg, 1) == 0);

    char buf[KBMIC_ACTION_NAME_MAX];
    name_of(cfg.profiles[1].slots[KBMIC_BTN_UP][KBMIC_SLOT_TAP], buf, sizeof(buf));
    CHECK_STR(buf, "Enter");

    // 自定义模式没有出厂值可回。
    const int custom = kbmic_config_add_profile(&cfg, "会议");
    CHECK(custom >= 0);
    CHECK(kbmic_config_reset_profile(&cfg, (uint8_t)custom) == -1);
    CHECK(kbmic_config_reset_profile(&cfg, KBMIC_BUILTIN_MODES) == -1);
    CHECK(kbmic_config_reset_profile(&cfg, (uint8_t)cfg.count) == -1);
    CHECK(kbmic_config_valid(&cfg));
}

int main(void)
{
    test_action_names();
    test_defaults();
    test_validation();
    test_add_delete();
    test_reset();

    if (failures) {
        printf("%d 项断言失败\n", failures);
        return 1;
    }
    printf("kbmic_model: 全部通过\n");
    return 0;
}
