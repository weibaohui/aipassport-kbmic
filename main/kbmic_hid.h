// main/kbmic_hid.h —— Mac 最小模式 BLE HID 键盘。
//
// 设备以 HOGP(HID over GATT)身份广播。8 字节报告依次为 modifier、Apple Fn、
// 6 个键码；Fn 使用 AppleVendor Top Case usage page 0xFF / KeyboardFn usage 0x03。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// 初始化 BT controller + Bluedroid + HID GATT 服务,并开始广播。
// 设备名固定为"AI小键盘"。成功时已处于广播态,等待主机连接。
esp_err_t kbmic_hid_init(void);

// 主机是否已连接。未连接时所有发送接口会安静失败(返回 ESP_ERR_INVALID_STATE),
// 不打印错误 —— 蓝牙键盘在未配对时按鍵是常态,不是故障。
bool kbmic_hid_connected(void);

// 点按一次:按下 -> 20 ms -> 松开。modifier 与 keycode 允许同时给,例如
// (MOD_LCTRL, 'c')。用于 Enter、退格这类离散动作。
esp_err_t kbmic_hid_tap(uint8_t modifier, uint8_t keycode);

// 按住类动作的半边。pressed=true 发按下报告,false 发全零释放报告。
// 语音输入是"按住说话",必须用这对接口,由调用方保持住状态。
esp_err_t kbmic_hid_key_hold(uint8_t modifier, uint8_t keycode, bool pressed);

// Apple Fn:复用 8 字节键盘报告,在第 2 字节发送 AppleVendor Top Case/KeyboardFn。
esp_err_t kbmic_hid_applefn(bool pressed);
