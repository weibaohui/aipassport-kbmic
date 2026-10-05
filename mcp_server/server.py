#!/usr/bin/env python3
"""AI小键盘(kbmic)配置 MCP 服务 —— stdio。

设计约束:
  * import 本模块**不碰蓝牙**。bleak 只在真正调用工具时才被 import,
    所以这个文件可以被静态检查、可以被单元测试导入、可以先起起来再插硬件。
  * 所有写操作都是「读 → 改内存 → 校验 → 写全 10 片 → 读回比对」,
    因为固件收齐 10 片才提交,只写一部分等于没写。
  * 设备不可达时返回可操作的排查提示,而不是裸异常。

独立自检(不碰蓝牙):
    python3 server.py --selftest
"""

from __future__ import annotations

import asyncio
import contextlib
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import catalog  # noqa: E402
import protocol as P  # noqa: E402
from protocol import ProtocolError  # noqa: E402

try:  # mcp 1.x 把这个类叫 FastMCP,2.x 改名成 MCPServer,两者的 @tool / run 一样
    from mcp.server.fastmcp import FastMCP as _Server
except Exception:  # noqa: BLE001
    try:
        from mcp.server.mcpserver import MCPServer as _Server
    except Exception as exc:  # pragma: no cover - 缺 mcp 包时给出安装指引
        raise SystemExit(
            "缺少可用的 mcp 包。请先安装依赖:\n"
            "  python3 -m pip install -r "
            + os.path.join(os.path.dirname(os.path.abspath(__file__)), "requirements.txt")
            + f"\n(原始错误: {type(exc).__name__}: {exc})"
        ) from exc

mcp = _Server("kbmic")

# ---------------------------------------------------------------------------
# 内部工具函数
# ---------------------------------------------------------------------------

def _require_ble():
    """延迟 import:import server 时不会碰 bleak。"""
    import ble  # noqa: PLC0415
    return ble


def _fail(exc: Exception) -> dict:
    """把异常翻成 MCP 能显示的结构化返回值。"""
    return {"ok": False, "error": str(exc), "error_type": type(exc).__name__}


def _diff(before: P.Config, after: P.Config) -> list[str]:
    """读回比对:列出仍然不一致的字段,避免"以为写成功了"。"""
    notes: list[str] = []
    if after.version != before.version:
        notes.append(f"version {before.version} -> {after.version}")
    if after.active != before.active:
        notes.append(f"active {before.active} -> {after.active}")
    if after.count != before.count:
        notes.append(f"count {before.count} -> {after.count}")
    for i, (a, b) in enumerate(zip(before.live_profiles(), after.live_profiles())):
        if a.name != b.name:
            notes.append(f"profile[{i}].name {a.name!r} -> {b.name!r}")
        for btn in range(P.BTN_COUNT):
            for slot in range(P.SLOT_COUNT):
                x, y = a.get_slot(btn, slot), b.get_slot(btn, slot)
                if P.action_name(x) != P.action_name(y):
                    notes.append(
                        f"profile[{i}] {P.BUTTON_NAMES[btn]}/{P.SLOT_NAMES[slot]}: "
                        f"{P.action_name(x)!r} -> {P.action_name(y)!r}"
                    )
    return notes


async def _read_config(address: str | None) -> P.Config:
    ble = _require_ble()
    async with ble.connected(address) as (client, _addr):
        return P.Config.decode(await ble.read_blob(client))


async def _mutate(address: str | None, mutate, summary: str) -> dict:
    """读 → 改 → 校验 → 写 → 读回比对。mutate 收到 Config,就地修改。"""
    ble = _require_ble()
    before = await _read_config(address)
    after = before.copy()
    detail = mutate(after)  # 校验失败会在这里抛 ProtocolError,不会碰到设备
    after.validate()
    blob = after.encode()

    async with ble.connected(address) as (client, addr):
        await ble.write_blob(client, blob)
        readback = P.Config.decode(await ble.read_blob(client))

    notes = _diff(after, readback)
    result = {
        "ok": not notes,
        "summary": summary,
        "device": addr,
        "verified": not notes,
        "bytes_written": len(blob),
        "chunks_written": P.CHUNK_COUNT,
        "active": readback.active,
        "count": readback.count,
    }
    if isinstance(detail, dict):
        result.update(detail)
    if notes:
        result["mismatch"] = notes
    return result


# ---------------------------------------------------------------------------
# 1. 扫描设备
# ---------------------------------------------------------------------------

@mcp.tool()
async def kbmic_list_devices(timeout: float = 10.0, pin: str | None = None) -> dict:
    """扫描附近的「AI小键盘」设备,返回地址/名字/信号强度。

    把结果里的 address 传给 pin(或直接传给其它工具)可以固定这台设备,
    之后就不必每次都扫描。

    Args:
        timeout: 扫描秒数,默认 10。
        pin: 设备地址(Mac 风格 AA:BB:CC:DD:EE:FF)。传 None 表示用已固定的地址。
    """
    try:
        ble = _require_ble()
        if pin is not None:
            ble.set_pinned_address(pin)
        rows = await ble.scan_devices(timeout=timeout, pin=True)
        return {
            "ok": True,
            "expected_name": P.DEVICE_NAME,
            "pinned": ble.get_pinned_address(),
            "devices": rows,
            "hint": "把 address 传给后续工具的 address 参数即可固定设备。",
        }
    except Exception as exc:  # noqa: BLE001
        return _fail(exc)


# ---------------------------------------------------------------------------
# 2. 读整份配置
# ---------------------------------------------------------------------------

@mcp.tool()
async def kbmic_get_config(address: str | None = None) -> dict:
    """读取设备完整配置(10 个分片拼成 1580 字节后解析)。

    返回每个模式的完整按键表:三个键 × (短按/长按) × 最多 4 步,
    每个动作都带一个由步骤反推出来的可读名字(如 "Ctrl+Win (hold)")。
    """
    try:
        cfg = await _read_config(address)
        data = cfg.to_dict()
        data["ok"] = True
        data["service_uuid"] = P.SERVICE_UUID
        data["bytes"] = P.CONFIG_SIZE
        for prof, raw in zip(cfg.live_profiles(), data["profiles"]):
            for label, slot in _iter_slots(prof):
                raw["buttons"][label[0]][label[1]]["catalog_id"] = catalog.match_catalog(slot)
        return data
    except Exception as exc:  # noqa: BLE001
        return _fail(exc)


def _iter_slots(prof: P.Profile):
    slots = prof._empty_slots()
    for b in range(P.BTN_COUNT):
        for s in range(P.SLOT_COUNT):
            yield (P.BUTTON_NAMES[b], P.SLOT_NAMES[s]), slots[b][s]


# ---------------------------------------------------------------------------
# 3. 紧凑视图:每个键现在干什么
# ---------------------------------------------------------------------------

@mcp.tool()
async def kbmic_list_modes(address: str | None = None) -> dict:
    """紧凑列出所有模式:索引、名字、是否内置、三个键各自的动作名(短按/长按)。"""
    try:
        cfg = await _read_config(address)
        rows = []
        for row, prof in zip(cfg.modes_view(), cfg.live_profiles()):
            row["catalog_id"] = {
                label: catalog.match_catalog(slot)
                for label, slot in _iter_slots(prof)
                if label[1] == P.SLOT_NAMES[P.SLOT_TAP]
            }
            rows.append(row)
        return {"ok": True, "active": cfg.active, "count": cfg.count, "modes": rows}
    except Exception as exc:  # noqa: BLE001
        return _fail(exc)


# ---------------------------------------------------------------------------
# 4. 切当前模式
# ---------------------------------------------------------------------------

@mcp.tool()
async def kbmic_set_active_mode(index: int, address: str | None = None) -> dict:
    """设定当前生效的模式索引。

    Args:
        index: 模式索引,必须小于当前模式数量。
    """
    try:
        def mutate(cfg: P.Config) -> dict:
            cfg.set_active(index)
            return {"mode_name": cfg.check_index(index).name}
        return await _mutate(address, mutate, f"当前模式切到 #{index}")
    except Exception as exc:  # noqa: BLE001
        return _fail(exc)


# ---------------------------------------------------------------------------
# 5. 新增模式
# ---------------------------------------------------------------------------

@mcp.tool()
async def kbmic_add_mode(name: str | None = None, address: str | None = None) -> dict:
    """新增一个用户模式,返回新索引。

    键位默认从**当前模式**复制一份(与固件行为一致,这里也在写出的字节里显式完成),
    所以新模式立刻可用,再逐个键改即可。

    Args:
        name: 模式名,UTF-8 最多 15 字节;不给则用「自定义」。超长会自动截断。
    """
    try:
        def mutate(cfg: P.Config) -> dict:
            src_name = cfg.check_index(cfg.active).name
            idx = cfg.add_profile(name or "自定义")
            return {"index": idx, "name": cfg.profiles[idx].name,
                    "copied_from": src_name}
        return await _mutate(address, mutate, f"新增模式「{name or '自定义'}」")
    except Exception as exc:  # noqa: BLE001
        return _fail(exc)


# ---------------------------------------------------------------------------
# 6. 删除模式
# ---------------------------------------------------------------------------

@mcp.tool()
async def kbmic_delete_mode(index: int, address: str | None = None) -> dict:
    """删除一个用户模式(内置模式会拒绝)。

    Args:
        index: 要删除的模式索引,必须 >= 4 且小于模式数量。
    """
    try:
        def mutate(cfg: P.Config) -> dict:
            name = cfg.check_index(index).name
            cfg.delete_profile(index)   # 内置模式在这里被拒
            return {"deleted": index, "deleted_name": name}
        return await _mutate(address, mutate, f"删除模式 #{index}")
    except Exception as exc:  # noqa: BLE001
        return _fail(exc)


# ---------------------------------------------------------------------------
# 7. 改名
# ---------------------------------------------------------------------------

@mcp.tool()
async def kbmic_rename_mode(index: int, name: str, address: str | None = None) -> dict:
    """给模式改名(UTF-8,最多 15 字节)。

    Args:
        index: 模式索引。
        name: 新名字;超长会直接报错,不做静默截断(截断会把中文切碎)。
    """
    try:
        encoded = P.encode_name(name)  # 先在内存里校验,不合格就不碰设备
        del encoded

        def mutate(cfg: P.Config) -> dict:
            prof = cfg.check_index(index)
            prof.name = name
            return {"index": index, "name": name}
        return await _mutate(address, mutate, f"模式 #{index} 改名为「{name}」")
    except Exception as exc:  # noqa: BLE001
        return _fail(exc)


# ---------------------------------------------------------------------------
# 8. 设键(核心)
# ---------------------------------------------------------------------------

@mcp.tool()
async def kbmic_set_key(
    index: int,
    button: str | int,
    slot: str | int = "short-tap",
    preset: str | int | None = None,
    trigger: str | int | None = None,
    steps: list[dict] | None = None,
    address: str | None = None,
) -> dict:
    """把某个模式的某个键设成一个动作。

    两种写法二选一:
      * preset="Enter" / preset=6 —— 用内置动作目录里的预设;
      * steps=[{...}] —— 自定义步骤序列(最多 4 步)。

    步骤字段: kind(0=none 1=key 2=consumer 3=delay,或 "key"/"consumer"/"delay"),
    mods(0..7 的位掩码,或 "Ctrl+Shift" 这样的字符串), keycode(HID 用法码),
    usage(16 位 Consumer 用法码), delay_ms(毫秒)。

    Args:
        index: 模式索引。
        button: Up / Down / OK(或 0/1/2)。
        slot: short-tap / long-press(或 0/1),默认短按。
        preset: 目录里的动作 id 或名字,与 steps 互斥。
        trigger: 覆盖预设的触发方式(none/click/tap/long);仅在用 preset 或 steps 时有效。
        steps: 自定义步骤数组,与 preset 互斥。
        address: 设备地址,不传则用已固定的地址。
    """
    try:
        if preset is not None and steps is not None:
            raise ProtocolError("preset 与 steps 只能给一个(要么用目录预设,要么给自定义步骤)")
        b = P.parse_button(button)
        s = P.parse_slot(slot)

        if steps is not None:
            if len(steps) > P.SEQ_MAX:
                raise ProtocolError(
                    f"一个动作最多 {P.SEQ_MAX} 步,收到 {len(steps)} 步"
                )
            action = P.Action.from_dict(
                {"trigger": trigger if trigger is not None else P.TRIGGER_CLICK,
                 "steps": steps}
            )
            source = "custom"
        elif preset is not None:
            item = catalog.lookup(preset)
            action = item.action.copy()
            if trigger is not None:
                action.trigger = P.parse_trigger(trigger)
            source = f"preset {item.id} ({item.name})"
        else:
            raise ProtocolError(
                "必须给 preset(目录里的 id 或名字)或 steps(自定义步骤),二选一"
            )

        action.validate()
        shown = P.action_text(action)

        def mutate(cfg: P.Config) -> dict:
            prof = cfg.check_index(index)
            prof.set_slot(b, s, action)
            return {
                "index": index,
                "mode_name": prof.name,
                "button": P.BUTTON_NAMES[b],
                "slot": P.SLOT_NAMES[s],
                "action": action.to_dict(),
            }
        return await _mutate(
            address, mutate,
            f"模式 #{index} 的 {P.BUTTON_NAMES[b]}/{P.SLOT_NAMES[s]} 设为 {shown}",
        ) | {"source": source}
    except Exception as exc:  # noqa: BLE001
        return _fail(exc)


# ---------------------------------------------------------------------------
# 9. 单个内置模式恢复出厂
# ---------------------------------------------------------------------------

@mcp.tool()
async def kbmic_reset_mode(index: int, address: str | None = None) -> dict:
    """把一个内置模式(0..3)恢复成出厂默认按键。

    这里的默认值是固件 kbmic_config_defaults() 的镜像(见 catalog.py),
    写回去的字节与固件自己重建的结果一致。用户模式没有"出厂默认",
    请改用 kbmic_delete_mode 删除后重建。
    """
    try:
        default = catalog.builtin_profile_defaults(index)

        def mutate(cfg: P.Config) -> dict:
            prof = cfg.check_index(index)
            prof.name = default.name
            prof.builtin = 1
            for b in range(P.BTN_COUNT):
                for sl in range(P.SLOT_COUNT):
                    prof.set_slot(b, sl, default.get_slot(b, sl).copy())
            return {"index": index, "name": default.name, "mirrors_firmware": True}
        return await _mutate(address, mutate, f"模式 #{index} 恢复出厂默认")
    except Exception as exc:  # noqa: BLE001
        return _fail(exc)


# ---------------------------------------------------------------------------
# 10. 整机恢复出厂
# ---------------------------------------------------------------------------

@mcp.tool()
async def kbmic_reset_device(address: str | None = None) -> dict:
    """整台设备恢复出厂:4 个内置模式(Mac/Windows/Android/iOS),当前模式 = 0。

    会清掉所有用户模式,不可撤销。
    """
    try:
        default = catalog.factory_default_config()

        def mutate(cfg: P.Config) -> dict:
            names = [p.name for p in default.live_profiles()]
            cfg.version = default.version
            cfg.active = 0
            cfg.count = P.BUILTIN_MODES
            cfg.profiles = [p.copy() for p in default.live_profiles()]
            while len(cfg.profiles) < P.MAX_PROFILES:
                cfg.profiles.append(P.Profile())
            return {"modes": names, "mirrors_firmware": True}
        return await _mutate(address, mutate, "整机恢复出厂默认(4 个内置模式,active=0)")
    except Exception as exc:  # noqa: BLE001
        return _fail(exc)


# ---------------------------------------------------------------------------
# 11. 动作目录
# ---------------------------------------------------------------------------

@mcp.tool()
async def kbmic_action_catalog() -> dict:
    """列出内置动作目录(16 项):id、名字、触发方式与步骤,可直接作为 preset 使用。"""
    return {
        "ok": True,
        "catalog": catalog.catalog_as_dicts(),
        "note": "id 与固件 main/kbmic_action.c 的 ACT_* 枚举同序,不要重排。",
    }


# ---------------------------------------------------------------------------
# 12. 事件订阅
# ---------------------------------------------------------------------------

_watch: dict = {"task": None, "events": [], "started_at": None, "error": None,
                "limit": 500}


async def _watch_worker(address: str | None, duration: float) -> None:
    ble = _require_ble()
    try:
        _watch["events"] = await ble.watch_events(address=address, duration=duration,
                                                  limit=_watch["limit"])
        _watch["error"] = None
    except Exception as exc:  # noqa: BLE001
        _watch["error"] = str(exc)
    finally:
        _watch["task"] = None


@mcp.tool()
async def kbmic_watch(action: str = "start", duration: float = 10.0,
                      address: str | None = None) -> dict:
    """订阅设备的 Event 特征,收集按键 / 配置保存 / 启动事件。

    action:
      * "start" —— 后台订阅 duration 秒,不阻塞其它工具;
      * "status" —— 返回已收集到的事件(不重新连设备);
      * "stop"  —— 立刻取消正在进行的订阅。

    事件字段:type(0=BOOT 1=CONFIG_SAVED 2=KEY)、active(当前模式索引)、
    aux(KEY 事件里是按钮索引)、name(模式名,可能为空)。
    """
    try:
        act = str(action).strip().lower()
        if act == "status":
            return {"ok": True, "watching": _watch["task"] is not None,
                    "events": list(_watch["events"]), "error": _watch["error"],
                    "count": len(_watch["events"])}
        if act == "stop":
            task = _watch["task"]
            if task is not None:
                task.cancel()
                with contextlib.suppress(asyncio.CancelledError, Exception):
                    await task
                _watch["task"] = None
                return {"ok": True, "stopped": True, "events": len(_watch["events"])}
            return {"ok": True, "stopped": False, "note": "当前没有进行中的订阅"}
        if act not in ("start", "collect"):
            raise ProtocolError("action 只能是 start / status / stop")
        if act == "collect":
            ble = _require_ble()
            _watch["events"] = []
            events = await ble.watch_events(address=address, duration=duration,
                                            limit=_watch["limit"])
            return {"ok": True, "count": len(events), "events": events}
        if _watch["task"] is not None:
            return {"ok": False, "error": "已经在订阅了,先用 action='stop' 取消",
                    "events": len(_watch["events"])}
        _require_ble()
        _watch["events"] = []
        _watch["started_at"] = time.time()
        _watch["task"] = asyncio.create_task(_watch_worker(address, duration))
        return {"ok": True, "watching": True, "duration": duration,
                "note": "订阅已在后台开始;稍后用 action='status' 取结果,"
                        "或 action='stop' 取消。"}
    except Exception as exc:  # noqa: BLE001
        return _fail(exc)


# ---------------------------------------------------------------------------
# 入口
# ---------------------------------------------------------------------------

def _selftest() -> int:
    """不碰蓝牙的自检:布局算术 + 出厂配置 1580 字节 + 目录一致。"""
    import json
    out = {"protocol": P.self_check(), "catalog": catalog.self_check()}
    print(json.dumps(out, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] in ("--selftest", "-s"):
        raise SystemExit(_selftest())
    mcp.run()  # stdio
