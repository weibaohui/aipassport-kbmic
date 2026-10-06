// main/kbmic_ble_svc.h —— 配置服务:MCP 与设备之间的唯一通道。
//
// 服务结构(自定义 128 位 UUID,末段按 0x40 起递增便于记忆):
//   Service        7d1c5a30-9f6e-4a21-8c3d-2b5e7a9f1c48
//   Config chunk 0 7d1c5a40-…   read / write
//   Config chunk 1 7d1c5a41-…   read / write
//   …
//   Config chunk N 7d1c5a40+N-1
//   Event          7d1c5a4f-…   read / notify
//
// 为什么分片而不是一个长特征:IDF 5.5 的 GATT server 公开 API 里没有
// esp_ble_gatts_read_long_resp / esp_ble_gatts_prerelease,长读/长写没有干净的
// 公开入口。分片用的是最普通的 read/write,跨 macOS/iOS/Android 都不会踩协议栈
// 的长读实现差异。分片大小取 160 字节,小于 iOS 常见 MTU 185 减掉协议头的余量。
// 客户端(MCP)负责拼回整份配置,这个负担对使用者是透明的。
//
// Event 报文(设备 → 客户端,小端):
//   u8  type       见 kbmic_ev_type_t
//   u8  active     当前模式索引
//   u8  aux        type 相关的附加字节(type==KEY 时是 btn,其余为 0)
//   u8  reserved
//   u8  name_len   后面跟着多少字节的 UTF-8 名字(可为 0)
//   …   名字
// 之所以要事件:改完配置后 MCP 需要知道设备真的生效了,而不是"写下去就算成功"。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "kbmic_config.h"

#define KBMIC_SVC_CHUNK_SIZE 160
#define KBMIC_SVC_CHUNKS \
    ((KBMIC_CONFIG_SIZE + KBMIC_SVC_CHUNK_SIZE - 1) / KBMIC_SVC_CHUNK_SIZE)

#define KBMIC_EV_BOOT 0        // 设备启动/连接完成,携带当前模式名
#define KBMIC_EV_CONFIG_SAVED 1 // 刚接受并落盘了一份新配置
#define KBMIC_EV_KEY 2         // 用户按键,aux=btn(自动化测试用)

#define KBMIC_EV_MAX 64

// 注册 GATT 服务。必须在 esp_hidd_dev_init 之后调用:
// 框架内部有一份静态的"最后一次建表"指针,多个 GATT app 的建表事件会共用它,
// 按注册顺序排队才不会互相踩。
esp_err_t kbmic_ble_svc_init(void);

// 请求"注册配置服务"(投递到 svc 任务异步执行)。必须在 HIDD START 事件里
// 调用而不是直接调 kbmic_ble_svc_init:START 回调跑在 esp_hid 的 4KB 事件
// 任务里,GATTS app 注册的 BTC 调用链会把那个栈压穿(2026-10-06 真机踩坑)。
void kbmic_ble_svc_request_register(void);

// 注册全局 GATT 回调。时机约束:**Bluedroid enable 之后**、任何
// esp_ble_gatts_app_register 之前(包括 esp_hidd_dev_init 内部的那几次)。
// 放在 Bluedroid 初始化前会静默失败,GATTS 事件从此无人接收。
esp_err_t kbmic_ble_svc_install_dispatch(void);

// 通知所有已连接的订阅者。没订阅者时静默返回,不报错 —— 没人在听是常态。
void kbmic_ble_svc_notify(uint8_t type, uint8_t active, uint8_t aux, const char *name);
