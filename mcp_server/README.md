<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# kbmic MCP server

An MCP (Model Context Protocol) server that configures the AI keypad over
Bluetooth Low Energy. It runs on your Mac, talks to the device over a custom
GATT service, and exposes twelve tools for reading and changing keyboard modes
and per-button key mappings.

The device advertises a fixed Chinese-language product name; the exact
characters are in the [Simplified Chinese README](README.zh_CN.md).

Use it to set up the device from an AI assistant instead of tapping through the
on-device menu — handy for modes the three buttons cannot reach.

> The Simplified Chinese README ([README.zh_CN.md](README.zh_CN.md)) is the more
> detailed one; this is the English summary of the same content.

## 1. Installing

```bash
cd ~/Desktop/aipassport-kbmic/mcp_server
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

`bleak` on macOS uses CoreBluetooth, which needs the **Bluetooth** permission
for whatever process actually talks to the device. If your MCP client is a
desktop app rather than a terminal process, macOS will prompt on first use —
click Allow, then restart the client. Without it you get
`BLE is unavailable` rather than a crash.

## 2. Self-check (no hardware required)

```bash
python3 -m unittest discover -s ~/Desktop/aipassport-kbmic/mcp_server -v
```

The unit tests cover the wire format, the codec, validation, and name
rendering. They run with no Bluetooth and no device present.

## 3. Registering as an MCP stdio server

```json
{
  "mcpServers": {
    "kbmic": {
      "command": "python3",
      "args": ["/Users/weibh/Desktop/aipassport-kbmic/mcp_server/server.py"]
    }
  }
}
```

With a virtualenv, point `command` at the interpreter directly:

```json
{
  "mcpServers": {
    "kbmic": {
      "command": "/Users/weibh/Desktop/aipassport-kbmic/mcp_server/.venv/bin/python",
      "args": ["/Users/weibh/Desktop/aipassport-kbmic/mcp_server/server.py"]
    }
  }
}
```

Restart the client afterwards. To confirm the link works, ask it to list the
device's modes.

## 4. Tools

| Tool | Purpose |
| --- | --- |
| `kbmic_list_devices` | Scan and return address / name / RSSI; can pin a device |
| `kbmic_get_config` | Read and decode the whole configuration |
| `kbmic_list_modes` | Compact view: one row per mode, what each of the three buttons does |
| `kbmic_set_active_mode` | Choose the active mode by index |
| `kbmic_add_mode` | Add a user mode (key mappings copied from the current one) |
| `kbmic_delete_mode` | Remove a user mode; built-in modes are refused |
| `kbmic_rename_mode` | Rename a mode (UTF-8, max 15 bytes) |
| `kbmic_set_key` | **Core tool**: set the action for one (mode, button, slot) |
| `kbmic_reset_mode` | Restore one built-in mode to factory defaults |
| `kbmic_reset_device` | Factory-reset the device (4 built-in modes, active = 0) |
| `kbmic_action_catalog` | List the 16 built-in action presets |
| `kbmic_watch` | Subscribe to device events |

Every tool returns a dict with an `ok` field; failures carry `error` text.
All but `kbmic_list_devices` and `kbmic_action_catalog` accept an optional
`address`.

Examples:

```
kbmic_list_devices()
kbmic_list_modes()
kbmic_set_key(index=0, button="OK", slot="long-press", preset="Globe")
kbmic_set_key(index=1, button="Up", slot="short-tap", trigger="click",
              steps=[{"kind": "key", "mods": "Ctrl+Shift", "keycode": 0x41}])
kbmic_add_mode(name="Meeting")
kbmic_set_active_mode(index=4)
kbmic_delete_mode(index=0)          # refused: built-in mode
```

## 5. Built-in action catalog

Ids match the `ACT_*` enum order in the firmware's `main/kbmic_action.c`; never
reorder one without the other. Reference presets by id or by name
(case-insensitive).

| id | name | trigger | action |
| --- | --- | --- | --- |
| 0 | `-` | none | empty |
| 1 | Enter | click | 1 × KEY(0x28) |
| 2 | Back | click | 1 × KEY(0x2A) |
| 3 | Tab | click | 1 × KEY(0x2B) |
| 4 | Space | click | 1 × KEY(0x2C) |
| 5 | Esc | click | 1 × KEY(0x29) |
| 6 | Globe | tap (hold) | 1 × CONSUMER(0x029D) |
| 7 | Ctrl+Win | tap (hold) | 1 × KEY(mods=Ctrl\|Win, keycode=0) |
| 8 | Up | click | 1 × KEY(0x52) |
| 9 | Down | click | 1 × KEY(0x51) |
| 10 | Left | click | 1 × KEY(0x50) |
| 11 | Right | click | 1 × KEY(0x4F) |
| 12 | Enter x3 | click | 3 × KEY(0x28) |
| 13 | F1 | click | 1 × KEY(0x3A) |
| 14 | F2 | click | 1 × KEY(0x3B) |
| 15 | Settings | none | software action: opens the on-device menu, sends no HID report |

## 6. Custom steps

`kbmic_set_key` accepts a `steps` array of at most 4 steps, 7 bytes each:

| Field | Meaning |
| --- | --- |
| `kind` | `0/none`, `1/key`, `2/consumer`, `3/delay` |
| `mods` | Bit mask, or a string like `"Ctrl+Shift"`. bit0=Ctrl bit1=Shift bit2=Alt bit3=Win |
| `keycode` | HID keyboard usage code (Enter 0x28, Esc 0x29, Backspace 0x2A, Tab 0x2B, Space 0x2C, A–Z 0x04–0x1D, digits 0x1E–0x27, F1–F12 0x3A–0x45) |
| `usage` | 16-bit Consumer usage (Globe = 0x029D) |
| `delay_ms` | Delay in milliseconds |

Example — Ctrl+Shift+A, wait 80 ms, then Enter:

```json
{"trigger": "click", "steps": [
  {"kind": "key", "mods": "Ctrl+Shift", "keycode": 0x04},
  {"kind": "delay", "delay_ms": 80},
  {"kind": "key", "keycode": 0x28}
]}
```

Renders as `Ctrl+Shift+A +80ms Enter`.

### Display-name rules

Action names are **not stored**; they are rendered from the steps on demand, and
the firmware's `kbmic_action_name()` must produce the same strings:

- `KEY`: `Ctrl+Shift+Alt+Win+` prefixes in that order plus the key name. A
  modifiers-only step drops the trailing `+`, so `Ctrl+Win`. Unknown keycodes
  degrade to `0x%02X`.
- `CONSUMER`: `0x029D` renders as `Globe`, anything else as `C:0x%04X`.
- `DELAY`: `+%dms`.
- Multiple steps joined by a single space.
- Tap (press-and-hold) actions get ` (hold)` appended.
- An empty action or `trigger=none` renders as `-`.
- `kind=none` with the high bit of `mods` set is the `Settings` software action.

## 7. Wire protocol (the contract with the firmware)

```
Service        7d1c5a30-9f6e-4a21-8c3d-2b5e7a9f1c48
Config chunk 0 7d1c5a40-…  read / write      10 chunks total; the first 9 are 160 bytes
Config chunk 9 7d1c5a49-…  read / write      the tenth is 140 bytes
Event          7d1c5a4f-…  read / notify
```

- The firmware has **no GATT long read/write**, so the server always uses plain
  single read/write calls and reassembles the chunks itself.
- Writes use `response=True`, one call per chunk, in order 0..9.
- The device commits and saves only after all 10 chunks arrive and pass
  validation, so "write the config" is never a partial state.
- After writing, the server reads the config back and compares; a mismatch comes
  back as `verified: false` plus a `mismatch` list.

Config layout (little-endian, packed, 1580 bytes total):

```
0    1  version  = 1
1    1  active   active mode index
2    1  count    number of modes, 1..8
3    1  reserved
4  1576 profiles[8]     197 bytes each
```

A profile is a 16-byte UTF-8 name (NUL-terminated, 15 bytes of content) + a
1-byte builtin flag + `slots[3][2]` (3 buttons × 2 slots). A slot is 30 bytes
(trigger + step count + 4 × 7-byte steps).

Event payload: `type(1) + active(1) + aux(1) + reserved(1) + name_len(1) + name(n)`,
where `type` is 0=BOOT, 1=CONFIG_SAVED, 2=KEY (`aux` is the button index for KEY).

## 8. Files

```
mcp_server/
├── protocol.py       wire format: constants, offsets, codec, validation, names, chunks, events
├── catalog.py        16 built-in presets + factory defaults for the 4 built-in modes
├── ble.py            BLE transport: scan, connect, chunked read/write, event subscription
├── server.py         MCP stdio server and the 12 tools (touches no Bluetooth at import)
├── test_protocol.py  unit tests (stdlib unittest, no hardware needed)
├── requirements.txt  dependencies
└── README.md         this file
```

Changing any layout or catalog entry means changing the firmware's
`main/kbmic_config.h` and `main/kbmic_action.c` in the same commit — otherwise
you will write a configuration the device reads as garbage. Run the unit tests
afterwards.
