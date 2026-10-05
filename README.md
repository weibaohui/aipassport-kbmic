<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AI Keypad (AI Passport firmware)

Turns the [FoloToy AI Passport](https://github.com/FoloToy/ai-passport) handheld (ESP32-C3, 2.4″ ST7789 240×320, three buttons) into a **configurable Bluetooth voice-input remote**: hold the OK button and the computer's or phone's own voice input starts; release when you're done and the text lands right at the cursor.

The Bluetooth device name is a fixed Chinese-language product name; the exact characters are in the [Simplified Chinese README](README.zh_CN.md).

Built on the [aipassport-fw](https://github.com/weibaohui/aipassport-fw) framework (mounted as a git submodule at `components/framework`). Only its `bsp` layer is used (display/LVGL, buttons, battery) — the `appfw` networking path is deliberately left out.

## Why recognition runs on the host, not the device

The ESP32-C3 has Bluetooth LE only, with no Classic Bluetooth (BR/EDR), so the HFP hands-free path doesn't exist in hardware. `SOC_BLE_ISO_SUPPORTED` is absent across the whole family, which rules out LC3 over LE Audio too. **It cannot present itself to the OS as a Bluetooth microphone.**

So the job splits cleanly:

| | Done by |
| --- | --- |
| Microphone capture, speech recognition, punctuation, candidate words | The computer's / phone's own IME |
| Triggering voice input, confirm, delete, arbitrary keystrokes | This device (a BLE HID keyboard) |
| On-device recording and ASR | Not involved |

Recognition runs locally, adds no latency, and needs **no app, no network, and no account**.

## Keyboard modes and key definitions

The device ships **4 built-in modes plus up to 4 user-created modes** (8 total). In each mode all three buttons are configurable, and each button has two slots:

- **Short tap** — fires on release, or fires on press and holds until release (that's how voice is configured)
- **Long press** — fires after 500 ms

An action is **up to 4 steps**, each being a keystroke with modifiers (`Ctrl+Win`), a Consumer usage (`Globe`), or a delay. So "Enter Enter Enter" and "Ctrl+Shift+A, wait 80 ms, Enter" are both expressible.

### Built-in modes

| Mode | OK short tap | Up / Down |
| --- | --- | --- |
| **Mac** | Globe (Fn) key | Enter / Backspace |
| **Windows** | `Ctrl+Win` | Enter / Backspace |
| **Android** | Long-press space | Enter / Backspace |
| **iOS** | Globe (**not verified on hardware**) | Enter / Backspace |

All four share the same Up/Down keys. Up long-press fires three Enters; Down long-press and OK long-press both default to opening the settings menu.

> macOS uses Consumer `0x029D` rather than **impersonating Apple's VID/PID**. The latter — privatising byte 2 of a 6KRO report as `FF00h/03h` — is a community QMK trick that is known to work, but it carries trademark/MFi risk and isn't advisable for a commercial product.
>
> The Windows and Android profiles send **plain modifiers and keys**, so the shortcut can still be remapped inside the host IME. WeChat's voice shortcut is configurable, which means you don't need a firmware change if the default is wrong for you.
>
> iOS has no documented third-party hardware-key global voice input interface. That profile just reuses the Globe report as an attempt, and it stays fully editable. If it doesn't work, remap it over MCP.

## The interface

Five screens, all driven by the three buttons.

**Home** — three rows, one per button, each showing what that button currently sends:

```
┌────────────────────────────┐
│ AI Keypad          ● linked │
│  ┌──────────────────────┐  │
│  │    hold to speak     │  │   panel turns green while OK is held
│  └──────────────────────┘  │
│  mode Mac          87%     │
│                            │
│  UP    Enter               │   ← these three lines follow the
│  DOWN  Back                │     configuration immediately
│  OK    Globe (hold)        │
│                            │
│  hold OK for settings      │
└────────────────────────────┘
```

**Settings menu** (hold OK): `Keyboard mode` / `Key mapping` / `Restore defaults`.

**Mode list**: every mode, tagged built-in or custom, the active one tagged in use. OK selects, applies, and saves immediately.

**Key mapping**: six rows = 3 buttons × 2 slots, each showing its current action name.

**Action picker**: 16 built-in presets (Enter, Backspace, Tab, Space, Esc, Globe, Ctrl+Win, arrows, F1/F2, Enter ×3, open settings, …).

Navigation: Up/Down move the cursor, OK selects, **hold OK to go back**.

> Mode names may be Chinese (UTF-8, 15 bytes max). The UI font embeds the 3500 level-1 characters of the standard Chinese character set, so names you invent never render as empty boxes.

## Configuring over MCP (recommended)

The device exposes a custom GATT configuration service. Run the MCP server on your Mac and you can read and write it without touching the device's buttons.

```
kbmic_list_devices()                       # find the device
kbmic_list_modes()                         # what does each key do right now
kbmic_set_key(index=0, button="OK", slot="long-press", preset="Globe")
kbmic_set_key(index=1, button="Up", slot="short-tap", trigger="click",
              steps=[{"kind": "key", "mods": "Ctrl+Shift", "keycode": 0x41}])
kbmic_add_mode(name="Meeting")
kbmic_set_active_mode(index=4)
kbmic_delete_mode(index=0)                 # refused: built-in mode
```

Twelve tools in total (list modes, add/remove modes, change keys, factory reset, subscribe to events, …). See [mcp_server/README.md](mcp_server/README.md) for the full list and the registration snippet.

<details>
<summary>Install it yourself</summary>

```bash
cd ~/Desktop/aipassport-kbmic/mcp_server
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
python3 -m unittest discover -s . -v      # 62 tests, no hardware needed
```

MCP client config:

```json
{ "mcpServers": { "kbmic": {
  "command": "/Users/weibh/Desktop/aipassport-kbmic/mcp_server/.venv/bin/python",
  "args": ["/Users/weibh/Desktop/aipassport-kbmic/mcp_server/server.py"] } } }
```

</details>

### Wire protocol at a glance

```
Service        7d1c5a30-9f6e-4a21-8c3d-2b5e7a9f1c48
Config chunk 0 7d1c5a40-…  read / write     10 chunks; the first 9 are 160 bytes
Config chunk 9 7d1c5a49-…  read / write     the tenth is 140 bytes
Event          7d1c5a4f-…  read / notify
```

The whole configuration is 1580 bytes, little-endian, packed, with no padding. **The chunking exists because IDF 5.5's GATT server has no public long-read/long-write entry point** — rather than depend on internal behaviour, the transport uses plain single reads and writes, which behave identically across macOS, iOS, and Android. The device commits and saves only after all 10 chunks arrive and validate, so there is no half-written state; afterwards the server reads the config back and compares.

## Pairing

Pick the device by its Bluetooth name in the host's settings the first time. It has no numeric keypad, so if the host asks you to type a passkey, the firmware prints it to the serial log.

## Flashing

```bash
cd ~/Desktop/aipassport-kbmic
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.13_env"  # required first in a non-interactive shell
source ~/esp/esp-idf/export.sh
idf.py build
idf.py flash
```

After a passing gate run, `build/firmware/<sha256>/` keeps a checksummed archive, and the merged image lands at `build/FoloToy-AI-Passport-full.bin` (write the whole thing from `0x0`; this resets the stored configuration).

## Build and test

```bash
./tools/validate.sh              # everything: static checks + host tests + firmware build and verification
./tools/validate.sh --static     # static checks and host tests only (no ESP-IDF needed)
./tools/validate.sh --firmware   # firmware build only
```

Three test layers, each guarding something different:

| Test | What it protects |
| --- | --- |
| `tests/test_kbmic_model.c` | Factory defaults, validation, mode add/delete, **action-name rendering** |
| `tests/test_ui_charset.py` | Every non-ASCII character in the UI is inside the font subset |
| `mcp_server/test_protocol.py` | Wire codec, validation, rejection paths (62 cases, no hardware) |

Action-name rendering exists twice — once in C, once in Python — and the two must agree character for character, otherwise the same thing shows up under different names on the device screen and in the MCP reply. `test_kbmic_model.c` exists for exactly that.

After changing UI copy, regenerate the font:

```bash
python3 tools/gen_font_charset.py
npx lv_font_conv@1.5.3 --font assets/fonts/NotoSansSC-Regular.otf \
  --size 16 --bpp 4 --format lvgl --no-compress \
  --lv-font-name app_font_16 --lv-include lvgl.h \
  --symbols "$(cat assets/fonts/kbmic_charset.txt)" \
  --output assets/fonts/app_font_16.c
```

Asset and font conventions are in [assets/README.md](assets/README.md).

## Layout

```
main/
  main.c          startup wiring, key queue, five-screen state machine
  kbmic_config.h  configuration model and wire structures (pure C, no ESP-IDF dependency)
  kbmic_model.c   factory defaults, validation, mode add/delete, action-name rendering
  kbmic_store.c   NVS persistence and the live configuration copy
  kbmic_action.c  action executor + 16 built-in presets
  kbmic_hid.c     BLE HID: two report maps (keyboard + Consumer), thin GAP layer, pairing
  kbmic_ble_svc.c configuration service: chunked I/O, staged commit, event notification
  kbmic_ui.c      five-screen renderer
assets/fonts/     3500-character table, source typeface, generated font, licence
mcp_server/       MCP server (protocol / catalog / BLE / tools / tests)
tools/            gate wrapper, symbol-table generation
tests/            firmware-side host tests and glyph coverage gate
```

## Needs on-device verification

A clean build, a green gate, and a byte-exact cross-check of the protocol on both sides still do not prove the chain works. These **must be checked on real hardware**:

1. On macOS, does holding the OK button bring up WeChat's voice input? (the Consumer `0x029D`, option A approach)
2. On Android, does a long-press of space from a BLE hardware keyboard reach the WeChat IME?
3. Does the iOS profile work at all?
4. Can MCP actually reach the device under real macOS CoreBluetooth permissions and complete a read/write round trip?

If (1) fails, the fallback is to remap the WeChat shortcut to something ordinary like `Ctrl+Option+V` — the firmware already supports arbitrary combinations, so no rebuild is needed.
