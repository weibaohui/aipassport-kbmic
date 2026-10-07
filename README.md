<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AI Keypad (AI Passport firmware)

Turns a [FoloToy AI Passport](https://github.com/FoloToy/ai-passport) handheld (ESP32-C3, 2.4″ 240×320 display, three buttons, 8 MB flash, no PSRAM) into a configurable BLE HID keypad. The verified default layout is optimized for dictation: hold the up key to press Apple Fn on the connected Mac, release it to stop; use Enter, Escape, and Backspace without reaching for the keyboard.

The Bluetooth device name is a fixed Chinese product name documented in the Simplified Chinese README. No mobile app is required.

The application uses the [aipassport-fw](https://github.com/weibaohui/aipassport-fw) framework as a git submodule at `components/framework`. It uses BSP for the ADC buttons, display, battery, and LVGL locking; uses `appfw_ui` for the settings shell and full key lifecycle; and optionally uses `appfw` networking when saved Wi-Fi is available and the heap has enough headroom.

## Why recognition runs on the host

ESP32-C3 has BLE only. It cannot expose itself to the host as a Bluetooth microphone through HFP or LE Audio. The device therefore acts as a keyboard:

| Responsibility | Where it runs |
| --- | --- |
| Microphone capture, ASR, punctuation, candidates | Host OS / input method |
| Triggering voice input, Enter, Escape, Backspace, custom shortcuts | This BLE HID keyboard |
| On-device recording and ASR | Not used |

The normal dictation path needs no app, account, or network. Optional Wi-Fi/HTTP configuration is separate and never required for BLE keys.

## Default keys

There are four built-in profiles—Mac, Windows, Android, and iOS—plus room for four user profiles. All built-in profiles start with the same physical key behavior:

| Key | Action |
| --- | --- |
| Up long press | Apple Fn down; release sends Fn up |
| Down short press | Enter |
| Down double click | Escape |
| OK short press | Backspace |
| OK long press | Open the on-device settings menu |
| Up short/double, Down long, OK double | No action |

The up-key action is the Apple Top Case Fn usage in byte 1 of the standard 8-byte keyboard report. Hosts decide what that key means; macOS voice input is the verified use case. Windows/Android/iOS behavior can differ by host, and each profile remains independently customizable through MCP, HTTP, or the on-device UI.

## Configuration model

- Versions: v3 wire protocol, with an in-place v2 migration for NVS data.
- Profiles: 4 built-in + up to 4 user profiles, 8 total.
- Slots: 3 per button—short press, double click, and long press.
- Actions: up to 4 steps.
- Step types: keyboard key with modifiers, Consumer Control usage, delay, or Apple Fn.
- Triggers: none, click, tap/hold, long press, or double click.
- Storage: packed little-endian configuration persisted in NVS.

Action names are generated in both C and Python and must match byte for byte; host tests enforce this.

## Device UI

The 240×320 home page shows three key cards, the active profile, BLE connection state, battery level, speaking/holding state, and the latest key feedback.

Long-press OK opens the framework settings menu, which includes screen timeout, brightness, device info, provisioning, and the Button modes entry. That application page has three views:

1. **Profile list**—select a profile or create a user profile.
2. **Key mapping**—3 buttons × 3 slots.
3. **Action picker**—17 built-in presets, including Enter, Backspace, Tab, Space, Escape, Globe, Ctrl+Win, arrows, F1/F2, Enter ×3, settings, and Apple Fn.

Up/Down move the cursor and OK selects. A `< Back` row moves up one level; long press OK exits the keyboard settings view. The on-device profile list is read-only for creation; use MCP or AI to add a profile. Chinese profile names are supported by the framework's GB2312-coverage 16 px font.

## Configuration channels

### BLE GATT (primary)

A custom GATT service is always registered and supports offline configuration from `mcp_server/` on a desktop:

```text
Service        7d1c5a30-9f6e-4a21-8c3d-2b5e7a9f1c48
Config chunks  7d1c5a40-… through 7d1c5a4e-…, read/write
Event          7d1c5a4f-…, read/notify
```

The configuration is 2300 bytes: 14 chunks of 160 bytes plus a final 60-byte chunk. The client writes all 15 chunks; the device validates and commits only a complete configuration, then persists it in NVS.

Install and run the desktop MCP bridge:

```bash
cd mcp_server
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python -m unittest discover -s . -v
```

Point your MCP client at `.venv/bin/python` and `server.py`. The bridge exposes device discovery, profile/key editing, mode management, factory reset, catalog lookup, and event watching.

### Wi-Fi / HTTP (optional)

When BLE/UI startup leaves enough heap, the firmware initializes Wi-Fi. Saved networks reconnect automatically, and the device-side MCP service starts only after the device is online. BLE keyboard operation remains the first priority.

Provisioning is always manual: long-press OK, open **Settings -> Provisioning**, then connect to the displayed SoftAP and open `http://192.168.4.1/`. After credentials are saved and the portal closes, the device reconnects to Wi-Fi and starts MCP. The firmware never opens the SoftAP automatically.

HTTP endpoints cover read/edit key slots, activate/add/rename/delete profiles, and use the same 17-preset catalog as the firmware and desktop MCP.

## Pairing

Pair the device in the host Bluetooth settings (its fixed Chinese name is documented in the Simplified Chinese README). If numeric comparison is required, the firmware accepts it and prints the passkey to the serial log. If a host caches an old HID report map, forget the device on the host or call the device BLE reset tool before pairing again.

## Build, test, and flash

```bash
./tools/validate.sh --static     # repository checks + framework/application host tests
./tools/validate.sh --firmware   # isolated ESP-IDF build + image verification
./tools/validate.sh              # complete gate

export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.13_env"
source ~/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbmodem1101 flash
```

The complete gate creates a checksummed archive under `build/firmware/<sha256>/`. For a clean flash of bootloader, partition table, and app together, use `idf.py flash` or write the generated merged image from address `0x0`.

## Layout

```text
main/
  main.c          startup, key queue, appfw full_key routing, optional network start
  kbmic_config.h  packed v3 model and HID constants
  kbmic_model.c   defaults, validation, migration, profile operations, action names
  kbmic_store.c   NVS load/save and live configuration
  kbmic_action.c  action catalog and HID action execution
  kbmic_hid.c     BLE HID, Apple Fn + Consumer reports, pairing/bond reset
  kbmic_ble_svc.c chunked GATT config service and event notifications
  kbmic_ui.c      home page and keyboard settings views
  kbmic_mcp.c     optional device-side MCP tools
  kbmic_web.c     optional HTTP keyboard settings API
assets/fonts/     retained font generation assets and license materials
mcp_server/       desktop BLE MCP bridge (protocol, BLE transport, tools, tests)
tools/            gate wrapper and font tooling
tests/            host tests, HID descriptor checks, UI glyph coverage
components/framework/  pinned aipassport-fw BSP/appfw submodule
```

## Hardware verification

Verified on hardware on 2026-10-06:

- BLE advertising, pairing, and HID connection.
- Apple Fn press on up-key long press and release on key release.
- Down-key Enter and Escape, OK-key Backspace, and OK-key settings navigation.
- Three-key ADC events, home UI feedback, BLE GATT config service readiness, and lifecycle routing through appfw `full_key`.

Host-side behavior of Apple Fn outside macOS is not implied by the shared default. Profiles can be edited individually if an input method or OS expects another shortcut.
