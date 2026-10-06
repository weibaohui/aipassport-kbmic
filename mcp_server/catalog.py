"""内置动作目录 + 出厂默认配置。

两者都必须与固件逐字对齐:

* 目录顺序 / id  : main/kbmic_action.c 的 s_catalog(ACT_* 枚举)
* 出厂默认 4 模式: main/kbmic_config.c 的 kbmic_config_defaults()

固件是唯一真相来源,这里只是把同样的常量用 Python 再写一遍。改动任意一侧都
必须同步改另一侧,否则 MCP 显示的名字会和设备实际执行的行为对不上。
"""

from __future__ import annotations

from dataclasses import dataclass

import protocol as P
from protocol import (
    Action,
    Config,
    Profile,
    ProtocolError,
    Step,
    MOD_ALT,
    MOD_CTRL,
    MOD_FLAG,
    MOD_GUI,
    MOD_SHIFT,
    STEP_APPLEFN,
    STEP_CONSUMER,
    STEP_DELAY,
    STEP_KEY,
    STEP_NONE,
    TRIGGER_CLICK,
    TRIGGER_NONE,
    TRIGGER_TAP,
    USAGE_GLOBE,
)

# ---------------------------------------------------------------------------
# 步 / 动作的构造辅助(与固件的 step_key / action_click 等同名函数对应)
# ---------------------------------------------------------------------------

def step_key(keycode: int, mods: int = 0) -> Step:
    return Step(kind=STEP_KEY, mods=mods, keycode=keycode)


def step_consumer(usage: int) -> Step:
    return Step(kind=STEP_CONSUMER, usage=usage)


def step_delay(ms: int) -> Step:
    return Step(kind=STEP_DELAY, delay_ms=ms)


def step_settings() -> Step:
    """Settings 软件动作:不发 HID 报告,main 拦截后切到机身菜单。"""
    return Step(kind=STEP_NONE, mods=MOD_FLAG)

def step_applefn() -> Step:
    """Apple Fn 步:键盘报告 AppleVendor Top Case 字节(见固件 KBMIC_STEP_APPLEFN)。"""
    return Step(kind=STEP_APPLEFN)



def action_click(*steps: Step) -> Action:
    return Action(trigger=TRIGGER_CLICK, steps=list(steps)[: P.SEQ_MAX])


def action_tap(*steps: Step) -> Action:
    return Action(trigger=TRIGGER_TAP, steps=list(steps)[: P.SEQ_MAX])


def action_none() -> Action:
    return Action(trigger=TRIGGER_NONE, steps=[])


def action_settings() -> Action:
    # step_count 必须是 1 而不是 0:固件先看 step_count,写成 0 会被当成空动作。
    return Action(trigger=TRIGGER_NONE, steps=[step_settings()])


# HID 用法码(固件 kbmic_hid.h / s_keynames 同一套)
KEY_ENTER = 0x28
KEY_ESCAPE = 0x29
KEY_BACKSPACE = 0x2A
KEY_TAB = 0x2B
KEY_SPACE = 0x2C
KEY_RIGHT = 0x4F
KEY_LEFT = 0x50
KEY_DOWN = 0x51
KEY_UP = 0x52
KEY_F1 = 0x3A
KEY_F2 = 0x3B
MOD_CTRL_GUI = MOD_CTRL | MOD_GUI

# ---------------------------------------------------------------------------
# 动作目录:id 与固件 ACT_* 枚举严格同序,不要重排
# ---------------------------------------------------------------------------

@dataclass
class CatalogItem:
    id: int
    name: str
    action: Action
    note: str = ""

    def to_dict(self) -> dict:
        return {
            "id": self.id,
            "name": self.name,
            "note": self.note,
            "trigger": self.action.trigger,
            "trigger_name": P.TRIGGER_NAMES.get(self.action.trigger, "?"),
            "step_count": self.action.step_count,
            "steps": [s.to_dict() for s in self.action.steps],
        }


CATALOG: list[CatalogItem] = [
    CatalogItem(0, "-", action_none(), "空动作,该槽不响应"),
    CatalogItem(1, "Enter", action_click(step_key(KEY_ENTER))),
    CatalogItem(2, "Back", action_click(step_key(KEY_BACKSPACE)), "退格"),
    CatalogItem(3, "Tab", action_click(step_key(KEY_TAB))),
    CatalogItem(4, "Space", action_click(step_key(KEY_SPACE))),
    CatalogItem(5, "Esc", action_click(step_key(KEY_ESCAPE))),
    CatalogItem(6, "Globe", action_tap(step_consumer(USAGE_GLOBE)),
                "按住触发系统语音/地球仪,微信语音输入用"),
    CatalogItem(7, "Ctrl+Win", action_tap(step_key(0, MOD_CTRL_GUI)),
                "Windows 版微信语音输入"),
    CatalogItem(8, "Up", action_click(step_key(KEY_UP))),
    CatalogItem(9, "Down", action_click(step_key(KEY_DOWN))),
    CatalogItem(10, "Left", action_click(step_key(KEY_LEFT))),
    CatalogItem(11, "Right", action_click(step_key(KEY_RIGHT))),
    CatalogItem(12, "Enter x3", action_click(
        step_key(KEY_ENTER), step_key(KEY_ENTER), step_key(KEY_ENTER))),
    CatalogItem(13, "F1", action_click(step_key(KEY_F1))),
    CatalogItem(14, "F2", action_click(step_key(KEY_F2))),
    CatalogItem(15, "Settings", action_settings(), "软件动作:打开机身菜单,不发 HID 报告"),
    CatalogItem(16, "Apple Fn (hold)", action_tap(step_applefn()), "按住=Fn 按下,松开=Fn 抬起;macOS 的 Fn/Globe 键(Mac 模式说话用)"),
]

CATALOG_BY_ID = {item.id: item for item in CATALOG}
CATALOG_BY_NAME = {item.name.lower(): item for item in CATALOG}

# 目录里暂时没收录、但常用的修饰键组合,供 set_key 的 mods 字符串写法使用
ALL_MODS = MOD_CTRL | MOD_SHIFT | MOD_ALT | MOD_GUI


def lookup(spec) -> CatalogItem:
    """按 id 或名字取目录项。名字大小写不敏感,也接受 "-" / "None"。"""
    if isinstance(spec, CatalogItem):
        return spec
    if isinstance(spec, bool):
        raise ProtocolError("动作预设必须是 id(0..15)或名字,例如 1 或 'Enter'")
    if isinstance(spec, int):
        if spec in CATALOG_BY_ID:
            return CATALOG_BY_ID[spec]
        raise ProtocolError(
            f"动作 id {spec} 不在目录内(0..{len(CATALOG) - 1});用 kbmic_action_catalog 看全部"
        )
    key = str(spec).strip().lower()
    if key in CATALOG_BY_NAME:
        return CATALOG_BY_NAME[key]
    if key.lstrip("-").isdigit() and int(key) in CATALOG_BY_ID:
        return CATALOG_BY_ID[int(key)]
    raise ProtocolError(
        f"动作名 {spec!r} 不在目录内;可用: {', '.join(i.name for i in CATALOG)}"
    )


def catalog_as_dicts() -> list[dict]:
    return [item.to_dict() for item in CATALOG]


def match_catalog(action: Action) -> int | None:
    """反向查:一段动作是否正好是某个目录项,返回 id(或 None)。用于紧凑视图。"""
    want = Action(trigger=action.trigger,
                  steps=[Step(s.kind, s.mods, s.keycode, s.usage, s.delay_ms)
                         for s in action.steps])
    for item in CATALOG:
        have = item.action
        if have.trigger != want.trigger or have.step_count != want.step_count:
            continue
        if all(
            a.kind == b.kind and a.mods == b.mods and a.keycode == b.keycode
            and a.usage == b.usage and a.delay_ms == b.delay_ms
            for a, b in zip(have.steps, want.steps)
        ):
            return item.id
    return None


# ---------------------------------------------------------------------------
# 出厂默认配置 —— 镜像固件 kbmic_config_defaults()
#
# 四个内置模式,count=4,active=0(Mac)。字节级一致,这样 MCP 写回去的"恢复默认"
# 和固件自己重建出来的结果完全相同。
# ---------------------------------------------------------------------------

def _enter_triple() -> Action:
    return action_click(step_key(KEY_ENTER), step_key(KEY_ENTER), step_key(KEY_ENTER))


def _default_builtin_profiles() -> list[Profile]:
    def blank() -> list[list[Action]]:
        return [[Action() for _ in range(P.SLOT_COUNT)] for _ in range(P.BTN_COUNT)]

    def fill(prof: Profile, ok_tap: Action) -> Profile:
        # 三个内置模式的 Up/Down 完全一样,只有 OK 的按住动作不同。
        prof.set_slot(P.BTN_UP, P.SLOT_TAP, action_click(step_key(KEY_ENTER)))
        prof.set_slot(P.BTN_UP, P.SLOT_DOUBLE, action_none())
        prof.set_slot(P.BTN_UP, P.SLOT_LONG, _enter_triple())
        prof.set_slot(P.BTN_DOWN, P.SLOT_TAP, action_click(step_key(KEY_BACKSPACE)))
        prof.set_slot(P.BTN_DOWN, P.SLOT_DOUBLE, action_none())
        prof.set_slot(P.BTN_DOWN, P.SLOT_LONG, action_settings())
        prof.set_slot(P.BTN_OK, P.SLOT_TAP, ok_tap)
        prof.set_slot(P.BTN_OK, P.SLOT_DOUBLE, action_none())
        # OK 长按留空(Mac 除外,见下):说话=按住某键,长按阈值必然落在
        # 按住途中,长按槽再配 HID 动作会被松手切断。
        prof.set_slot(P.BTN_OK, P.SLOT_LONG, action_none())
        return prof

    mac = fill(Profile(name="Mac", builtin=1, slots=blank()),
               action_tap(step_consumer(USAGE_GLOBE)))
    # Mac 键位(用户定稿 2026-10-06 二改):
    #   长按上键 = 按住 Fn/Globe 说话(按住=按下,松开=抬起);短按上键=无;
    #   短按下键 = 回车;双击下键 = Esc;长按下键 = 无;
    #   短按 OK = 退格;长按 OK = 进设置。
    mac.set_slot(P.BTN_UP, P.SLOT_TAP, action_none())
    mac.set_slot(P.BTN_UP, P.SLOT_LONG, action_tap(step_consumer(USAGE_GLOBE)))
    mac.set_slot(P.BTN_DOWN, P.SLOT_TAP, action_click(step_key(KEY_ENTER)))
    mac.set_slot(P.BTN_DOWN, P.SLOT_DOUBLE,
                 action_click(step_key(0x29)))      # Esc = 0x29
    mac.set_slot(P.BTN_DOWN, P.SLOT_LONG, action_none())
    mac.set_slot(P.BTN_OK, P.SLOT_TAP, action_click(step_key(KEY_BACKSPACE)))
    mac.set_slot(P.BTN_OK, P.SLOT_LONG, action_settings())
    windows = fill(Profile(name="Windows", builtin=1, slots=blank()),
                   action_tap(step_key(0, MOD_CTRL_GUI)))
    android = fill(Profile(name="Android", builtin=1, slots=blank()),
                   action_tap(step_key(KEY_SPACE)))
    # iOS 与 Mac 相同(发 Globe)。固件注释已标明:未经实机验证,不通就直接用 MCP 改。
    ios = fill(Profile(name="iOS", builtin=1, slots=blank()),
               action_tap(step_consumer(USAGE_GLOBE)))
    return [mac, windows, android, ios]


def builtin_profile_defaults(index: int) -> Profile:
    """单个内置模式的出厂默认(供 kbmic_reset_mode 用)。"""
    profiles = _default_builtin_profiles()
    if not 0 <= index < P.BUILTIN_MODES:
        raise ProtocolError(
            f"只有前 {P.BUILTIN_MODES} 个内置模式有出厂默认(0..{P.BUILTIN_MODES - 1});"
            f"用户模式 {index} 没有出厂默认,请用 kbmic_delete_mode 删掉后重建"
        )
    return profiles[index]


def factory_default_config() -> Config:
    """整份出厂默认:4 个内置模式,active=0。必须正好 1580 字节。"""
    profiles = _default_builtin_profiles()
    cfg = Config(version=P.CONFIG_VERSION, active=0,
                 count=P.BUILTIN_MODES, reserved=0, profiles=profiles)
    cfg.validate()
    assert len(cfg.encode()) == P.CONFIG_SIZE
    return cfg


def self_check() -> dict:
    """目录与出厂默认的一致性自检(protocol.self_check() 会顺带调用)。"""
    assert [item.id for item in CATALOG] == list(range(len(CATALOG))), "目录 id 必须连续"
    assert len(CATALOG) == 16, "固件 s_catalog 有 16 项"
    for item in CATALOG:
        item.action.validate()
    cfg = factory_default_config()
    assert cfg.count == P.BUILTIN_MODES and cfg.active == 0
    assert [p.name for p in cfg.live_profiles()] == ["Mac", "Windows", "Android", "iOS"]
    assert P.action_name(cfg.profiles[0].get_slot(P.BTN_OK, P.SLOT_TAP)) == "Globe (hold)"
    assert P.action_name(cfg.profiles[1].get_slot(P.BTN_OK, P.SLOT_TAP)) == "Ctrl+Win (hold)"
    assert P.action_name(cfg.profiles[0].get_slot(P.BTN_UP, P.SLOT_LONG)) == "Enter Enter Enter"
    assert P.action_name(cfg.profiles[0].get_slot(P.BTN_DOWN, P.SLOT_LONG)) == "Settings"
    return {
        "catalog_items": len(CATALOG),
        "default_bytes": len(cfg.encode()),
        "builtin_profiles": [p.name for p in cfg.live_profiles()],
    }
