<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# AI小键盘(AI Passport 固件)

把 [FoloToy AI Passport](https://github.com/FoloToy/ai-passport) 掌机(ESP32-C3,2.4″ ST7789 240×320,三键)变成一个**可自定义的蓝牙语音输入遥控器**:按住确定键,电脑/手机自己的语音输入就启动了,说完松开,文字直接落在光标处。

蓝牙设备名固定为 **AI小键盘**。

基于 [aipassport-fw](https://github.com/weibaohui/aipassport-fw) 基础框架(以 git submodule 挂在 `components/framework`)开发,只用其中的 `bsp`(显示/LVGL、按键、电池),不引入 `appfw` 的联网链路。

## 为什么识别在电脑/手机而不在设备上

ESP32-C3 只有 Bluetooth LE,没有经典蓝牙(BR/EDR),HFP 免提协议这条硬件路不存在;`SOC_BLE_ISO_SUPPORTED` 全系缺失,LE Audio 的 LC3 也用不上。**它做不了一个能被系统当作麦克风的蓝牙设备。**

所以分工很干脆:

| | 谁来做 |
| --- | --- |
| 麦克风采集、语音识别、标点与候选词 | 电脑 / 手机自己的输入法 |
| 触发语音输入、确认、删除、任意按键 | 本设备(BLE HID 键盘) |
| 设备端录音与 ASR | 不参与 |

识别在本地跑,没有额外延迟,也**不需要 App、不需要联网、不需要账号**。

## 键盘模式与按键定义

设备带 **4 个内置模式 + 最多 4 个用户自定义模式**(上限 8)。每个模式里三个键都可自定义,每个键有两个槽:

- **短按** —— 点一下触发,或按住触发(语音就是这么配的)
- **长按** —— 按住超过 500 ms 触发

> **为什么说话不在长按上**:说话=按住 OK,按多久说多久;BSP 的长按阈值
> (500 ms)必然落在说话途中。所以出厂默认 OK 长按留空,"进设置"放在
> 长按下键 —— 两者不打架。想改回来用 MCP `kbmic_set_key` 或网页。

一个动作可以由**最多 4 步**组成,每步是一次带修饰键的敲击(`Ctrl+Win`)、一次 Consumer 用法(`Globe`),或一步延时。所以「回车回车回车」「Ctrl+Shift+A,等 80ms,再回车」这类组合都能配出来。

### 内置模式

**Mac**(用户定稿,2026-10-06 二改):

| 按键 | 动作 |
| --- | --- |
| **长按上键** | **按住 Fn/Globe 说话** —— 按住=Fn 按下,松开=Fn 抬起 |
| **双击下键** | 发送 Esc |
| 长按 OK | 进设置 |
| 短按下键 | 回车 |
| 短按 OK | 退格 |
| 短按上键 / 长按下键 | 无(留空,可经 MCP/网页自行分配) |

其他三模式(通用键位):

| 模式 | OK 短按(按住说话) | 上 / 下 |
| --- | --- | --- |
| **Windows** | `Ctrl+Win` | 回车 / 退格 |
| **Android** | 长按空格 | 回车 / 退格 |
| **iOS** | Globe(⚠️ 未实机验证) | 回车 / 退格 |

通用键位:上键短按回车、长按连发三次;下键短按退格、长按进设置;OK 长按留空
(说话=按住 OK,长按阈值会切断说话)。所有键位都可用 MCP / 网页 / 机身菜单改。

> Mac 走 Consumer `0x029D` 而**不**仿冒 Apple VID/PID —— 后者(把 6KRO 报告第 2 字节私有化成 `FF00h/03h`)是 QMK 的社区做法,实测有效,但涉及商标/MFi 风险,商业产品不建议。
>
> Windows / Android 发的都是**普通组合键与按键**,快捷键仍可在宿主输入法里改成别的。微信的语音快捷键是可自定义的,所以不合适时不必回来改固件。
>
> iOS 没有公开的第三方硬件键全局语音输入接口,这一档只是沿用 Globe 报文做尝试,通不通得实测。不通就用 MCP 改成别的组合键。

## 界面

五个页面,全用三个键操作。

**主页** —— 三行,每行一个键,右边显示它当前实际发送的东西:

```
┌────────────────────────────┐
│ AI小键盘           ●已连接  │
│  ┌──────────────────────┐  │
│  │     按住 OK 说话      │  │   按住 OK 时整块变绿
│  └──────────────────────┘  │
│  模式 Mac         电量 87%  │
│                            │
│  上    Enter               │   ← 改配置后这里立刻跟着变
│  下    Back                │
│  OK    Globe (hold)        │
│                            │
│  长按下键:设置              │
└────────────────────────────┘
```

**设置菜单**(**长按下键**进入):`键盘模式` / `按键配置` / `恢复默认` / `开启配网`。

**键盘模式**:列出全部模式,内置的标「内置」,自定义的标「自定义」,当前的在用的标「使用中」。OK 选中即切换并落盘。

**按键配置**:6 行 = 3 个键 × 2 个槽,每行右侧是这个槽当前的动作名。

**动作选择**:从 17 个内置预设里挑(回车、退格、Tab、空格、Esc、Globe、Ctrl+Win、方向键、F1/F2、回车×3、进设置…)。

导航约定:上下键移动光标,OK 选中,**长按 OK 返回**上一层。

> 模式名支持中文(UTF-8 最多 15 字节),界面字体编进了《通用规范汉字表》一级字表的 3500 个常用字,所以自起的名字不会显示成方块。

## 用 MCP 配置(推荐)

设备有**两条 MCP 通道**,工具语义一致:

1. **设备侧(推荐,联网后)** —— 固件内置 MCP 常驻服务,局域网直连,无需任何
   宿主侧桥接。设备先配网(设置菜单 → 开启配网,手机连热点访问 192.168.4.1;
   或已联网时 AI 调 `kbmic_web_start` 起管理页),然后:

   ```
   POST http://<设备IP>:8080/mcp      # 标准 JSON-RPC,单端点
   ```

   15 个工具:配置读写(`kbmic_get_config` / `kbmic_set_key` 支持 preset 或
   自定义 steps:修饰键可用 "Ctrl+Shift" 字符串)、模式管理(增删改/激活/恢复
   出厂)、`kbmic_simulate_key`(模拟触发物理键,等效真人按键)、
   `kbmic_get_state`(模式/BLE/电量/WiFi)、热点增删查、以及框架内置的
   `wifi_status` / `get_device_info` / `get_provisioning_status` /
   `get_recent_logs`(诊断)。

2. **BLE 桥(离线可用)** —— 设备暴露自定义 GATT 配置服务,Mac 上跑
   [mcp_server/](mcp_server/) 的 MCP 服务端经蓝牙读写,不依赖网络。

### 网页设置

设备联网后,AI 调 `kbmic_web_start` 会返回管理页地址,浏览器打开页面下方有
「键盘设置」卡片:切换生效模式、六个槽(3 键 × 短按/长按)从内置动作目录
下拉选择写入。BLE+WiFi 共存下内存紧张,管理页按需起停(`kbmic_web_stop`
释放);不稳定就重启设备再来。完整热点配网走机身:设置菜单 → 开启配网。

### 配网流程

1. 机身:长按下键进设置 → 选「开启配网」→ 设备切热点模式(断 WiFi);
2. 手机连接设备热点,访问 `http://192.168.4.1`,选路由器热点、填密码、保存并连接;
3. 设备自动回连并关闭热点,屏幕底注/`get_provisioning_status` 可查 IP。

首次烧录后无热点配置时,设备会自动开启一次配网热点(之后全手动)。

### BLE 桥的工具一览

```
kbmic_list_devices()                       # 找到设备
kbmic_list_modes()                         # 现在每个键干什么
kbmic_set_key(index=0, button="OK", slot="long-press", preset="Globe")
kbmic_set_key(index=1, button="Up", slot="short-tap", trigger="click",
              steps=[{"kind": "key", "mods": "Ctrl+Shift", "keycode": 0x41}])
kbmic_add_mode(name="会议")
kbmic_set_active_mode(index=4)
kbmic_delete_mode(index=0)                 # 会被拒绝:内置模式
```

设备端 MCP 工具(列模式、加删模式、改键、恢复出厂、订阅事件……),完整清单与注册方法见 [mcp_server/README.zh_CN.md](mcp_server/README.zh_CN.md)。

<details>
<summary>自己装一个</summary>

```bash
cd ~/Desktop/aipassport-kbmic/mcp_server
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
python3 -m unittest discover -s . -v      # 62 个单测,不需要硬件
```

MCP 客户端配置:

```json
{ "mcpServers": { "kbmic": {
  "command": "/Users/weibh/Desktop/aipassport-kbmic/mcp_server/.venv/bin/python",
  "args": ["/Users/weibh/Desktop/aipassport-kbmic/mcp_server/server.py"] } } }
```

</details>

### 线协议要点

```
Service        7d1c5a30-9f6e-4a21-8c3d-2b5e7a9f1c48
Config chunk 0 7d1c5a40-…  read / write     共 15 片,前 14 片 160 字节
Config chunk 14 7d1c5a4e-… read / write     第 15 片 60 字节
Event          7d1c5a4f-…  read / notify
```

整份配置 2300 字节,小端、packed、无对齐空洞。**分片是因为 IDF 5.5 的 GATT server 公开 API 里没有长读/长写入口**,与其依赖内部实现,不如用最普通的 read/write,跨 macOS/iOS/Android 都不踩协议栈差异。设备收齐 15 片并通过校验才提交落盘,所以不存在写了一半的中间状态;写完 MCP 会读回来比对。

## 配对

首次在系统蓝牙设置里选 **AI小键盘** 配对。本机没有输入数字键盘的能力,若主机要求输入配对码,固件会把码打到串口日志里。

## 烧录

```bash
cd ~/Desktop/aipassport-kbmic
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.13_env"  # 非交互 shell 必须先指定
source ~/esp/esp-idf/export.sh
idf.py build
idf.py flash
```

门禁通过后 `build/firmware/<sha256>/` 保留带校验的归档,合并镜像在 `build/FoloToy-AI-Passport-full.bin`(整体从 `0x0` 写入,会重置 NVS 里的配置)。

## 构建与测试

```bash
./tools/validate.sh              # 全量:静态检查 + host 测试 + 固件构建与校验
./tools/validate.sh --static     # 只跑静态检查与 host 测试(不需要 ESP-IDF)
./tools/validate.sh --firmware   # 只跑固件构建
```

测试分三层,各管一段:

| 测试 | 守住什么 |
| --- | --- |
| `tests/test_kbmic_model.c` | 出厂默认、合法性、增删模式、**动作名渲染** |
| `tests/test_ui_charset.py` | 界面里每个非 ASCII 字符都在字体子集内 |
| `mcp_server/test_protocol.py` | 线协议编解码、校验、拒绝路径(62 个用例,不需要硬件) |

其中动作名渲染两边各有一份实现(C 与 Python),必须逐字一致,否则同一件事在设备屏幕上和 MCP 返回值里显示成不同的名字 —— `test_kbmic_model.c` 就是为此存在的。

改了界面文案后重新生成字体:

```bash
python3 tools/gen_font_charset.py
npx lv_font_conv@1.5.3 --font assets/fonts/NotoSansSC-Regular.otf \
  --size 16 --bpp 4 --format lvgl --no-compress \
  --lv-font-name app_font_16 --lv-include lvgl.h \
  --symbols "$(cat assets/fonts/kbmic_charset.txt)" \
  --output assets/fonts/app_font_16.c
```

字体与资产约定见 [assets/README.zh_CN.md](assets/README.zh_CN.md)。

## 目录

```
main/
  main.c          启动装配、按键队列、五屏状态机
  kbmic_config.h  配置模型与线协议结构体(纯 C,无 ESP-IDF 依赖)
  kbmic_model.c   出厂默认、合法性、增删模式、动作名反推(可在主机上编译测试)
  kbmic_store.c   NVS 持久化与生效副本
  kbmic_action.c  动作执行器 + 17 个内置预设
  kbmic_hid.c     BLE HID:两张 report map(键盘 + Consumer)、GAP 薄层、配对
  kbmic_ble_svc.c 配置服务:分片读写、暂存校验、事件通知
  kbmic_ui.c      五屏渲染
assets/fonts/     3500 常用字表、源字库、生成字体、许可
mcp_server/       MCP 服务端(协议 / 目录 / BLE / 工具 / 单测)
tools/            门禁转发、符号表生成
tests/            固件侧主机测试与字形覆盖门禁
```

## 待实机验证

编译通过、门禁全绿、协议两侧逐字节核对过,都不等于链路一定通。下面几点**必须在真机上验证**:

已验证(2026-10-06 真机):BLE 广播/配对/连接、设备侧 MCP 全部工具
(配置读写/模式管理/模拟触发/热点管理)、网页管理页与键盘卡片、
配网门户(热点模式)。

仍待验证:

1. Mac 上按住确定键,微信电脑版的语音输入是否弹出(Consumer `0x029D` 方案 A)。
2. Android 上 BLE 硬件键盘的长按空格能否送到微信输入法。
3. iOS 那一档到底通不通。
4. BLE 桥(mcp_server/)在真实 macOS CoreBluetooth 权限下的完整读写。

若第 1 点不成立,退路是让用户把微信快捷键改成 `Ctrl+Option+V` 这类普通组合键 —— 固件已经支持任意组合,不需要重编。
