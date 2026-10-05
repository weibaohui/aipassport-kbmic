// main/kbmic_action.h —— 动作执行器 + 固件内置动作目录。
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "kbmic_config.h"

// 目录项。界面上"选一个动作"和 MCP 报"有哪些内置动作"用的是同一张表,
// 所以固件和 MCP 各自维护一份是危险的 —— 索引必须能互相对上。
typedef struct {
    const char *name;        // 界面显示用的 ASCII 名
    kbmic_action_t action;   // 实际动作
} kbmic_catalog_item_t;

// 目录长度。改了这里要同步 mcp_server/catalog.py。
#define KBMIC_CATALOG_MAX 20

const kbmic_catalog_item_t *kbmic_catalog_get(uint8_t id);
uint8_t kbmic_catalog_count(void);

// 这个动作是不是"进设置"这种纯软件动作。main 必须在发 HID 之前拦下它。
bool kbmic_action_is_settings(const kbmic_action_t *a);

// 执行一个动作。CLICK / LONG 会自己按下并松开;TAP 只按下,松手由
// kbmic_action_release() 收尾。settings 动作在这里直接返回 ESP_ERR_NOT_SUPPORTED。
esp_err_t kbmic_action_run(const kbmic_action_t *a);

// 松开所有还按着的报告。当前实现只会同时按住"键盘"和"Consumer"两种报告,
// 所以记住两个标志就够,不需要通用的挂起表。
esp_err_t kbmic_action_release(void);
