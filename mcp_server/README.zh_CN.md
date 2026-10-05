<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# kbmic MCP 服务端

通过蓝牙低功耗(BLE)给 **AI小键盘** 配置按键模式的 MCP 服务,stdio 方式运行。

设备是一块 ESP32-C3 的 BLE HID 键盘(广播名 `AI小键盘`),每个"模式"对应一套
按键映射(微信语音输入、系统语言切换之类)。这个服务把整份配置读出来解析成可读
结构,让你用自然语言改键,再整份写回去;所有本地计算(编解码、校验、生成显示名)
都不需要硬件,可以离线跑单元测试。

---

## 一、它做什么

* 扫描并连上 `AI小键盘`,读出 1580 字节的配置(10 个 GATT 分片)并解析;
* 列出所有模式、每个模式三个键(上 / 下 / OK)各自的动作;
* 新增 / 删除 / 改名模式,切换当前模式;
* 给任意一个键(短按 / 长按)设动作:可以用内置预设,也可以给自定义步骤序列;
* 恢复单个内置模式或整机的出厂默认;
* 订阅设备事件(启动 / 配置已保存 / 按键),用于自动化验证。

三个键:`Up` 上、`Down` 下、`OK` 确认。每个键有两个槽:`short-tap` 短按、
`long-press` 长按。所以一个模式有 3 × 2 = 6 个可配的槽位,每个槽位是一个
最多 4 步的动作。

---

## 二、安装依赖

```bash
cd /Users/weibh/Desktop/aipassport-kbmic/mcp_server
python3 -m pip install -r requirements.txt
```

只依赖两个包:`bleak`(BLE)、`mcp`(MCP SDK)。建议用虚拟环境:

```bash
python3 -m venv .venv && source .venv/bin/activate
python3 -m pip install -r requirements.txt
```

### 蓝牙权限(macOS 必看)

`bleak` 在 macOS 上走系统的 CoreBluetooth,所以**运行本服务的那个程序**
(终端、IDE、或者承载 MCP 的聊天客户端)必须有蓝牙权限:

> 系统设置 → 隐私与安全性 → 蓝牙 → 打开你所用程序的开关 → **重启该程序**

没权限时所有工具都会返回一段可执行的排查提示(上电?已配对?权限?在范围内?),
而不是抛一个看不懂的堆栈。

另外:HID 设备同一时间只接受**一个**主机连接。如果设备已经连着另一台机器,
这里就连不上。

---

## 三、独立自检(不需要硬件)

```bash
python3 /Users/weibh/Desktop/aipassport-kbmic/mcp_server/server.py --selftest
```

打印协议布局自检结果(配置 1580 字节、8 个分片、目录与出厂默认一致性),不碰蓝牙。

单元测试(同样不需要硬件,也不需要装 bleak / mcp):

```bash
python3 -m unittest discover -s /Users/weibh/Desktop/aipassport-kbmic/mcp_server -v
```

---

## 四、注册成 MCP stdio 服务

在 MCP 客户端的配置里(Claude Code / Cursor / Claude Desktop 等)加入:

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

用虚拟环境的话,把 `command` 换成解释器绝对路径即可:

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

改完配置重启客户端。服务起来之后,可以直接说"列出 AI小键盘的模式"来验证连通性。

> 蓝牙权限跟着**运行本服务的程序**走。如果客户端是以图形方式启动的,第一次
> 连设备时系统会弹权限对话框,必须点"允许",并且之后要重启客户端。

---

## 五、工具清单

| 工具 | 作用 |
| --- | --- |
| `kbmic_list_devices` | 扫描设备,返回地址 / 名字 / RSSI;可 pin 住某台设备 |
| `kbmic_get_config` | 读整份配置并解析(模式、每个键每个槽的动作与显示名) |
| `kbmic_list_modes` | 紧凑视图:每个模式一行,三个键当前各干什么 |
| `kbmic_set_active_mode` | 指定当前生效的模式索引 |
| `kbmic_add_mode` | 新增用户模式(键位从当前模式复制),返回新索引 |
| `kbmic_delete_mode` | 删除用户模式;内置模式会被拒绝 |
| `kbmic_rename_mode` | 改模式名(UTF-8 最多 15 字节,超长报错) |
| `kbmic_set_key` | **核心**:给 (模式, 键, 槽) 设动作,可用预设或自定义步骤 |
| `kbmic_reset_mode` | 把某个内置模式恢复出厂默认 |
| `kbmic_reset_device` | 整机恢复出厂(4 个内置模式,当前模式 = 0) |
| `kbmic_action_catalog` | 列出 16 个内置动作预设 |
| `kbmic_watch` | 订阅设备事件(start / status / stop) |

所有工具都返回一个字典,含 `ok` 字段;失败时 `ok: false` 并带 `error` 文本。
除 `kbmic_list_devices` / `kbmic_action_catalog` 外,都接受一个可选的
`address` 参数(Mac 风格地址),不传就用已 pin 的地址或重新扫描。

### 几个例子

```
kbmic_list_devices()                                  # 先找到设备
kbmic_list_modes()                                    # 现在每个键干什么
kbmic_set_key(index=0, button="OK", slot="long-press", preset="Globe")
kbmic_set_key(index=1, button="Up", slot="short-tap",
              trigger="click",
              steps=[{"kind": "key", "mods": "Ctrl+Shift", "keycode": 0x41}])
kbmic_add_mode(name="我的语音")
kbmic_set_active_mode(index=4)
kbmic_delete_mode(index=0)                            # 会被拒绝:内置模式
```

---

## 六、内置动作目录

id 与固件 `main/kbmic_action.c` 的 `ACT_*` 枚举同序,不要重排。
用 `preset=<id 或名字>` 引用,名字大小写不敏感。

| id | 名字 | 触发 | 动作 |
| --- | --- | --- | --- |
| 0 | `-` | none | 空 |
| 1 | Enter | click | 1 × KEY(0x28) |
| 2 | Back | click | 1 × KEY(0x2A) |
| 3 | Tab | click | 1 × KEY(0x2B) |
| 4 | Space | click | 1 × KEY(0x2C) |
| 5 | Esc | click | 1 × KEY(0x29) |
| 6 | Globe | tap(按住) | 1 × CONSUMER(0x029D) |
| 7 | Ctrl+Win | tap(按住) | 1 × KEY(mods=Ctrl\|Win, keycode=0) |
| 8 | Up | click | 1 × KEY(0x52) |
| 9 | Down | click | 1 × KEY(0x51) |
| 10 | Left | click | 1 × KEY(0x50) |
| 11 | Right | click | 1 × KEY(0x4F) |
| 12 | Enter x3 | click | 3 × KEY(0x28) |
| 13 | F1 | click | 1 × KEY(0x3A) |
| 14 | F2 | click | 1 × KEY(0x3B) |
| 15 | Settings | none | 软件动作:打开机身菜单,不发 HID 报告 |

---

## 七、自定义步骤

`kbmic_set_key` 的 `steps` 接受一个数组,最多 4 步。每步 7 字节:

| 字段 | 说明 |
| --- | --- |
| `kind` | `0/none`、`1/key`、`2/consumer`、`3/delay` |
| `mods` | 位掩码,或 `"Ctrl+Shift"` 这样的字符串。bit0=Ctrl bit1=Shift bit2=Alt bit3=Win |
| `keycode` | HID 键盘/ keypad 用法码(回车 0x28、Esc 0x29、退格 0x2A、Tab 0x2B、空格 0x2C、A-Z 0x04-0x1D、数字 0x1E-0x27、F1-F12 0x3A-0x45) |
| `usage` | 16 位 Consumer 用法码(Globe = 0x029D) |
| `delay_ms` | 延时毫秒数 |

例子:先按 Ctrl+Shift+A,等 80 毫秒,再按回车。

```json
{"trigger": "click", "steps": [
  {"kind": "key", "mods": "Ctrl+Shift", "keycode": 0x04},
  {"kind": "delay", "delay_ms": 80},
  {"kind": "key", "keycode": 0x28}
]}
```

显示成 `Ctrl+Shift+A +80ms Enter`。

### 显示名规则

动作名**不落盘**,每次由步骤现拼(和固件 `kbmic_action_name()` 一致):

* `KEY`:`Ctrl+Shift+Alt+Win+` 前缀(按此顺序)+ 键名;只有修饰键没有主键时
  去掉尾部 `+`,如 `Ctrl+Win`;表外键码退化成 `0x%02X`;
* `CONSUMER`:`0x029D` 显示 `Globe`,其余 `C:0x%04X`;
* `DELAY`:`+%dms`;
* 多步用单个空格连接;
* 按住类(tap)追加 ` (hold)`;
* 空动作或 `trigger=none` 显示 `-`;
* `kind=none` 且 `mods` 高位置位的是 `Settings` 软件动作。

---

## 八、线协议(与固件的共同契约)

```
Service        7d1c5a30-9f6e-4a21-8c3d-2b5e7a9f1c48
Config chunk 0 7d1c5a40-…  read / write      共 10 个,前 9 个 160 字节
Config chunk 9 7d1c5a49-…  read / write      第 10 个 140 字节
Event          7d1c5a4f-…  read / notify
```

* 固件**不支持 GATT long read/write**,所以服务端一律用普通的单次
  read/write,按 0..9 顺序一格一格搬并自己拼接;
* 写的时候 `response=True`,一格一次调用,顺序 0..9;
* 设备收齐全部 10 格才会提交并落盘,校验不过就整份丢弃 —— 所以"写配置"永远是
  写满 10 格,不存在只写一半的中间状态;
* 写完立刻读回来比对,不一致会把差异放在返回值里(`verified: false` + `mismatch`)。

配置结构(小端、packed、无对齐空洞,共 1580 字节):

```
0    1  version  = 1
1    1  active   当前模式索引
2    1  count    模式数量 1..8
3    1  reserved
4  1576 profiles[8]     每个 197 字节
```

模式 = 名字 16 字节(UTF-8,以 `\0` 结尾,内容最多 15 字节)+ builtin 1 字节 +
`slots[3][2]`(3 个键 × 2 个槽),槽 = 30 字节(触发方式 + 步数 + 4 × 7 字节步骤)。

事件报文:`type(1) + active(1) + aux(1) + reserved(1) + name_len(1) + name(n)`,
`type` 为 0=BOOT / 1=CONFIG_SAVED / 2=KEY(KEY 时 `aux` 是按钮索引)。

---

## 九、文件结构

```
mcp_server/
├── protocol.py      线协议:常量、偏移、编解码、校验、显示名、分片、事件解析(纯逻辑,无依赖)
├── catalog.py       16 个内置预设 + 出厂默认 4 模式(镜像固件)
├── ble.py           BLE 传输:扫描、连接、分片读写、事件订阅(惰性 import bleak)
├── server.py        MCP stdio 服务与 12 个工具(import 时不碰蓝牙)
├── test_protocol.py 单元测试(stdlib unittest,不需要硬件)
├── requirements.txt 依赖
└── README.zh_CN.md  本文件(英文版见 README.md)
```

改任何布局或目录,都必须同步改固件 `main/kbmic_config.h` / `main/kbmic_action.c`,
否则会写出一份设备读成乱码的配置。改完先跑单元测试:

```bash
python3 -m unittest discover -s /Users/weibh/Desktop/aipassport-kbmic/mcp_server -v
```
