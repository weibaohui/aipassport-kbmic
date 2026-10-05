// main/kbmic_store.h —— 配置的持久化与生效副本。
//
// 为什么单独一个头:这几个函数要碰 NVS 和 esp_log,返回值是 esp_err_t。
// kbmic_config.h(纯模型 + 线协议结构体)刻意不拉任何 ESP-IDF 头,好让
// tests/test_kbmic_model.c 能在主机上直接编译它。两者分开,主机测试才成立。
#pragma once

#include "esp_err.h"
#include "kbmic_config.h"

// 启动时读 NVS 装进生效存储。读不到 / 版本对不上 / 校验失败都回落默认值并重建,
// 绝不返回半个有效的配置;本函数返回后 kbmic_config_current() 一定可用。
esp_err_t kbmic_config_load(void);

// 整体写回 NVS。失败只记录日志,不阻断按键使用 —— 配置存不进去不该让键盘瘫掉。
esp_err_t kbmic_config_save(const kbmic_config_t *cfg);

// 当前生效配置的**只读**视图。地址在整个运行期稳定(内部静态存储),
// BLE 分片特征的值指针就指着它,所以返回的指针可以长期持有。
const kbmic_config_t *kbmic_config_current(void);

// 提交一份新配置:先校验,再存进生效存储与 NVS,最后触发 hook。
// 界面上的改动和 MCP 写进来的配置都走这一个入口,保证只有一条生效路径。
esp_err_t kbmic_config_commit(const kbmic_config_t *cfg);

// 配置生效后的回调。只保留一个,后注册的覆盖先注册的。
typedef void (*kbmic_config_hook_t)(void);
void kbmic_config_set_hook(kbmic_config_hook_t hook);
