"""BLE 传输层:扫描、连接、分片读写、事件订阅。

本模块只依赖 bleak,且 bleak 的 import 本身是惰性的 —— 没装 bleak 时 `import ble`
照样成功,真正用之前才会给出可执行的报错。这样协议层与 MCP 工具层在没有硬件、
没有蓝牙栈的机器上也能被导入和测试。

固件不支持 GATT long read/write,所以这里一律用最普通的单次 read_gatt_char /
write_gatt_char,按 0..9 顺序一格一格搬,不使用 read_long / write_long /
Prepare Write 之类的长写辅助。
"""

from __future__ import annotations

import asyncio
import contextlib

import protocol as P
from protocol import ProtocolError

try:  # 惰性:真正的连接才需要 bleak
    from bleak import BleakClient, BleakScanner
    _BLEAK_IMPORT_ERROR: str | None = None
except Exception as exc:  # pragma: no cover - 只在没装 bleak 的机器上走到
    BleakClient = None  # type: ignore[assignment]
    BleakScanner = None  # type: ignore[assignment]
    _BLEAK_IMPORT_ERROR = f"{type(exc).__name__}: {exc}"


# 固定的设备地址(kbmic_list_devices 结果里可以 pin 住,后续工具就不用再扫)
PINNED_ADDRESS: str | None = None

# 扫描超时默认秒数。HID 设备的连接间隔通常在 5s 以内,10s 比较稳。
DEFAULT_SCAN_TIMEOUT = 10.0

# 一次 BLE 操作的重试次数(macOS 上偶发的 "operation timed out" 是常客)
_OP_RETRIES = 2


class BleUnavailable(RuntimeError):
    """bleak 不可用 / 蓝牙不可用。消息必须是可执行的排查步骤。"""


class BleError(RuntimeError):
    """BLE 操作失败,已经把排查方向写进消息里。"""


# ---------------------------------------------------------------------------
# 错误翻译:所有对外抛出的 BLE 异常都带可操作提示
# ---------------------------------------------------------------------------

_PERMISSION_HINTS = (
    "\n排查顺序:\n"
    "  1) 设备是否上电、是否在信号范围内(离电脑 1 米内先试);\n"
    "  2) 是否已与这台电脑配对:macOS「系统设置 → 蓝牙」里应能看到 "
    f"「{P.DEVICE_NAME}」;先在系统里连一次再重试;\n"
    "  3) 蓝牙权限:macOS「系统设置 → 隐私与安全性 → 蓝牙」里允许运行本程序的"
    "终端 / IDE(Claude Code、Cursor、Terminal…);改完要**重启该程序**才生效;\n"
    "  4) 确认蓝牙已打开,且没有其它程序(比如另一个 MCP 客户端)正连着设备 —— "
    "HID 设备同一时间只接受一个主机连接。\n"
    "  Trivia: powered on? paired? Bluetooth permission granted? in range?"
)

_PERMISSION_KEYWORDS = (
    "unauthorized", "not authorized", "permission", "denied", "not permitted",
    "not allowed", "access is denied",
)


def _describe(exc: BaseException) -> str:
    """把底层异常翻成一句人话 + 排查清单。"""
    name = type(exc).__name__
    msg = str(exc).strip() or name
    low = msg.lower()
    if any(k in low for k in _PERMISSION_KEYWORDS) or ("bluetooth" in low and "unavailable" in low):
        head = ("蓝牙不可用:系统拒绝了本次访问,通常是没有授予蓝牙权限 "
                f"(底层报错: {name}: {msg})")
    elif "timed out" in low or "timeout" in low:
        head = (f"操作超时(底层报错: {name}: {msg})。"
                "设备可能已经休眠或被别的程序连走了")
    elif "not connected" in low or "disconnected" in low:
        head = f"设备已断开连接(底层报错: {name}: {msg})"
    elif "not found" in low or "no device" in low or "not available" in low:
        head = (f"找不到设备(底层报错: {name}: {msg})。"
                f"请确认设备已上电并完成配对,且名字是「{P.DEVICE_NAME}」")
    elif "invalidate" in low and "state" in low:
        head = ("设备已不在可连接状态(底层报错: " f"{name}: {msg})")
    else:
        head = f"BLE 操作失败(底层报错: {name}: {msg})"
    return head + _PERMISSION_HINTS


def require_bleak() -> None:
    if BleakScanner is None or BleakClient is None:
        raise BleUnavailable(
            "bleak 不可用,无法访问蓝牙(" + str(_BLEAK_IMPORT_ERROR) + ")。\n"
            "修复:pip install bleak(若在虚拟环境里,记得用同一个解释器)。"
        )


async def _retry(op, what: str):
    """带重试的一次 BLE 操作。bleak 在 macOS 上偶发超时不是罕见情况。"""
    last: BaseException | None = None
    for attempt in range(_OP_RETRIES + 1):
        try:
            return await op()
        except asyncio.CancelledError:
            raise
        except (ProtocolError, BleUnavailable):
            raise
        except Exception as exc:  # noqa: BLE001 - 这里就是要兜住 bleak 的所有异常
            last = exc
            if attempt < _OP_RETRIES:
                await asyncio.sleep(0.4 * (attempt + 1))
    raise BleError(f"{what} 失败。{_describe(last)}") from last


# ---------------------------------------------------------------------------
# 扫描
# ---------------------------------------------------------------------------

def set_pinned_address(address: str | None) -> None:
    """固定设备地址。传 None 取消固定,之后每次操作都会重新扫描。"""
    global PINNED_ADDRESS
    PINNED_ADDRESS = address


def get_pinned_address() -> str | None:
    return PINNED_ADDRESS


def _adv_name(adv) -> str | None:
    """广播名:local_name 优先,其次平台广播里的 Complete/Shortened Local Name。"""
    local = getattr(adv, "local_name", None)
    if local:
        return local
    data = getattr(adv, "service_data", None) or {}
    for payload in data.values():
        with contextlib.suppress(Exception):
            raw = bytes(payload)
            if raw and P.DEVICE_NAME.encode("utf-8") in raw:
                return P.DEVICE_NAME
    uuids = getattr(adv, "service_uuids", None) or []
    if P.SERVICE_UUID in uuids:
        return P.DEVICE_NAME
    return None


async def scan_devices(timeout: float = DEFAULT_SCAN_TIMEOUT,
                       pin: str | None | bool = None) -> list[dict]:
    """扫描并列出所有看起来是本设备的设备。

    pin=True 时优先只返回已固定的那台;也允许直接传地址字符串。
    """
    require_bleak()
    want_pin = PINNED_ADDRESS if pin is None else (pin if isinstance(pin, str) else None)

    def _do_scan():
        return BleakScanner.discover(timeout=timeout, return_adv=True)

    try:
        found = await _retry(_do_scan, "扫描蓝牙设备")
    except BleError as exc:
        raise BleError(
            "扫描蓝牙设备失败。" + _PERMISSION_HINTS
        ) from exc

    rows: list[dict] = []
    for device, adv in (found or {}).items():
        name = _adv_name(adv)
        if not name:
            continue
        try:
            local, rssi = device.name, adv.rssi
        except Exception:  # noqa: BLE001 - 广播数据随时可能过期
            local, rssi = name, None
        rows.append({
            "address": device.address,
            "name": name,
            "local_name": local,
            "rssi": rssi,
            "pinned": bool(want_pin) and device.address.upper() == want_pin.upper(),
            "service_uuid": P.SERVICE_UUID,
        })
    rows.sort(key=lambda r: (not r["pinned"], -(r["rssi"] if r["rssi"] is not None else -999)))
    if want_pin and not any(r["pinned"] for r in rows):
        raise BleError(
            f"扫描结果里没有已固定的设备 {want_pin}。"
            f"请确认那台设备还开着;或用 kbmic_list_devices(pin=\"设备地址\") 换一台。"
            + _PERMISSION_HINTS
        )
    return rows


async def _resolve_address(address: str | None) -> str:
    """有地址就用地址,否则扫一台出来。"""
    if address:
        return address
    if PINNED_ADDRESS:
        return PINNED_ADDRESS
    rows = await scan_devices(pin=True)
    if not rows:
        raise BleError(
            f"没有扫描到名为「{P.DEVICE_NAME}」的设备。" + _PERMISSION_HINTS
        )
    return rows[0]["address"]


# ---------------------------------------------------------------------------
# 连接
# ---------------------------------------------------------------------------

@contextlib.asynccontextmanager
async def connected(address: str | None = None, timeout: float = 20.0):
    """连上设备并保证断开。拿到的 client 只在块内有效。"""
    require_bleak()
    addr = await _resolve_address(address)
    client = BleakClient(addr, timeout=timeout)
    try:
        await _retry(lambda: client.connect(), f"连接设备 {addr}")
    except BleError as exc:
        with contextlib.suppress(Exception):
            await client.disconnect()
        raise BleError(
            f"连接设备 {addr} 失败。" + _PERMISSION_HINTS
        ) from exc
    if not client.is_connected:
        with contextlib.suppress(Exception):
            await client.disconnect()
        raise BleError(
            f"设备 {addr} 连上后立刻断开了(常见于另一台主机已连着它)。" + _PERMISSION_HINTS
        )
    try:
        yield client, addr
    finally:
        with contextlib.suppress(Exception):
            await client.disconnect()


async def _resolve_chars(client, address: str) -> tuple[list, object]:
    """把 10 个分片特征和一个事件特征找出来。找不到就报清楚缺哪个。"""
    missing: list[str] = []
    chunks = []
    for i in range(P.CHUNK_COUNT):
        uuid = P.chunk_uuid(i)
        chars = client.services.get_characteristics(uuid)
        if not chars:
            missing.append(uuid)
        else:
            chunks.append(chars[0])
    evt_chars = client.services.get_characteristics(P.EVENT_UUID)
    if missing or not evt_chars:
        if missing:
            raise BleError(
                f"设备 {address} 上找不到配置分片特征 {missing} —— "
                f"它广播了「{P.DEVICE_NAME}」但没有注册本 MCP 需要的自定义服务 "
                f"{P.SERVICE_UUID}。\n"
                "多半是固件版本不匹配:请用与本 MCP 服务端同一份固件重新烧录。"
            )
    return chunks, evt_chars[0]


# ---------------------------------------------------------------------------
# 读 / 写
# ---------------------------------------------------------------------------

async def read_blob(client) -> bytes:
    """按 0..9 顺序读全部分片并拼回 1580 字节。"""
    chunks, _evt = await _resolve_chars(client, getattr(client, "address", "?"))
    parts: list[bytes] = []
    for i, char in enumerate(chunks):
        data = await _retry(lambda c=char: client.read_gatt_char(c), f"读取分片 {i}")
        if not isinstance(data, (bytes, bytearray)):
            raise BleError(f"分片 {i} 返回了非字节数据: {type(data).__name__}")
        parts.append(bytes(data))
    return P.join_chunks(parts)


async def write_blob(client, blob: bytes) -> None:
    """写全 10 片(带响应),0..9 顺序。设备收齐后才会提交并落盘。"""
    if len(blob) != P.CONFIG_SIZE:
        raise ProtocolError(f"待写入的配置必须是 {P.CONFIG_SIZE} 字节,收到 {len(blob)} 字节")
    chunks, _evt = await _resolve_chars(client, getattr(client, "address", "?"))
    for i, payload in enumerate(P.split_chunks(blob)):
        await _retry(
            lambda c=chunks[i], d=payload: client.write_gatt_char(c, d, response=True),
            f"写入分片 {i}",
        )


async def read_config(address: str | None = None) -> "P.Config":
    async with connected(address) as (client, _addr):
        return P.Config.decode(await read_blob(client))


async def write_config(cfg: "P.Config", address: str | None = None) -> "P.Config":
    """校验 → 写全 10 片 → 读回确认。返回读回来的那份配置。"""
    cfg.validate()
    blob = cfg.encode()
    async with connected(address) as (client, _addr):
        await write_blob(client, blob)
        return P.Config.decode(await read_blob(client))


# ---------------------------------------------------------------------------
# 事件订阅
# ---------------------------------------------------------------------------

async def watch_events(address: str | None = None, duration: float = 10.0,
                       limit: int = 200) -> list[dict]:
    """订阅 Event 特征,收集 duration 秒内的事件后返回。"""
    require_bleak()
    events: list[dict] = []
    async with connected(address) as (client, _addr):
        _chunks, evt_char = await _resolve_chars(client, getattr(client, "address", "?"))

        def _on_notify(_sender, data: bytearray) -> None:
            if len(events) >= limit:
                return
            try:
                events.append(P.decode_event(bytes(data)))
            except ProtocolError as exc:
                events.append({"error": str(exc), "raw": bytes(data).hex()})

        await _retry(lambda: client.start_notify(evt_char, _on_notify), "订阅事件特征")
        try:
            await asyncio.sleep(max(0.1, float(duration)))
        finally:
            with contextlib.suppress(Exception):
                await client.stop_notify(evt_char)
    return events
