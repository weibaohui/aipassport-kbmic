// main/kbmic_mcp.h —— 设备侧 MCP 工具(经框架 8080 端口的 JSON-RPC)。
//
// 与 mcp_server/(BLE 桥)的工具语义保持一致:模式增删改/激活/按键配置。
// 网络侧多出 WiFi 管理与模拟触发按键。动作的 JSON 形状两边共用:
//   {trigger: "none"|"click"|"tap"|"long"|0..3,
//    steps: [{kind:"key"|"consumer"|"delay"|0..3, mods:"Ctrl+Shift"|0..15,
//             keycode:0..255, usage:0..65535, delay_ms:0..65535}]}
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "kbmic_config.h"

#include "cJSON.h"

// 注册工具表并启动 MCP 常驻服务(幂等)。必须在 appfw 网络栈起来之后。
void kbmic_mcp_init(void);

// 模拟触发回调:main 注入(要往按键队列里投递事件)。
typedef void (*kbmic_sim_fn_t)(int btn, bool long_press);
void kbmic_mcp_set_simulate(kbmic_sim_fn_t fn);

// ---- JSON 序列化(kbmic_web 的门户端点复用) ----
// 把一个模式的 6 个槽(button×slot)写进 JSON 数组,含 display 名。
void kbmic_mcp_add_slots_json(cJSON *arr, const kbmic_profile_t *p);

// ---- JSON ↔ 动作转换 ----
// 解析失败返回 false 且不写 out。steps 缺省视为空动作。
bool kbmic_action_from_json(const cJSON *j, kbmic_action_t *out);
// 写出 {trigger, steps:[...]} 并附 display 名。
void kbmic_action_to_json(const kbmic_action_t *a, cJSON *out);
