<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# AI小键盘(AI Passport 固件)

把 [FoloToy AI Passport](https://github.com/FoloToy/ai-passport) 掌机(ESP32-C3、2.4″ 240×320 屏、三键、8 MB Flash、无 PSRAM)变成可配置的 BLE HID 键盘。当前默认键位为听写优化：长按上键向已连接的 Mac 按下 Apple Fn，松开结束；下键和 OK 键负责回车、取消、退格。

蓝牙设备名固定为 **AI小键盘**，不需要手机 App。

工程通过 git submodule 使用 [aipassport-fw](https://github.com/weibaohui/aipassport-fw)(挂在 `components/framework`)：BSP 负责 ADC 按键、显示、电量和 LVGL 锁;`appfw_ui` 负责设置壳层和完整按键生命周期;只有当已有保存的 Wi-Fi 且堆内存足够时，才可选启用 `appfw` 联网链路。

## 为什么识别在主机侧

ESP32-C3 只有 Bluetooth LE，不能通过 HFP 或 LE Audio 被系统当作麦克风。设备因此以键盘身份工作：

| 职责 | 执行位置 |
| --- | --- |
| 麦克风采集、语音识别、标点、候选词 | 主机系统 / 输入法 |
| 触发语音输入、回车、取消、退格、自定义快捷键 | 本设备(BLE HID 键盘) |
| 设备端录音与 ASR | 不使用 |

正常听写路径不需要 App、账号或网络；可选的 Wi-Fi/HTTP 配置与按键功能彼此独立。

## 默认键位

内置 Mac / Windows / Android / iOS 四个档案，另可创建最多 4 个自定义档案。四个内置档案的默认物理键位完全一致：

| 按键 | 动作 |
| --- | --- |
| 上键长按 | Apple Fn 按下；松开发送 Fn 抬起 |
| 下键短按 | Enter |
| 下键双击 | Escape |
| OK 短按 | Backspace |
| OK 长按 | 打开机身设置菜单 |
| 上键短按/双击、下键长按、OK 双击 | 无动作 |

上键使用标准 8 字节键盘报告第 2 字节中的 Apple Top Case Fn usage。主机决定该键的系统行为；macOS 语音输入已实机验证。Windows/Android/iOS 的映射可能不同，每个档案仍可通过 MCP、HTTP 或机身 UI 单独修改。

## 配置模型

- 协议版本：v3；NVS 中的 v2 数据可原地迁移。
- 档案：4 个内置 + 最多 4 个自定义，上限 8 个。
- 槽位：每个按键 3 个——短按、双击、长按。
- 动作：最多 4 步。
- 步骤类型：带修饰键的键盘键、Consumer Control usage、延时、Apple Fn。
- 触发方式：none、click、tap/hold、long、double。
- 存储：小端 packed 配置，持久化在 NVS。

C 与 Python 各自生成动作显示名，但必须逐字节一致；主机测试会检查这一点。

## 设备界面

240×320 主页显示三张按键卡片、当前模式、BLE 连接状态、电量、说话/按住状态和最近一次按键反馈。

长按 OK 打开框架设置菜单，包含息屏时间、亮度、设备信息、配网，以及「按键模式」。键盘设置分三层：

1. **键盘模式**：选择档案，或新建自定义模式。
2. **按键配置**：3 个按键 × 3 个槽。
3. **选动作**：17 个内置预设，包括 Enter、Backspace、Tab、Space、Esc、Globe、Ctrl+Win、方向键、F1/F2、Enter ×3、设置、Apple Fn。

上下键移动光标，OK 选中。`< 返回` 行回到上一层，长按 OK 退出键盘设置视图。机身模式列表只读，不再提供“新建模式”；如需新增，请使用 AI/MCP。模式名支持中文，界面 16px 字体覆盖 GB2312。

## 配置通道

### BLE GATT(主通道)

自定义 GATT 配置服务始终注册，可在桌面通过 `mcp_server/` 离线配置：

```text
Service        7d1c5a30-9f6e-4a21-8c3d-2b5e7a9f1c48
Config chunks  7d1c5a40-… 到 7d1c5a4e-…，read/write
Event          7d1c5a4f-…，read/notify
```

整份配置 2300 字节：前 14 片各 160 字节，第 15 片 60 字节。客户端必须写完全部 15 片；设备校验完整配置后才提交并写入 NVS。

安装并运行桌面 MCP 桥：

```bash
cd mcp_server
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python -m unittest discover -s . -v
```

把 MCP 客户端指向 `.venv/bin/python` 和 `server.py`。桥接服务支持设备发现、档案/按键编辑、模式管理、恢复出厂、动作目录和事件订阅。

### Wi-Fi / HTTP(可选)

当 BLE/UI 启动后堆内存足够时，固件会初始化 Wi-Fi。已保存的热点会自动回连；设备侧 MCP 服务只在 Wi-Fi 真正上线后启动。BLE 键盘始终保持最高优先级。

配网必须手工开启：长按 OK 进入 **设置 → 配网**，连接屏幕显示的 SoftAP，打开 `http://192.168.4.1/` 保存热点。凭据保存、portal 关闭后，设备回连 Wi-Fi 并启动 MCP。固件不会自动打开 SoftAP。

HTTP 端点支持读取/编辑按键槽、激活/新增/改名/删除模式，并与固件、桌面 MCP 使用同一个 17 项动作目录。

## 配对

首次在系统蓝牙设置中选择 **AI小键盘** 配对。需要数字比对时，固件会自动确认并把配对码打印到串口日志。如果主机缓存了旧 HID report map，请先在主机侧忽略设备，或调用设备 BLE reset 后重新配对。

## 构建、测试与烧录

```bash
./tools/validate.sh --static     # 仓库检查 + 框架/应用主机测试
./tools/validate.sh --firmware   # 独立 ESP-IDF 构建 + 镜像校验
./tools/validate.sh              # 全量门禁

export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.13_env"
source ~/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbmodem1101 flash
```

全量门禁会在 `build/firmware/<sha256>/` 生成带校验的归档。要同时刷新 bootloader、分区表和 app，使用 `idf.py flash`，或从 `0x0` 写入生成的合并镜像。

## 目录

```text
main/
  main.c          启动、按键队列、appfw full_key 路由、可选网络启动
  kbmic_config.h  packed v3 配置模型与 HID 常量
  kbmic_model.c   默认值、合法性、迁移、档案操作、动作名
  kbmic_store.c   NVS 读写与生效配置
  kbmic_action.c  动作目录和 HID 动作执行
  kbmic_hid.c     BLE HID、Apple Fn + Consumer 报告、配对/bond reset
  kbmic_ble_svc.c 分片 GATT 配置服务与事件通知
  kbmic_ui.c      主页与键盘设置视图
  kbmic_mcp.c     可选设备侧 MCP 工具
  kbmic_web.c     可选 HTTP 键盘设置 API
assets/fonts/     保留的字体生成资产与许可材料
mcp_server/       桌面 BLE MCP 桥(协议、BLE 传输、工具、测试)
tools/            门禁封装与字体工具
tests/            主机测试、HID 描述符检查、UI 字形覆盖检查
components/framework/  固定版本的 aipassport-fw BSP/appfw 子模块
```

## 实机验证

2026-10-06 已实机验证：

- BLE 广播、配对和 HID 连接。
- 上键长按按下 Apple Fn，松开释放 Fn。
- 下键 Enter / Escape，OK Backspace，OK 长按进入设置。
- 三键 ADC 事件、主页 UI 反馈、BLE GATT 配置服务就绪，以及 appfw `full_key` 生命周期路由。

共享默认键位不承诺 Apple Fn 在 macOS 以外系统的行为。如果宿主输入法或系统需要其他快捷键，可单独编辑各档案。
