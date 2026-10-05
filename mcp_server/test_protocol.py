"""协议层单元测试 —— 不需要硬件,也不需要 bleak / mcp。

    python3 -m unittest discover -s /Users/weibh/Desktop/aipassport-kbmic/mcp_server -v

覆盖:
  * blob 编解码后正好 1580 字节,且 encode(decode(x)) == x(字节级往返)
  * 出厂默认配置的往返
  * 动作显示名的各种边角情况(Ctrl+Win / Globe / 多步 / TAP 后缀 / "-")
  * 越界索引、删除内置模式、名字超长、步数超限等拒绝路径
"""

from __future__ import annotations

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import catalog  # noqa: E402
import protocol as P  # noqa: E402
from protocol import (  # noqa: E402
    Action,
    Config,
    Profile,
    ProtocolError,
    Step,
    MOD_ALT,
    MOD_CTRL,
    MOD_GUI,
    MOD_SHIFT,
    STEP_CONSUMER,
    STEP_DELAY,
    STEP_KEY,
    STEP_NONE,
    TRIGGER_CLICK,
    TRIGGER_NONE,
    TRIGGER_TAP,
)


# ---------------------------------------------------------------------------
# 尺寸与布局
# ---------------------------------------------------------------------------

class TestLayout(unittest.TestCase):
    def test_sizes_match_firmware(self):
        self.assertEqual(P.CONFIG_SIZE, 1580)
        self.assertEqual(P.PROFILE_SIZE, 197)
        self.assertEqual(P.ACTION_SIZE, 30)
        self.assertEqual(P.STEP_SIZE, 7)
        self.assertEqual(P.CHUNK_COUNT, 10)
        self.assertEqual(P.CHUNK_SIZE, 160)
        self.assertEqual(P.chunk_expected_len(9), 140)

    def test_absolute_step_offset(self):
        # 题面给的算式:4 + 197*b_index + 17 + (b*2+s)*30 + 2 + i*7
        for b_index, b, s, i in ((0, 0, 0, 0), (3, 2, 1, 2), (7, 1, 0, 3)):
            expected = 4 + 197 * b_index + 17 + (b * 2 + s) * 30 + 2 + i * 7
            self.assertEqual(P.step_offset(b_index, b, s, i), expected)

    def test_uuid_scheme(self):
        self.assertEqual(P.SERVICE_UUID, "7d1c5a30-9f6e-4a21-8c3d-2b5e7a9f1c48")
        self.assertEqual(P.chunk_uuid(0), "7d1c5a40-9f6e-4a21-8c3d-2b5e7a9f1c48")
        self.assertEqual(P.chunk_uuid(9), "7d1c5a49-9f6e-4a21-8c3d-2b5e7a9f1c48")
        self.assertEqual(P.EVENT_UUID, "7d1c5a4f-9f6e-4a21-8c3d-2b5e7a9f1c48")
        self.assertEqual(len(P.DEVICE_NAME.encode("utf-8")), 11)

    def test_uuids_are_unique(self):
        uuids = [P.SERVICE_UUID, P.EVENT_UUID] + [P.chunk_uuid(i) for i in range(P.CHUNK_COUNT)]
        self.assertEqual(len(set(uuids)), len(uuids))

    def test_self_check_reports_1580(self):
        report = P.self_check()
        self.assertEqual(report["config_size"], 1580)
        self.assertEqual(report["default_bytes"], 1580)


# ---------------------------------------------------------------------------
# 编解码往返
# ---------------------------------------------------------------------------

class TestRoundTrip(unittest.TestCase):
    def test_factory_default_is_1580(self):
        blob = catalog.factory_default_config().encode()
        self.assertIsInstance(blob, bytes)
        self.assertEqual(len(blob), 1580)

    def test_encode_decode_is_1580(self):
        cfg = catalog.factory_default_config()
        decoded = Config.decode(cfg.encode())
        self.assertEqual(len(decoded.encode()), 1580)

    def test_encode_of_decode_equals_original(self):
        blob = catalog.factory_default_config().encode()
        self.assertEqual(Config.decode(blob).encode(), blob)

    def test_round_trip_of_mutated_config(self):
        cfg = catalog.factory_default_config()
        cfg.profiles[2].set_slot(P.BTN_OK, P.SLOT_TAP,
                                catalog.action_tap(catalog.step_key(0x1B, MOD_CTRL | MOD_SHIFT)))
        cfg.add_profile("我的语音")
        cfg.set_active(4)
        blob = cfg.encode()
        self.assertEqual(len(blob), 1580)
        back = Config.decode(blob)
        self.assertEqual(back.encode(), blob)
        self.assertEqual(back.count, 5)
        self.assertEqual(back.active, 4)
        self.assertEqual(back.profiles[4].name, "我的语音")
        self.assertEqual(
            P.action_name(back.profiles[2].get_slot(P.BTN_OK, P.SLOT_TAP)),
            "Ctrl+Shift+X (hold)",
        )

    def test_header_fields(self):
        cfg = catalog.factory_default_config()
        blob = cfg.encode()
        self.assertEqual(blob[0], 1, "version 必须是 1")
        self.assertEqual(blob[1], 0, "active 必须是 0")
        self.assertEqual(blob[2], 4, "出厂配置 4 个模式")
        self.assertEqual(blob[3], 0, "reserved")

    def test_field_offsets_are_little_endian(self):
        blob = catalog.factory_default_config().encode()
        # 模式 0 的 builtin 标记在 4 + 16
        self.assertEqual(blob[4 + 16], 1)
        # Mac 的 Up/短按 = CLICK + 1 步 KEY(0x28)
        off = P.slot_offset(0, P.BTN_UP, P.SLOT_TAP)
        self.assertEqual(blob[off], TRIGGER_CLICK)
        self.assertEqual(blob[off + 1], 1)
        step_off = off + 2
        self.assertEqual(blob[step_off], STEP_KEY)       # kind
        self.assertEqual(blob[step_off + 1], 0)          # mods
        self.assertEqual(blob[step_off + 2], 0x28)       # keycode
        self.assertEqual(struct.unpack_from("<H", blob, step_off + 3)[0], 0)  # usage

    def test_name_is_nul_terminated_utf8(self):
        blob = catalog.factory_default_config().encode()
        self.assertEqual(blob[4: 4 + 4], b"Mac\x00")
        cfg = catalog.factory_default_config()
        idx = cfg.add_profile("语音🎤")
        self.assertEqual(idx, 4)
        # 15 字节上限:"语音"=6 字节 + 🎤=4 字节 = 10 字节,放得下
        blob = cfg.encode()
        self.assertEqual(Config.decode(blob).profiles[4].name, "语音🎤")

    def test_decode_rejects_wrong_length(self):
        with self.assertRaises(ProtocolError):
            Config.decode(b"\x00" * 1579)
        with self.assertRaises(ProtocolError):
            Config.decode(b"\x00" * 1581)

    def test_decode_rejects_bad_step_count(self):
        blob = bytearray(catalog.factory_default_config().encode())
        blob[P.slot_offset(0, P.BTN_UP, P.SLOT_TAP) + 1] = 5  # step_count > 4
        with self.assertRaises(ProtocolError):
            Config.decode(bytes(blob))

    def test_decode_rejects_unterminated_name(self):
        blob = bytearray(catalog.factory_default_config().encode())
        blob[4: 4 + P.NAME_MAX] = b"A" * P.NAME_MAX  # 整段没有 '\0'
        with self.assertRaises(ProtocolError):
            Config.decode(bytes(blob))

    def test_padding_bytes_are_preserved(self):
        """尾部 pad 步原样往返 —— 不然设备上的残留字节会被静默抹掉。"""
        cfg = catalog.factory_default_config()
        act = cfg.profiles[0].get_slot(P.BTN_UP, P.SLOT_LONG)
        act.pad = [Step(kind=STEP_KEY, mods=0x03, keycode=0x1D)]
        blob = cfg.encode()
        back = Config.decode(blob)
        self.assertEqual(back.encode(), blob)
        self.assertEqual(back.profiles[0].get_slot(P.BTN_UP, P.SLOT_LONG).pad[0].keycode, 0x1D)


# ---------------------------------------------------------------------------
# 分片
# ---------------------------------------------------------------------------

class TestChunks(unittest.TestCase):
    def test_split_and_join(self):
        blob = catalog.factory_default_config().encode()
        chunks = P.split_chunks(blob)
        self.assertEqual(len(chunks), P.CHUNK_COUNT)
        self.assertEqual([len(c) for c in chunks], [160] * 9 + [140])
        self.assertEqual(P.join_chunks(chunks), blob)

    def test_split_rejects_wrong_size(self):
        with self.assertRaises(ProtocolError):
            P.split_chunks(b"\x00" * 100)

    def test_join_rejects_short_result(self):
        chunks = [b"\x00" * 160] * 9 + [b"\x00" * 100]  # 少 40 字节
        with self.assertRaises(ProtocolError):
            P.join_chunks(chunks)

    def test_join_rejects_wrong_chunk_count(self):
        with self.assertRaises(ProtocolError):
            P.join_chunks([b"\x00" * 160] * 3)


# ---------------------------------------------------------------------------
# 动作显示名
# ---------------------------------------------------------------------------

class TestActionNames(unittest.TestCase):
    def name(self, trigger, *steps) -> str:
        return P.action_name(Action(trigger=trigger, steps=list(steps)))

    def test_empty_is_dash(self):
        self.assertEqual(self.name(TRIGGER_CLICK), "-")
        self.assertEqual(self.name(TRIGGER_NONE, catalog.step_key(0x28)), "-")

    def test_plain_key(self):
        self.assertEqual(self.name(TRIGGER_CLICK, Step(STEP_KEY, 0, 0x28)), "Enter")
        self.assertEqual(self.name(TRIGGER_CLICK, Step(STEP_KEY, 0, 0x2C)), "Space")
        self.assertEqual(self.name(TRIGGER_CLICK, Step(STEP_KEY, 0, 0x1D)), "Z")
        self.assertEqual(self.name(TRIGGER_CLICK, Step(STEP_KEY, 0, 0x27)), "0")

    def test_modifier_prefixes_in_order(self):
        # 顺序固定 Ctrl+Shift+Alt+Win
        step = Step(STEP_KEY, MOD_CTRL | MOD_SHIFT | MOD_GUI, 0x04)
        self.assertEqual(self.name(TRIGGER_CLICK, step), "Ctrl+Shift+Win+A")

    def test_modifier_only_drops_trailing_plus(self):
        self.assertEqual(self.name(TRIGGER_CLICK, Step(STEP_KEY, MOD_CTRL | MOD_GUI, 0)),
                         "Ctrl+Win")
        self.assertEqual(self.name(TRIGGER_CLICK, Step(STEP_KEY, MOD_SHIFT, 0)), "Shift")
        self.assertEqual(self.name(TRIGGER_CLICK, Step(STEP_KEY, MOD_GUI, 0)), "Win")
        self.assertEqual(self.name(TRIGGER_CLICK, Step(STEP_KEY, 0, 0)), "-")

    def test_globe(self):
        self.assertEqual(self.name(TRIGGER_TAP, Step(STEP_CONSUMER, 0, 0, P.USAGE_GLOBE)),
                         "Globe (hold)")
        self.assertEqual(self.name(TRIGGER_CLICK, Step(STEP_CONSUMER, 0, 0, 0x00E9)),
                         "C:0x00E9")

    def test_tap_suffix(self):
        self.assertEqual(self.name(TRIGGER_TAP, Step(STEP_KEY, 0, 0x28)), "Enter (hold)")
        self.assertEqual(self.name(TRIGGER_CLICK, Step(STEP_KEY, 0, 0x28)), "Enter")

    def test_multi_step_joined_by_single_space(self):
        act = Action(trigger=TRIGGER_CLICK, steps=[
            catalog.step_key(0x28), catalog.step_key(0x28), catalog.step_key(0x28)])
        self.assertEqual(P.action_name(act), "Enter Enter Enter")

    def test_mixed_steps(self):
        act = Action(trigger=TRIGGER_CLICK, steps=[
            catalog.step_key(0x28), catalog.step_delay(100), catalog.step_key(0x28)])
        self.assertEqual(P.action_name(act), "Enter +100ms Enter")

    def test_settings_software_action(self):
        act = Action(trigger=TRIGGER_NONE, steps=[Step(STEP_NONE, 0xF0)])
        self.assertEqual(P.action_name(act), "Settings")
        self.assertTrue(P.is_settings(act))

    def test_unknown_keycode_falls_back_to_hex(self):
        self.assertEqual(self.name(TRIGGER_CLICK, Step(STEP_KEY, 0, 0x63)), "0x63")
        self.assertEqual(self.name(TRIGGER_CLICK, Step(STEP_KEY, MOD_ALT, 0x63)), "Alt+0x63")

    def test_names_of_all_builtin_default_slots(self):
        cfg = catalog.factory_default_config()
        expect = {
            (0, P.BTN_UP, P.SLOT_TAP): "Enter",
            (0, P.BTN_UP, P.SLOT_LONG): "Enter Enter Enter",
            (0, P.BTN_DOWN, P.SLOT_TAP): "Back",
            (0, P.BTN_DOWN, P.SLOT_LONG): "Settings",
            (0, P.BTN_OK, P.SLOT_TAP): "Globe (hold)",
            # OK 长按留空:说话=按住 OK,长按阈值不能把说话切断(进设置在下键长按)。
            (0, P.BTN_OK, P.SLOT_LONG): "-",
            (1, P.BTN_OK, P.SLOT_TAP): "Ctrl+Win (hold)",
            (2, P.BTN_OK, P.SLOT_TAP): "Space (hold)",
            (3, P.BTN_OK, P.SLOT_TAP): "Globe (hold)",
        }
        for (i, b, s), want in expect.items():
            got = P.action_name(cfg.profiles[i].get_slot(b, s))
            self.assertEqual(got, want, f"profile {i} {P.BUTTON_NAMES[b]}/{P.SLOT_NAMES[s]}")

    def test_catalog_names_render_as_expected(self):
        want = {
            0: "-",
            1: "Enter",
            2: "Back",
            3: "Tab",
            4: "Space",
            5: "Esc",
            6: "Globe (hold)",
            7: "Ctrl+Win (hold)",
            8: "Up",
            9: "Down",
            10: "Left",
            11: "Right",
            12: "Enter Enter Enter",
            13: "F1",
            14: "F2",
            15: "Settings",
        }
        for item in catalog.CATALOG:
            self.assertEqual(P.action_name(item.action), want[item.id], f"id {item.id}")

    def test_keycode_table_coverage(self):
        for code, label in ((0x04, "A"), (0x1D, "Z"), (0x1E, "1"), (0x27, "0"),
                            (0x28, "Enter"), (0x29, "Esc"), (0x2A, "Back"), (0x2B, "Tab"),
                            (0x2C, "Space"), (0x2D, "Minus"), (0x2E, "Equal"),
                            (0x3A, "F1"), (0x45, "F12"), (0x49, "Insert"), (0x4A, "Home"),
                            (0x4B, "PgUp"), (0x4C, "Del"), (0x4D, "End"), (0x4E, "PgDn"),
                            (0x4F, "Right"), (0x50, "Left"), (0x51, "Down"), (0x52, "Up")):
            self.assertEqual(P.KEY_NAMES[code], label, f"keycode 0x{code:02X}")

    def test_action_name_fits_firmware_buffer(self):
        act = Action(trigger=TRIGGER_CLICK,
                     steps=[catalog.step_key(0x04, MOD_CTRL | MOD_SHIFT | MOD_ALT | MOD_GUI)] * 4)
        self.assertLessEqual(len(P.action_text(act).encode()), P.ACTION_NAME_MAX)


# ---------------------------------------------------------------------------
# 拒绝路径(改设备之前就该失败)
# ---------------------------------------------------------------------------

class TestRejections(unittest.TestCase):
    def setUp(self):
        self.cfg = catalog.factory_default_config()

    def test_out_of_range_profile_index(self):
        with self.assertRaises(ProtocolError):
            self.cfg.check_index(4)      # 只有 4 个模式
        with self.assertRaises(ProtocolError):
            self.cfg.set_active(4)
        with self.assertRaises(ProtocolError):
            self.cfg.check_index(-1)
        with self.assertRaises(ProtocolError):
            self.cfg.check_index(99)

    def test_delete_builtin_rejected(self):
        for i in range(P.BUILTIN_MODES):
            with self.assertRaises(ProtocolError) as ctx:
                self.cfg.delete_profile(i)
            self.assertIn("内置", str(ctx.exception))
        # 拒绝之后不能有任何副作用
        self.assertEqual(self.cfg.count, 4)
        self.assertEqual([p.name for p in self.cfg.live_profiles()],
                         ["Mac", "Windows", "Android", "iOS"])

    def test_delete_user_profile_ok(self):
        self.cfg.add_profile("测试")
        self.cfg.set_active(4)
        self.cfg.delete_profile(4)
        self.assertEqual(self.cfg.count, 4)
        self.assertEqual(self.cfg.active, 3, "删掉的是当前模式,active 要退到上一个")
        self.assertEqual(len(self.cfg.encode()), 1580)

    def test_delete_shifts_active_down(self):
        self.cfg.add_profile("A")
        self.cfg.add_profile("B")   # index 5
        self.cfg.set_active(5)
        self.cfg.delete_profile(4)
        self.assertEqual(self.cfg.count, 5)
        self.assertEqual(self.cfg.active, 4, "删掉前面的模式,active 要跟着往前挪")

    def test_add_profile_copies_current_mode(self):
        self.cfg.set_active(1)  # Windows
        idx = self.cfg.add_profile("我的")
        self.assertEqual(idx, 4)
        self.assertEqual(self.cfg.profiles[4].builtin, 0)
        for b in range(P.BTN_COUNT):
            for s in range(P.SLOT_COUNT):
                self.assertEqual(
                    P.action_name(self.cfg.profiles[4].get_slot(b, s)),
                    P.action_name(self.cfg.profiles[1].get_slot(b, s)),
                )

    def test_add_profile_beyond_limit(self):
        for name in ("a", "b", "c", "d"):
            self.cfg.add_profile(name)
        self.assertEqual(self.cfg.count, P.MAX_PROFILES)
        with self.assertRaises(ProtocolError):
            self.cfg.add_profile("e")

    def test_name_too_long_rejected(self):
        with self.assertRaises(ProtocolError):
            P.encode_name("A" * 16)           # 15 字节上限
        P.encode_name("A" * 15)               # 15 字节刚好
        with self.assertRaises(ProtocolError):
            P.encode_name("中文模式名超过十五字节了哦")  # 12 汉字 = 36 字节

    def test_empty_name_rejected(self):
        with self.assertRaises(ProtocolError):
            P.encode_name("")

    def test_name_truncation_never_splits_utf8(self):
        truncated = P.truncate_name("一二三四五六七八九十甲乙丙丁")
        self.assertLessEqual(len(truncated.encode("utf-8")), 15)
        truncated.encode("utf-8").decode("utf-8")  # 不该抛异常

    def test_too_many_steps_rejected(self):
        act = Action(trigger=TRIGGER_CLICK, steps=[catalog.step_key(0x28)] * 5)
        with self.assertRaises(ProtocolError):
            act.validate()

    def test_bad_kind_and_trigger_rejected(self):
        with self.assertRaises(ProtocolError):
            Action(trigger=TRIGGER_CLICK, steps=[Step(kind=9)]).validate()
        with self.assertRaises(ProtocolError):
            Action(trigger=9).validate()
        with self.assertRaises(ProtocolError):
            Step(kind=STEP_KEY, keycode=999).validate()
        with self.assertRaises(ProtocolError):
            Step(kind=STEP_DELAY, delay_ms=70000).validate()

    def test_bad_version_rejected(self):
        self.cfg.version = 2
        with self.assertRaises(ProtocolError):
            self.cfg.validate()

    def test_active_out_of_range_rejected(self):
        self.cfg.active = 4
        with self.assertRaises(ProtocolError):
            self.cfg.validate()

    def test_builtin_flag_cleared_rejected(self):
        self.cfg.profiles[0] = Profile(name="假内置", builtin=0)
        with self.assertRaises(ProtocolError):
            self.cfg.validate()

    def test_empty_name_in_profile_rejected(self):
        self.cfg.profiles[0] = Profile(name="", builtin=1)
        with self.assertRaises(ProtocolError):
            self.cfg.validate()

    def test_catalog_lookup(self):
        self.assertEqual(catalog.lookup(1).name, "Enter")
        self.assertEqual(catalog.lookup("Enter").name, "Enter")
        self.assertEqual(catalog.lookup("enter").id, 1)
        self.assertEqual(catalog.lookup("globe").id, 6)
        self.assertEqual(catalog.lookup("Ctrl+Win").id, 7)
        with self.assertRaises(ProtocolError):
            catalog.lookup(99)
        with self.assertRaises(ProtocolError):
            catalog.lookup("不存在的动作")

    def test_match_catalog_round_trip(self):
        for item in catalog.CATALOG:
            self.assertEqual(catalog.match_catalog(item.action), item.id)

    def test_button_and_slot_parsing(self):
        self.assertEqual(P.parse_button("Up"), P.BTN_UP)
        self.assertEqual(P.parse_button(2), P.BTN_OK)
        self.assertEqual(P.parse_slot("long-press"), P.SLOT_LONG)
        self.assertEqual(P.parse_slot(0), P.SLOT_TAP)
        with self.assertRaises(ProtocolError):
            P.parse_button("Middle")
        with self.assertRaises(ProtocolError):
            P.parse_button(5)
        with self.assertRaises(ProtocolError):
            P.parse_slot("double-click")

    def test_mods_parsing(self):
        self.assertEqual(P.parse_mods("Ctrl+Shift"), MOD_CTRL | MOD_SHIFT)
        self.assertEqual(P.parse_mods("win"), MOD_GUI)
        with self.assertRaises(ProtocolError):
            P.parse_mods("Hyper")

    def test_step_from_dict_kinds(self):
        self.assertEqual(P.Step.from_dict({"kind": "key", "keycode": 0x28}).kind, STEP_KEY)
        self.assertEqual(P.Step.from_dict({"kind": 2, "usage": 0x29D}).kind, STEP_CONSUMER)
        self.assertEqual(P.Step.from_dict({"kind": "delay", "delay_ms": 50}).delay_ms, 50)
        with self.assertRaises(ProtocolError):
            P.Step.from_dict({})

    def test_action_from_dict_rejects_unknown_trigger(self):
        with self.assertRaises(ProtocolError):
            P.Action.from_dict({"trigger": "double-tap", "steps": []})

    def test_builtin_defaults_only_for_first_four(self):
        for i in range(P.BUILTIN_MODES):
            self.assertEqual(catalog.builtin_profile_defaults(i).name,
                             ["Mac", "Windows", "Android", "iOS"][i])
        with self.assertRaises(ProtocolError):
            catalog.builtin_profile_defaults(4)


# ---------------------------------------------------------------------------
# 事件报文
# ---------------------------------------------------------------------------

class TestEvent(unittest.TestCase):
    def test_decode_boot_event(self):
        name = "Android"
        data = bytes([P.EV_BOOT, 2, 0, 0, len(name.encode())]) + name.encode("utf-8")
        ev = P.decode_event(data)
        self.assertEqual(ev["type_name"], "BOOT")
        self.assertEqual(ev["active"], 2)
        self.assertEqual(ev["name"], "Android")

    def test_decode_key_event(self):
        data = bytes([P.EV_KEY, 0, P.BTN_OK, 0, 0])
        ev = P.decode_event(data)
        self.assertEqual(ev["type_name"], "KEY")
        self.assertEqual(ev["aux"], 2)
        self.assertEqual(ev["name"], "")

    def test_decode_config_saved(self):
        ev = P.decode_event(bytes([P.EV_CONFIG_SAVED, 1, 0, 0, 0]))
        self.assertEqual(ev["type_name"], "CONFIG_SAVED")
        self.assertEqual(ev["active"], 1)

    def test_decode_utf8_chinese_name(self):
        name = "语音"
        data = bytes([P.EV_BOOT, 0, 0, 0, len(name.encode())]) + name.encode()
        self.assertEqual(P.decode_event(data)["name"], "语音")

    def test_decode_rejects_too_short(self):
        with self.assertRaises(ProtocolError):
            P.decode_event(b"\x00\x01")

    def test_decode_clamps_bogus_name_len(self):
        ev = P.decode_event(bytes([0, 0, 0, 0, 200]) + b"abc")
        self.assertEqual(ev["name"], "abc")


if __name__ == "__main__":
    unittest.main()
