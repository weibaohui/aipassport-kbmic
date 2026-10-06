"""kbmic 配置线协议 —— 纯 Python 实现,不依赖任何 BLE 栈。

这一份布局是**固件与 MCP 之间的共同契约**,逐字节对齐 main/kbmic_config.h 里的
packed 结构体:

    kbmic_config_t  = 4 + 8 * 287 = 2300 字节
    kbmic_profile_t = 16 + 1 + 270 = 287 字节
    kbmic_action_t  = 1 + 1 + 4*7 = 30 字节
    kbmic_step_t    = 1 + 1 + 1 + 2 + 2 = 7 字节

所有字段小端、无对齐空洞(struct 模式串一律以 '<' 开头,packed 因此是显式的,
不依赖平台 struct 布局)。改任何偏移都必须同步固件并 bump CONFIG_VERSION。

本模块只做纯逻辑:编码、解码、校验、显示名、分片。任何文件都没有 import bleak,
所以单元测试可以在没有硬件、没有蓝牙权限的机器上跑。
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Iterable

# ---------------------------------------------------------------------------
# 常量:与固件 kbmic_config.h 一一对应
# ---------------------------------------------------------------------------

CONFIG_VERSION = 3
MAX_PROFILES = 8
BUILTIN_MODES = 4
NAME_MAX = 16  # 含结尾 '\0',即最多 15 字节内容
NAME_CONTENT_MAX = NAME_MAX - 1
SEQ_MAX = 4
BTN_COUNT = 3
SLOT_COUNT = 3

STEP_SIZE = 7
ACTION_SIZE = 30
PROFILE_SIZE = 287
HEADER_SIZE = 4
CONFIG_SIZE = 2300  # 4 + 8 * 287

# BLE 分片:main/kbmic_ble_svc.h 的 KBMIC_SVC_CHUNK_SIZE
CHUNK_SIZE = 160
CHUNK_COUNT = 15  # ceil(2300 / 160);第 15 片只有 60 字节

# GATT UUID(与 main/kbmic_ble_svc.c 的 UUID128_LO 字面量对应)
_UUID_TAIL = "-9f6e-4a21-8c3d-2b5e7a9f1c48"
SERVICE_UUID = "7d1c5a30" + _UUID_TAIL
EVENT_UUID = "7d1c5a4f" + _UUID_TAIL
DEVICE_NAME = "AI小键盘"  # 系统蓝牙列表里看到的名字,11 字节 UTF-8


def chunk_uuid(index: int) -> str:
    """分片 i 的特征 UUID(固件从 0x40 起递增)。"""
    if not 0 <= index < CHUNK_COUNT:
        raise ProtocolError(f"分片索引越界: {index} (有效范围 0..{CHUNK_COUNT - 1})")
    return f"7d1c5a{0x40 + index:02x}" + _UUID_TAIL


def chunk_expected_len(index: int) -> int:
    """分片 i 的字节数:0..8 是 160,第 9 片是 140。"""
    if not 0 <= index < CHUNK_COUNT:
        raise ProtocolError(f"分片索引越界: {index} (有效范围 0..{CHUNK_COUNT - 1})")
    left = CONFIG_SIZE - index * CHUNK_SIZE
    return CHUNK_SIZE if left >= CHUNK_SIZE else left


# ---------------------------------------------------------------------------
# 枚举
# ---------------------------------------------------------------------------

TRIGGER_NONE = 0
TRIGGER_CLICK = 1
TRIGGER_TAP = 2
TRIGGER_LONG = 3
TRIGGER_DOUBLE = 4
TRIGGERS = (TRIGGER_NONE, TRIGGER_CLICK, TRIGGER_TAP, TRIGGER_LONG, TRIGGER_DOUBLE)
TRIGGER_NAMES = {0: "none", 1: "click", 2: "tap", 3: "long", 4: "double"}

STEP_NONE = 0
STEP_KEY = 1
STEP_CONSUMER = 2
STEP_DELAY = 3
STEP_APPLEFN = 4
STEP_KINDS = (STEP_NONE, STEP_KEY, STEP_CONSUMER, STEP_DELAY, STEP_APPLEFN)
STEP_KIND_NAMES = {0: "none", 1: "key", 2: "consumer", 3: "delay", 4: "applefn"}
STEP_KIND_VALUES = {name: value for value, name in STEP_KIND_NAMES.items()}

MOD_CTRL = 0x01
MOD_SHIFT = 0x02
MOD_ALT = 0x04
MOD_GUI = 0x08  # Win / Cmd
MOD_FLAG = 0xF0  # 高位借用:bit4 = 软件保留动作(打开机身菜单)

BTN_UP = 0
BTN_DOWN = 1
BTN_OK = 2
BUTTON_NAMES = {0: "Up", 1: "Down", 2: "OK"}
BUTTON_ALIASES = {
    "up": BTN_UP, "上": BTN_UP, "0": BTN_UP,
    "down": BTN_DOWN, "下": BTN_DOWN, "1": BTN_DOWN,
    "ok": BTN_OK, "confirm": BTN_OK, "确认": BTN_OK, "2": BTN_OK,
}

SLOT_TAP = 0
SLOT_DOUBLE = 1
SLOT_LONG = 2
SLOT_NAMES = {0: "short-tap", 1: "double-click", 2: "long-press"}
SLOT_ALIASES = {
    "tap": SLOT_TAP, "short": SLOT_TAP, "short-tap": SLOT_TAP, "短按": SLOT_TAP, "0": SLOT_TAP,
    "double": SLOT_DOUBLE, "double-click": SLOT_DOUBLE, "双击": SLOT_DOUBLE, "1": SLOT_DOUBLE,
    "long": SLOT_LONG, "hold": SLOT_LONG, "long-press": SLOT_LONG, "长按": SLOT_LONG, "2": SLOT_LONG,
}

EV_BOOT = 0
EV_CONFIG_SAVED = 1
EV_KEY = 2
EV_TYPES = {0: "BOOT", 1: "CONFIG_SAVED", 2: "KEY"}

# 只列真正会用到的键码,表外走 "0x%02X" 兜底(与固件 s_keynames 同表)
KEY_NAMES = {
    0x04: "A", 0x05: "B", 0x06: "C", 0x07: "D", 0x08: "E", 0x09: "F",
    0x0A: "G", 0x0B: "H", 0x0C: "I", 0x0D: "J", 0x0E: "K", 0x0F: "L",
    0x10: "M", 0x11: "N", 0x12: "O", 0x13: "P", 0x14: "Q", 0x15: "R",
    0x16: "S", 0x17: "T", 0x18: "U", 0x19: "V", 0x1A: "W", 0x1B: "X",
    0x1C: "Y", 0x1D: "Z",
    0x1E: "1", 0x1F: "2", 0x20: "3", 0x21: "4", 0x22: "5",
    0x23: "6", 0x24: "7", 0x25: "8", 0x26: "9", 0x27: "0",
    0x28: "Enter", 0x29: "Esc", 0x2A: "Back", 0x2B: "Tab",
    0x2C: "Space", 0x2D: "Minus", 0x2E: "Equal",
    0x3A: "F1", 0x3B: "F2", 0x3C: "F3", 0x3D: "F4", 0x3E: "F5", 0x3F: "F6",
    0x40: "F7", 0x41: "F8", 0x42: "F9", 0x43: "F10", 0x44: "F11", 0x45: "F12",
    0x49: "Insert", 0x4A: "Home", 0x4B: "PgUp", 0x4C: "Del",
    0x4D: "End", 0x4E: "PgDn", 0x4F: "Right", 0x50: "Left",
    0x51: "Down", 0x52: "Up",
}

USAGE_GLOBE = 0x029D

# 动作显示名缓冲上限(固件 KBMIC_ACTION_NAME_MAX)
ACTION_NAME_MAX = 40


# ---------------------------------------------------------------------------
# 异常
# ---------------------------------------------------------------------------

class ProtocolError(ValueError):
    """线协议 / 编码不合法。消息面向使用者,直接展示即可。"""


class NameTooLongError(ProtocolError):
    """模式名超过 15 字节 UTF-8。"""


# ---------------------------------------------------------------------------
# 偏移:集中在这里,避免各处硬编码 "4 + 197*i + ..." 这种算术
# ---------------------------------------------------------------------------

_STEP_FMT = struct.Struct("<BBBHH")  # kind, mods, keycode, usage, delay_ms = 7 字节
_HDR_FMT = struct.Struct("<BBBB")  # version, active, count, reserved = 4 字节

PROFILE_NAME_OFF = 0
PROFILE_BUILTIN_OFF = 16
PROFILE_SLOTS_OFF = 17


def profile_offset(profile_index: int) -> int:
    """模式 p 在 blob 里的起始偏移。"""
    return HEADER_SIZE + profile_index * PROFILE_SIZE


def slot_offset(profile_index: int, button: int, slot: int) -> int:
    """(b,s) 槽的动作起始偏移。"""
    return (
        profile_offset(profile_index)
        + PROFILE_SLOTS_OFF
        + (button * SLOT_COUNT + slot) * ACTION_SIZE
    )


def step_offset(profile_index: int, button: int, slot: int, index: int) -> int:
    """槽内第 index 步的起始偏移。"""
    return slot_offset(profile_index, button, slot) + 2 + index * STEP_SIZE


# ---------------------------------------------------------------------------
# 数据模型
# ---------------------------------------------------------------------------

@dataclass
class Step:
    """一步。7 字节,packed。"""

    kind: int = STEP_NONE
    mods: int = 0
    keycode: int = 0
    usage: int = 0
    delay_ms: int = 0

    def pack_into(self, buf: bytearray, offset: int) -> None:
        _STEP_FMT.pack_into(
            buf, offset,
            self.kind & 0xFF, self.mods & 0xFF, self.keycode & 0xFF,
            self.usage & 0xFFFF, self.delay_ms & 0xFFFF,
        )

    @classmethod
    def unpack_from(cls, buf: bytes, offset: int) -> "Step":
        return cls(*_STEP_FMT.unpack_from(buf, offset))

    def to_dict(self) -> dict:
        d = {"kind": self.kind, "kind_name": STEP_KIND_NAMES.get(self.kind, "?")}
        if self.kind == STEP_KEY:
            d.update(mods=self.mods, mod_names=mod_names(self.mods), keycode=self.keycode)
        elif self.kind == STEP_CONSUMER:
            d.update(usage=self.usage)
        elif self.kind == STEP_DELAY:
            d.update(delay_ms=self.delay_ms)
        return d

    @classmethod
    def from_dict(cls, d: dict) -> "Step":
        kind = d.get("kind")
        if isinstance(kind, str):
            kind = STEP_KIND_VALUES.get(kind.strip().lower())
        if kind is None:
            raise ProtocolError("step 缺少 kind 字段(0..4 或 none/key/consumer/delay/applefn)")
        mods = d.get("mods", 0)
        if isinstance(mods, str):  # 允许 "Ctrl+Shift" 这种写法
            mods = parse_mods(mods)
        if isinstance(mods, (list, tuple)):
            mods = mod_bits(list(mods))
        try:
            return cls(
                kind=int(kind),
                mods=int(mods),
                keycode=int(d.get("keycode", 0)),
                usage=int(d.get("usage", 0)),
                delay_ms=int(d.get("delay_ms", 0)),
            )
        except (TypeError, ValueError) as exc:
            raise ProtocolError(f"step 字段类型不对: {exc}") from exc

    def validate(self) -> None:
        if self.kind not in STEP_KINDS:
            raise ProtocolError(
                f"step.kind 非法: {self.kind} (有效 0..4: none/key/consumer/delay/applefn)"
            )
        if not 0 <= self.keycode <= 0xFF:
            raise ProtocolError("step.keycode 必须是 0..255 的 HID 用法码")
        if not 0 <= self.usage <= 0xFFFF:
            raise ProtocolError("step.usage 必须是 0..65535 的 Consumer 用法码")
        if not 0 <= self.delay_ms <= 0xFFFF:
            raise ProtocolError("step.delay_ms 必须在 0..65535 之间")
        if not 0 <= self.mods <= 0xFF:
            raise ProtocolError("step.mods 必须在 0..255 之间")


@dataclass
class Action:
    """一个槽里的动作。30 字节,packed。

    steps 只保存"有效步"(固件里的 step_count 决定读多少步),pad 保存结构体尾部
    那几个没用的槽位。保留 pad 是为了让 encode(decode(x)) == x 在字节层面成立,
    否则设备上残留的旧字节会被静默抹掉。
    """

    trigger: int = TRIGGER_NONE
    steps: list[Step] = field(default_factory=list)
    pad: list[Step] = field(default_factory=list)

    @property
    def step_count(self) -> int:
        return len(self.steps)

    def _normalized(self) -> list[Step]:
        used = list(self.steps)[:SEQ_MAX]
        tail = list(self.pad)[: SEQ_MAX - len(used)]
        while len(used) + len(tail) < SEQ_MAX:
            tail.append(Step())
        return used + tail

    def pack_into(self, buf: bytearray, offset: int) -> None:
        allsteps = self._normalized()
        buf[offset] = self.trigger & 0xFF
        buf[offset + 1] = len(self.steps) & 0xFF
        for i, st in enumerate(allsteps):
            st.pack_into(buf, offset + 2 + i * STEP_SIZE)

    @classmethod
    def unpack_from(cls, buf: bytes, offset: int) -> "Action":
        trigger = buf[offset]
        count = buf[offset + 1]
        if count > SEQ_MAX:
            raise ProtocolError(
                f"动作 step_count 非法: {count} (上限 {SEQ_MAX});设备配置已损坏"
            )
        allsteps = [Step.unpack_from(buf, offset + 2 + i * STEP_SIZE) for i in range(SEQ_MAX)]
        return cls(trigger=trigger, steps=allsteps[:count], pad=allsteps[count:])

    def copy(self) -> "Action":
        return Action(
            trigger=self.trigger,
            steps=[Step(s.kind, s.mods, s.keycode, s.usage, s.delay_ms) for s in self.steps],
            pad=[Step(s.kind, s.mods, s.keycode, s.usage, s.delay_ms) for s in self.pad],
        )

    def validate(self) -> None:
        if self.trigger not in TRIGGERS:
            raise ProtocolError(
                f"trigger 非法: {self.trigger} (有效 0..4: none/click/tap/long/double)"
            )
        if len(self.steps) > SEQ_MAX:
            raise ProtocolError(
                f"一个动作最多 {SEQ_MAX} 步,收到 {len(self.steps)} 步"
            )
        for st in self.steps:
            st.validate()

    def to_dict(self) -> dict:
        return {
            "trigger": self.trigger,
            "trigger_name": TRIGGER_NAMES.get(self.trigger, "?"),
            "step_count": self.step_count,
            "name": action_name(self),
            "steps": [s.to_dict() for s in self.steps],
        }

    @classmethod
    def from_dict(cls, d: dict) -> "Action":
        trigger = d.get("trigger", TRIGGER_CLICK)
        if isinstance(trigger, str):
            trigger = _lookup_enum(TRIGGER_NAMES, trigger, "trigger")
        steps_raw = d.get("steps") or []
        if not isinstance(steps_raw, (list, tuple)):
            raise ProtocolError("steps 必须是数组")
        return cls(
            trigger=int(trigger),
            steps=[Step.from_dict(s) for s in steps_raw],
        )


@dataclass
class Profile:
    """一个模式。197 字节,packed。"""

    name: str = ""
    builtin: int = 0
    slots: list[list[Action]] = field(default_factory=list)

    def _empty_slots(self) -> list[list[Action]]:
        if not self.slots:
            return [[Action() for _ in range(SLOT_COUNT)] for _ in range(BTN_COUNT)]
        return self.slots

    def pack_into(self, buf: bytearray, offset: int) -> None:
        # count 之外的占位模式在固件里就是全 0(名字整段 NUL),必须允许写出去;
        # 活着的模式名非空这条规矩由 Config.validate() 负责。
        name_bytes = encode_name(self.name) if self.name else b"\x00" * NAME_MAX
        buf[offset + PROFILE_NAME_OFF: offset + PROFILE_NAME_OFF + NAME_MAX] = name_bytes
        buf[offset + PROFILE_BUILTIN_OFF] = 1 if self.builtin else 0
        slots = self._empty_slots()
        for b in range(BTN_COUNT):
            for s in range(SLOT_COUNT):
                # offset 是模式在 blob 里的绝对偏移,这里只加模式内部偏移。
                act_off = offset + PROFILE_SLOTS_OFF + (b * SLOT_COUNT + s) * ACTION_SIZE
                slots[b][s].pack_into(buf, act_off)

    @classmethod
    def unpack_from(cls, buf: bytes, offset: int) -> "Profile":
        raw = buf[offset + PROFILE_NAME_OFF: offset + PROFILE_NAME_OFF + NAME_MAX]
        nul = raw.find(b"\x00")
        if nul < 0:
            raise ProtocolError(
                "模式名缺少结尾的 '\\0'(16 字节内没找到 NUL);设备配置已损坏"
            )
        try:
            name = raw[:nul].decode("utf-8")
        except UnicodeDecodeError as exc:
            raise ProtocolError(f"模式名不是合法 UTF-8: {raw[:nul]!r}") from exc
        builtin = buf[offset + PROFILE_BUILTIN_OFF]
        slots = [
            [Action.unpack_from(buf, offset + PROFILE_SLOTS_OFF + (b * SLOT_COUNT + s) * ACTION_SIZE)
             for s in range(SLOT_COUNT)]
            for b in range(BTN_COUNT)
        ]
        return cls(name=name, builtin=builtin, slots=slots)

    def copy(self) -> "Profile":
        return Profile(
            name=self.name,
            builtin=self.builtin,
            slots=[[a.copy() for a in row] for row in self._empty_slots()],
        )

    def get_slot(self, button: int, slot: int) -> Action:
        return self._empty_slots()[button][slot]

    def set_slot(self, button: int, slot: int, action: Action) -> None:
        self._empty_slots()[button][slot] = action

    def validate(self, index: int) -> None:
        try:
            encode_name(self.name)
        except NameTooLongError:
            raise
        slots = self._empty_slots()
        for b in range(BTN_COUNT):
            for s in range(SLOT_COUNT):
                try:
                    slots[b][s].validate()
                except ProtocolError as exc:
                    raise ProtocolError(
                        f"模式 {index} ({self.name!r}) 的 {BUTTON_NAMES[b]}/"
                        f"{SLOT_NAMES[s]} 非法: {exc}"
                    ) from exc


@dataclass
class Config:
    """整份配置。2300 字节,packed。

    profiles 永远保持 8 个槽位(count 之外的也要参与编解码,否则字节级往返不成立)。
    """

    version: int = CONFIG_VERSION
    active: int = 0
    count: int = BUILTIN_MODES
    reserved: int = 0
    profiles: list[Profile] = field(default_factory=list)

    def _all_profiles(self) -> list[Profile]:
        out = [p.copy() if isinstance(p, Profile) else Profile() for p in self.profiles]
        while len(out) < MAX_PROFILES:
            out.append(Profile())
        return out[:MAX_PROFILES]

    def encode(self) -> bytes:
        """编成 2300 字节。尺寸不对会在这里直接炸,不会写出一份半截配置。"""
        buf = bytearray(CONFIG_SIZE)
        _HDR_FMT.pack_into(buf, 0, self.version, self.active, self.count, self.reserved)
        for i, prof in enumerate(self._all_profiles()):
            prof.pack_into(buf, profile_offset(i))
        assert len(buf) == CONFIG_SIZE, f"配置长度必须是 {CONFIG_SIZE} 字节"
        return bytes(buf)

    @classmethod
    def decode(cls, blob: bytes) -> "Config":
        if len(blob) != CONFIG_SIZE:
            raise ProtocolError(
                f"配置长度必须是 {CONFIG_SIZE} 字节,收到 {len(blob)} 字节;分片读取不完整"
            )
        version, active, count, reserved = _HDR_FMT.unpack_from(blob, 0)
        profiles = [Profile.unpack_from(blob, profile_offset(i)) for i in range(MAX_PROFILES)]
        return cls(version=version, active=active, count=count,
                   reserved=reserved, profiles=profiles)

    def copy(self) -> "Config":
        return Config(
            version=self.version, active=self.active, count=self.count,
            reserved=self.reserved,
            profiles=[p.copy() for p in self._all_profiles()],
        )

    def live_profiles(self) -> list[Profile]:
        return [p for p in self._all_profiles()[: max(0, self.count)]]

    def validate(self) -> None:
        """与固件 kbmic_config_valid() 同规则,只是这里抛异常而不是返回 false。"""
        if self.version != CONFIG_VERSION:
            raise ProtocolError(
                f"配置版本必须是 {CONFIG_VERSION},收到 {self.version};"
                "请让固件与 MCP 服务端一起升级"
            )
        if not 1 <= self.count <= MAX_PROFILES:
            raise ProtocolError(f"模式数量必须在 1..{MAX_PROFILES} 之间,收到 {self.count}")
        if not 0 <= self.active < self.count:
            raise ProtocolError(
                f"当前模式索引 {self.active} 越界(共 {self.count} 个模式,有效 0..{self.count - 1})"
            )
        for i, prof in enumerate(self._all_profiles()[: self.count]):
            prof.validate(i)
            if i < BUILTIN_MODES and not prof.builtin:
                raise ProtocolError(
                    f"索引 {i} 属于前 {BUILTIN_MODES} 个内置模式,builtin 标记必须是 1"
                )

    # -- 便捷操作(全部是纯内存操作,不发 BLE)--------------------------

    def check_index(self, index: int) -> Profile:
        if not 0 <= index < self.count:
            raise ProtocolError(
                f"模式索引 {index} 越界:当前只有 {self.count} 个模式,有效 0..{self.count - 1}"
            )
        return self._all_profiles()[index]

    def set_active(self, index: int) -> None:
        self.check_index(index)
        self.active = index

    def add_profile(self, name: str) -> int:
        """追加一个用户模式,键位从当前模式拷一份(与固件 kbmic_config_add_profile 一致)。"""
        if self.count >= MAX_PROFILES:
            raise ProtocolError(f"模式数量已达上限 {MAX_PROFILES},请先删掉一个")
        idx = self.count
        src = self._all_profiles()[self.active]
        prof = Profile(name=name, builtin=0, slots=[[a.copy() for a in row] for row in src.slots])
        prof.name = truncate_name(name)
        prof.builtin = 0
        profiles = self._all_profiles()
        profiles[idx] = prof
        self.profiles = profiles
        self.count = idx + 1
        return idx

    def delete_profile(self, index: int) -> None:
        prof = self.check_index(index)
        if prof.builtin:
            raise ProtocolError(
                f"模式 {index} ({prof.name!r}) 是内置模式,不能删除;"
                f"可删除范围是 {BUILTIN_MODES}..{self.count - 1}"
            )
        profiles = self._all_profiles()
        del profiles[index]
        profiles.append(Profile())
        self.profiles = profiles
        self.count -= 1
        if self.active >= self.count:
            self.active = self.count - 1
        elif self.active > index:
            self.active -= 1

    def to_dict(self) -> dict:
        return {
            "version": self.version,
            "active": self.active,
            "count": self.count,
            "profiles": [self.profile_to_dict(p, i) for i, p in
                         enumerate(self._all_profiles()[: self.count])],
        }

    @staticmethod
    def profile_to_dict(prof: Profile, index: int) -> dict:
        slots = prof._empty_slots()
        return {
            "index": index,
            "name": prof.name,
            "builtin": bool(prof.builtin),
            "buttons": {
                BUTTON_NAMES[b]: {SLOT_NAMES[s]: slots[b][s].to_dict()
                                  for s in range(SLOT_COUNT)}
                for b in range(BTN_COUNT)
            },
        }

    def modes_view(self) -> list[dict]:
        """紧凑视图:每个模式一行,给出三个键两个槽当前的动作名。

        值刻意只用字符串,这样这一层可以直接塞进 MCP 的返回值里,不需要再序列化。
        """
        rows = []
        for i, prof in enumerate(self._all_profiles()[: self.count]):
            slots = prof._empty_slots()
            row = {
                "index": i,
                "name": prof.name,
                "builtin": bool(prof.builtin),
                "active": i == self.active,
            }
            for b in range(BTN_COUNT):
                for s in range(SLOT_COUNT):
                    row[f"{BUTTON_NAMES[b]}.{SLOT_NAMES[s]}"] = action_name(slots[b][s])
            rows.append(row)
        return rows


# ---------------------------------------------------------------------------
# 名字与枚举解析
# ---------------------------------------------------------------------------

def encode_name(name: str) -> bytes:
    """UTF-8 编码成 16 字节定长字段(补 '\\0')。超长直接拒绝,不静默截断。"""
    if not isinstance(name, str):
        raise ProtocolError(f"模式名必须是字符串,收到 {type(name).__name__}")
    if "\x00" in name:
        raise ProtocolError("模式名不能包含 '\\0'")
    raw = name.encode("utf-8")
    if len(raw) > NAME_CONTENT_MAX:
        raise NameTooLongError(
            f"模式名 {name!r} 的 UTF-8 长度是 {len(raw)} 字节,超过上限 {NAME_CONTENT_MAX} 字节"
        )
    if not raw:
        raise ProtocolError("模式名不能为空")
    return raw + b"\x00" * (NAME_MAX - len(raw))


def truncate_name(name: str, default: str = "自定义") -> str:
    """按固件 kbmic_config_add_profile 的做法截断到 15 字节(不切碎 UTF-8 字符)。"""
    raw = (name or default).encode("utf-8")[:NAME_CONTENT_MAX]
    return raw.decode("utf-8", errors="ignore") or default


def mod_names(mods: int) -> list[str]:
    out = []
    if mods & MOD_CTRL:
        out.append("Ctrl")
    if mods & MOD_SHIFT:
        out.append("Shift")
    if mods & MOD_ALT:
        out.append("Alt")
    if mods & MOD_GUI:
        out.append("Win")
    return out


def mod_bits(names: Iterable[str]) -> int:
    bits = 0
    table = {"ctrl": MOD_CTRL, "control": MOD_CTRL, "shift": MOD_SHIFT,
             "alt": MOD_ALT, "opt": MOD_ALT, "win": MOD_GUI, "gui": MOD_GUI,
             "cmd": MOD_GUI, "command": MOD_GUI, "super": MOD_GUI}
    for n in names:
        key = str(n).strip().lower()
        if key not in table:
            raise ProtocolError(f"未知修饰键 {n!r}(可用: Ctrl/Shift/Alt/Win)")
        bits |= table[key]
    return bits


def parse_mods(text: str) -> int:
    """把 "Ctrl+Shift"、"ctrl,win" 之类的写法变成位掩码。"""
    for sep in ("+", ",", "|"):
        text = text.replace(sep, " ")
    return mod_bits(text.split())


# 名字解析:供 MCP 工具把 "Up" / "长按" / "tap" 之类的入参变成索引
def _lookup_enum(table: dict[int, str], text: str, what: str) -> int:
    key = text.strip().lower()
    for value, name in table.items():
        if name == key:
            return value
    raise ProtocolError(f"{what} 取值非法: {text!r} (可用: {', '.join(table.values())})")


def parse_button(value) -> int:
    if isinstance(value, bool):
        raise ProtocolError("按钮必须是 Up/Down/OK 或 0/1/2")
    if isinstance(value, int):
        if 0 <= value < BTN_COUNT:
            return value
        raise ProtocolError(f"按钮索引越界: {value} (有效 0..{BTN_COUNT - 1}: 0=Up 1=Down 2=OK)")
    key = str(value).strip().lower()
    if key in BUTTON_ALIASES:
        return BUTTON_ALIASES[key]
    if key.isdigit() and 0 <= int(key) < BTN_COUNT:
        return int(key)
    raise ProtocolError(f"按钮取值非法: {value!r} (可用: Up/Down/OK 或 0/1/2)")


def parse_slot(value) -> int:
    if isinstance(value, bool):
        raise ProtocolError("槽位必须是 short-tap/long-press 或 0/1")
    if isinstance(value, int):
        if 0 <= value < SLOT_COUNT:
            return value
        raise ProtocolError(f"槽位索引越界: {value} (有效 0..1: 0=short-tap 1=long-press)")
    key = str(value).strip().lower()
    if key in SLOT_ALIASES:
        return SLOT_ALIASES[key]
    if key.isdigit() and 0 <= int(key) < SLOT_COUNT:
        return int(key)
    raise ProtocolError(f"槽位取值非法: {value!r} (可用: short-tap/long-press 或 0/1)")


def parse_trigger(value) -> int:
    if isinstance(value, bool):
        raise ProtocolError("trigger 必须是 none/click/tap/long/double 或 0..4")
    if isinstance(value, int):
        if value in TRIGGERS:
            return value
        raise ProtocolError(f"trigger 越界: {value} (有效 0..4)")
    return _lookup_enum(TRIGGER_NAMES, str(value), "trigger")


def parse_kind(value) -> int:
    if isinstance(value, bool):
        raise ProtocolError("kind 必须是 none/key/consumer/delay/applefn 或 0..4")
    if isinstance(value, int):
        if value in STEP_KINDS:
            return value
        raise ProtocolError(f"kind 越界: {value} (有效 0..4)")
    key = str(value).strip().lower()
    if key in STEP_KIND_VALUES:
        return STEP_KIND_VALUES[key]
    raise ProtocolError(
        f"kind 取值非法: {value!r} (可用: {', '.join(STEP_KIND_NAMES.values())})"
    )


# ---------------------------------------------------------------------------
# 动作显示名
#
# 规则与固件 kbmic_action_name() 一致:不落盘,每次从步骤现拼。
#   KEY      Ctrl+Shift+Alt+Win+ 前缀 + 键名;keycode==0 且只有修饰键时去掉尾部 '+'
#   CONSUMER 0x029D -> Globe,其余 -> C:0x%04X
#   DELAY    +%dms
#   多步用单个空格连接;TAP 触发追加 " (hold)";空动作或 trigger==0 -> "-"
#   kind=NONE 且 mods 高位置位 = Settings 软件动作
# ---------------------------------------------------------------------------

def _step_text(step: Step) -> str:
    if step.kind == STEP_KEY:
        prefix = "".join(m + "+" for m in mod_names(step.mods))
        if step.keycode == 0:
            return prefix[:-1] if prefix else ""  # 纯修饰键:去掉尾部 '+'
        return prefix + KEY_NAMES.get(step.keycode, "0x%02X" % step.keycode)
    if step.kind == STEP_CONSUMER:
        return "Globe" if step.usage == USAGE_GLOBE else "C:0x%04X" % step.usage
    if step.kind == STEP_DELAY:
        return "+%dms" % step.delay_ms
    if step.kind == STEP_APPLEFN:
        return "Fn"
    return ""


def is_settings(action: Action) -> bool:
    """是否为"打开机身菜单"这个软件动作(固件 kbmic_action_is_settings)。"""
    return (action.step_count > 0
            and action.steps[0].kind == STEP_NONE
            and bool(action.steps[0].mods & MOD_FLAG))


def action_name(action: Action) -> str:
    """由步骤反推 ASCII 显示名。"""
    if is_settings(action):
        return "Settings"
    if action.trigger == TRIGGER_NONE or action.step_count == 0:
        return "-"
    parts = [_step_text(s) for s in action.steps]
    text = " ".join(parts)
    if not text.strip():
        text = "-"
    if action.trigger == TRIGGER_TAP:
        text += " (hold)"
    return text


def action_text(action: Action, cap: int = ACTION_NAME_MAX) -> str:
    """与固件一样截到 KBMIC_ACTION_NAME_MAX 字节。"""
    out = action_name(action).encode("utf-8")[: max(1, cap)]
    return out.decode("utf-8", errors="ignore")


# ---------------------------------------------------------------------------
# BLE 分片
# ---------------------------------------------------------------------------

def split_chunks(blob: bytes) -> list[bytes]:
    """2300 字节切成 15 片(前 14 片 160,第 15 片 60)。"""
    if len(blob) != CONFIG_SIZE:
        raise ProtocolError(f"待写入的配置必须是 {CONFIG_SIZE} 字节,收到 {len(blob)} 字节")
    return [blob[i * CHUNK_SIZE: (i + 1) * CHUNK_SIZE] for i in range(CHUNK_COUNT)]


def join_chunks(chunks: Iterable[bytes]) -> bytes:
    """把读到的分片拼回 2300 字节。总长不对时明确报错,不做静默补齐。"""
    parts = list(chunks)
    if len(parts) != CHUNK_COUNT:
        raise ProtocolError(
            f"分片数量必须是 {CHUNK_COUNT},收到 {len(parts)};"
            "设备端服务可能没注册全,或读到了别的分片"
        )
    blob = b"".join(parts)
    if len(blob) != CONFIG_SIZE:
        raise ProtocolError(
            f"分片拼回来是 {len(blob)} 字节,应为 {CONFIG_SIZE} 字节;"
            f"实际各片: {[len(p) for p in parts]} (期望 {[chunk_expected_len(i) for i in range(CHUNK_COUNT)]})"
        )
    return blob


# ---------------------------------------------------------------------------
# 事件报文
# ---------------------------------------------------------------------------

def decode_event(data: bytes) -> dict:
    """解析 Event 通知: type, active, aux, reserved, name_len, name。"""
    if len(data) < 5:
        raise ProtocolError(f"事件报文至少 5 字节,收到 {len(data)}")
    etype, active, aux, _reserved, name_len = data[0], data[1], data[2], data[3], data[4]
    if name_len > len(data) - 5:  # 宁可截断也不要抛异常打断事件流
        name_len = len(data) - 5
    name = data[5: 5 + name_len].decode("utf-8", errors="replace")
    return {
        "type": etype,
        "type_name": EV_TYPES.get(etype, "UNKNOWN"),
        "active": active,
        "aux": aux,
        "name": name,
        "raw": data[: 5 + name_len].hex(),
    }


# ---------------------------------------------------------------------------
# 自检
# ---------------------------------------------------------------------------

def self_check() -> dict:
    """导入即跑不起眼、但能挡住布局写错的算术自检。返回一份可打印的摘要。"""
    assert STEP_SIZE == _STEP_FMT.size == 7
    assert HEADER_SIZE == _HDR_FMT.size == 4
    assert ACTION_SIZE == 2 + SEQ_MAX * STEP_SIZE == 30
    assert PROFILE_SIZE == NAME_MAX + 1 + BTN_COUNT * SLOT_COUNT * ACTION_SIZE == 287
    assert CONFIG_SIZE == HEADER_SIZE + MAX_PROFILES * PROFILE_SIZE == 2300
    assert CHUNK_COUNT == (CONFIG_SIZE + CHUNK_SIZE - 1) // CHUNK_SIZE == 15
    assert chunk_expected_len(14) == 60
    assert len(DEVICE_NAME.encode("utf-8")) == 11
    assert profile_offset(1) == 4 + 287
    assert step_offset(0, 2, 1, 3) == 4 + 17 + (2 * 3 + 1) * 30 + 2 + 3 * 7

    # 全零配置必须能编解码往返
    zero = Config(count=1, active=0, profiles=[Profile(name="-")])
    assert len(zero.encode()) == CONFIG_SIZE
    assert Config.decode(zero.encode()).encode() == zero.encode()

    # 出厂默认配置(从 catalog 延迟导入,避免模块级循环依赖)必须正好 2300 字节
    from catalog import factory_default_config  # noqa: PLC0415

    default = factory_default_config()
    blob = default.encode()
    assert len(blob) == CONFIG_SIZE, f"出厂配置编出来是 {len(blob)} 字节,应为 {CONFIG_SIZE}"
    assert Config.decode(blob).encode() == blob

    return {
        "config_size": CONFIG_SIZE,
        "profile_size": PROFILE_SIZE,
        "action_size": ACTION_SIZE,
        "step_size": STEP_SIZE,
        "chunks": CHUNK_COUNT,
        "last_chunk_len": chunk_expected_len(CHUNK_COUNT - 1),
        "default_bytes": len(blob),
    }


if __name__ == "__main__":
    import json

    print(json.dumps(self_check(), indent=2))
