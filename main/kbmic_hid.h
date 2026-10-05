// main/kbmic_hid.h —— BLE HID 设备(键盘 + Consumer 控制)。
//
// 设备以 HOGP(HID over GATT)身份广播,主机把它当成一把标准蓝牙键盘。
// 除了 8 字节标准键盘报告,本应用还额外声明了一张 Consumer Control 报告,
// 用来发 Apple 的 Globe(🌐/Fn)键 —— 那是 macOS 微信语音输入的默认触发键。
//
// 为什么不把 Fn 塞进键盘报告:标准 Keyboard/Keypad Page(0x07)里没有 Fn 这个
// usage,Fn 在多数键盘上由键盘自己的 MCU 消化掉,主机从来收不到。Apple 后来在
// Consumer Page 里补了一个官方编码 0x029D(Keyboard Layout Select),macOS 会
// 认它。所以这里用第二张 report map 单独发 Consumer 报告。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "kbmic_config.h"   // KBMIC_HID_KEY_* 与 KBMIC_HID_USAGE_GLOBE 在那边定义

// 初始化 BT controller + Bluedroid + HID GATT 服务,并开始广播。
// 设备名固定为"AI小键盘"。成功时已处于广播态,等待主机连接。
esp_err_t kbmic_hid_init(void);

// 主机是否已连接。未连接时所有发送接口会安静失败(返回 ESP_ERR_INVALID_STATE),
// 不打印错误 —— 蓝牙键盘在未配对时按鍵是常态,不是故障。
bool kbmic_hid_connected(void);

// 上报电量百分比(0..100),走标准 BLE Battery Service。
esp_err_t kbmic_hid_set_battery(int percent);

// 点按一次:按下 -> 20 ms -> 松开。modifier 与 keycode 允许同时给,例如
// (MOD_LCTRL, 'c')。用于 Enter、退格这类离散动作。
esp_err_t kbmic_hid_tap(uint8_t modifier, uint8_t keycode);

// 按住类动作的半边。pressed=true 发按下报告,false 发全零释放报告。
// 语音输入是"按住说话",必须用这对接口,由调用方保持住状态。
esp_err_t kbmic_hid_key_hold(uint8_t modifier, uint8_t keycode, bool pressed);

// Consumer 报告的按下/释放。usage 见 KBMIC_HID_USAGE_GLOBE。
esp_err_t kbmic_hid_consumer(uint16_t usage, bool pressed);
