// main/kbmic_ble_svc.c —— 配置服务的 GATT 实现。见 kbmic_ble_svc.h 的分片说明。
//
// IDF 5.5 的属性表与 5.4 差别很大,这里踩过的坑记在下面:
//   * esp_attr_desc_t.uuid(结构体) 变成了 uuid_length + uuid_p(指向 esp_bt_uuid_t 的指针)
//   * 属性表里不再有 handle 字段,句柄要在 ESP_GATTS_CREAT_ATTR_TAB_EVT 里接
//   * esp_ble_gatts_read_long_resp / esp_ble_gatts_prerelease 在 5.5 的公开 API 里没了,
//     所以配置只能分片走普通读写
#include "kbmic_ble_svc.h"

#include "kbmic_store.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_assert.h"
#include "esp_gatt_defs.h"
#include "esp_gatts_api.h"
#include "esp_hidd_gatts.h"
#include "esp_log.h"

static const char *TAG = "kbmic_svc";

// 不能与框架用的那几个 app_id 撞:
//   ESP_GATT_UUID_DEVICE_INFO_SVC=0x1800、BATTERY=0x180F、HID_SVC=0x1812。
#define KBMIC_SVC_APP_ID 0x2040

// 128 位 UUID 在 Bluedroid 里按小端存放,字面量要整体反着写。
// 可变的是倒数第 4 个字节(0x30=服务 / 0x40+i=第 i 个分片 / 0x4f=事件),
// 前面 12 字节与后面 3 字节都是固定的。
//   7d1c5a30-9f6e-4a21-8c3d-2b5e7a9f1c48  ← a=0x30
#define UUID128_LO(a)                                                            \
    {                                                                            \
        0x48, 0x1c, 0x9f, 0x7a, 0x5e, 0x2b, 0x3d, 0x8c, 0x21, 0x4a, 0x6e, 0x9f,    \
            (a), 0x5a, 0x1c, 0x7d                                                \
    }

// 分片 i 的 UUID 取 0x40 + i。客户端从 0x40 开始顺序探测,碰到缺口就停。
#define KBMIC_CHUNK_UUID_BASE 0x40

// esp_bt_uuid_t 是 {len, union{...}},不能直接拿 16 个字节的宏去初始化它。
#define UUID128_INIT(a) \
    { .len = ESP_UUID_LEN_128, .uuid.uuid128 = UUID128_LO(a) }

static esp_bt_uuid_t s_uuid_svc = UUID128_INIT(0x30);
// 主服务声明行的 uuid_p 必须是 0x2800:IDF 5.5 的建表循环只识别 16 位
// UUID 行,服务的真实 UUID 从该行的 value/length 里取(2026-10-05 真机
// 踩坑:服务行写成 128 位被 continue 跳过,服务永不创建,建表报 133)。
static uint16_t s_uuid_svc_decl = ESP_GATT_UUID_PRI_SERVICE;
static uint8_t s_evt_val[KBMIC_EV_MAX];   // 事件特征初值(AUTO_RSP 需要真实缓冲)
static esp_bt_uuid_t s_uuid_evt = UUID128_INIT(0x4f);
static esp_bt_uuid_t s_uuid_char_decl = {.len = ESP_UUID_LEN_16,
                                        .uuid.uuid16 = ESP_GATT_UUID_CHAR_DECLARE};
static esp_bt_uuid_t s_uuid_ccc = {.len = ESP_UUID_LEN_16,
                                   .uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG};
// 分片 i 的 UUID。必须活到建表之后,所以是文件作用域的静态数组。
static esp_bt_uuid_t s_chunk_uuid[KBMIC_SVC_CHUNKS];

// 属性表布局:服务 1 项 + 每分片 2 项(声明/值) + 事件 3 项(声明/值/CCC)。
enum {
    IDX_SVC = 0,
    IDX_CHUNK_BASE = 1,
    IDX_EVT_CHAR = IDX_CHUNK_BASE + KBMIC_SVC_CHUNKS * 2,
    IDX_EVT_VAL,
    IDX_EVT_CCC,
    IDX_NB,
};

static esp_gatt_if_t s_gatts_if;
static bool s_registered;
static bool s_connected;
static uint16_t s_conn_id;
static uint16_t s_evt_ccc;   // 事件特征的 CCC 值,非 0 表示已订阅(属性值 2 字节)

// 属性表由 esp_ble_gatts_create_attr_tab 异步建表,句柄在那之后才拿得到。
static uint16_t s_handle[IDX_NB];
static esp_gatts_attr_db_t s_attr[IDX_NB];
static uint8_t s_chunk_len[KBMIC_SVC_CHUNKS];
// CREAT_ATTR_TAB_EVT 的 handles 指向 BTC 内部静态数组，事件返回前必须深拷贝。
static uint16_t s_created_handles[IDX_NB];

// 写入暂存区。客户端把 N 个分片依次写进来,收齐且校验通过才提交。
// 直接写生效存储的话,一次写到一半断连就会留下半份配置,而 2300 字节里任何一个
// 字节错了都足以让设备认不出自己配过什么。
static kbmic_config_t s_staging;
static uint32_t s_staging_mask;

static uint8_t s_char_prop_read = ESP_GATT_CHAR_PROP_BIT_READ;
static uint8_t s_char_prop_notify = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;

// 分片数不能超过 32:暂存位图是 uint32。
// IDF 5.5 的 esp_assert.h 里已经没有 ESP_ASSERT 了,只有 ESP_STATIC_ASSERT。
ESP_STATIC_ASSERT(KBMIC_SVC_CHUNKS <= 32, "分片位图是 uint32,分片数不能超过 32");
// 属性表长度与枚举必须一致,否则事件里按 IDX_* 取句柄会取错位置。
ESP_STATIC_ASSERT(1 + KBMIC_SVC_CHUNKS * 2 + 3 == IDX_NB, "属性表长度与 IDX_* 不一致");

// 配置的正文存在 kbmic_config 里(kbmic_config_current(),地址运行期稳定)。
// 分片特征的 att_desc.value 直接指向它的对应偏移:Bluedroid 建表时只保存指针
// 不做深拷贝,所以配置一换,下一次读拿到的就是新值,不需要额外的读回调。
static uint8_t *cfg_store(void)
{
    return (uint8_t *)kbmic_config_current();
}

static int chunk_of_handle(uint16_t handle)
{
    for (uint8_t i = 0; i < KBMIC_SVC_CHUNKS; i++) {
        if (s_handle[IDX_CHUNK_BASE + 1 + i * 2] == handle) {
            return i;
        }
    }
    return -1;
}

// 组装属性表。必须在 ESP_GATTS_CREAT_ATTR_TAB_EVT 里、BLE 起来之后调用一次。
static void build_attr_table(void)
{
    for (uint8_t i = 0; i < KBMIC_SVC_CHUNKS; i++) {
        const uint16_t left = (uint16_t)(KBMIC_CONFIG_SIZE - i * KBMIC_SVC_CHUNK_SIZE);
        s_chunk_len[i] = (uint8_t)(left < KBMIC_SVC_CHUNK_SIZE ? left : KBMIC_SVC_CHUNK_SIZE);
    }

    uint8_t n = 0;

    // 服务本身:声明行(0x2800)+ 服务 UUID 放 value
    s_attr[n].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
    s_attr[n].att_desc.uuid_length = ESP_UUID_LEN_16;
    s_attr[n].att_desc.uuid_p = (uint8_t *)&s_uuid_svc_decl;
    s_attr[n].att_desc.perm = ESP_GATT_PERM_READ;
    s_attr[n].att_desc.max_length = ESP_UUID_LEN_128;
    s_attr[n].att_desc.length = ESP_UUID_LEN_128;
    s_attr[n].att_desc.value = s_uuid_svc.uuid.uuid128;
    n++;

    for (uint8_t i = 0; i < KBMIC_SVC_CHUNKS; i++) {
        s_chunk_uuid[i] = (esp_bt_uuid_t)UUID128_INIT((uint8_t)(KBMIC_CHUNK_UUID_BASE + i));

        s_attr[n].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
        s_attr[n].att_desc.uuid_length = ESP_UUID_LEN_16;
        s_attr[n].att_desc.uuid_p = (uint8_t *)&s_uuid_char_decl.uuid.uuid16;
        s_attr[n].att_desc.perm = ESP_GATT_PERM_READ;
        s_attr[n].att_desc.max_length = 1;
        s_attr[n].att_desc.length = 1;
        s_attr[n].att_desc.value = (uint8_t *)&s_char_prop_read;
        n++;

        s_attr[n].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
        s_attr[n].att_desc.uuid_length = ESP_UUID_LEN_128;
        s_attr[n].att_desc.uuid_p = s_chunk_uuid[i].uuid.uuid128;
        s_attr[n].att_desc.perm = ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE;
        s_attr[n].att_desc.max_length = KBMIC_SVC_CHUNK_SIZE;
        s_attr[n].att_desc.length = s_chunk_len[i];
        s_attr[n].att_desc.value = cfg_store() + i * KBMIC_SVC_CHUNK_SIZE;
        n++;
    }

    s_attr[n].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
    s_attr[n].att_desc.uuid_length = ESP_UUID_LEN_16;
    s_attr[n].att_desc.uuid_p = (uint8_t *)&s_uuid_char_decl.uuid.uuid16;
    s_attr[n].att_desc.perm = ESP_GATT_PERM_READ;
    s_attr[n].att_desc.max_length = 1;
    s_attr[n].att_desc.length = 1;
    s_attr[n].att_desc.value = (uint8_t *)&s_char_prop_notify;
    n++;

    s_attr[n].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
    s_attr[n].att_desc.uuid_length = ESP_UUID_LEN_128;
    s_attr[n].att_desc.uuid_p = s_uuid_evt.uuid.uuid128;
    s_attr[n].att_desc.perm = ESP_GATT_PERM_READ;
    s_attr[n].att_desc.max_length = KBMIC_EV_MAX;
    s_attr[n].att_desc.length = 0;
    s_attr[n].att_desc.value = s_evt_val;
    n++;

    s_attr[n].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
    s_attr[n].att_desc.uuid_length = ESP_UUID_LEN_16;
    s_attr[n].att_desc.uuid_p = (uint8_t *)&s_uuid_ccc.uuid.uuid16;
    s_attr[n].att_desc.perm = ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE;
    s_attr[n].att_desc.max_length = 2;
    s_attr[n].att_desc.length = 2;
    s_attr[n].att_desc.value = (uint8_t *)&s_evt_ccc;
    n++;

}

static void commit_staging(void)
{
    if (!kbmic_config_valid(&s_staging)) {
        ESP_LOGW(TAG, "收到的配置未通过校验,丢弃(已写 %u/%u 片)",
                 (unsigned)__builtin_popcount(s_staging_mask), KBMIC_SVC_CHUNKS);
        s_staging_mask = 0;
        return;
    }
    s_staging_mask = 0;
    kbmic_config_commit(&s_staging);

    const kbmic_config_t *cur = kbmic_config_current();
    kbmic_ble_svc_notify(KBMIC_EV_CONFIG_SAVED, cur->active, 0, cur->profiles[cur->active].name);
}

// ---------------------------------------------------------------------------
// 服务事件
// ---------------------------------------------------------------------------
static void svc_event(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                     esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "注册 GATT app 失败: %d", param->reg.status);
            return;
        }
        s_gatts_if = gatts_if;
        s_registered = true;
        ESP_LOGI(TAG, "GATT app 注册成功 gatts_if=%u", gatts_if);
        build_attr_table();
        const esp_err_t create_ret = esp_ble_gatts_create_attr_tab(
            s_attr, gatts_if, IDX_NB, 0);
        if (create_ret != ESP_OK) {
            ESP_LOGE(TAG, "创建属性表请求失败: %s", esp_err_to_name(create_ret));
        }
        break;

    case ESP_GATTS_CREATE_EVT:
        // 该事件只属于 create_service() 流程；属性表服务不会走到这里。
        if (param->create.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "创建服务失败: %d", param->create.status);
            return;
        }
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT: {
        // 句柄只在这一步才拿得到,必须整段拷出来:param->handles 是协议栈的临时
        // 缓冲,事件返回后就没了。
        if (param->add_attr_tab.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "建表失败: status=%d gatts_if=%u num=%u",
                     param->add_attr_tab.status, gatts_if,
                     param->add_attr_tab.num_handle);
            return;
        }
        const uint16_t got = param->add_attr_tab.num_handle;
        ESP_LOGI(TAG, "属性表就绪 num=%u svc_handle=%u first=%u last=%u",
                 got, param->add_attr_tab.handles[0],
                 param->add_attr_tab.handles[0],
                 param->add_attr_tab.handles[got ? got - 1 : 0]);
        for (uint16_t i = 0; i < got && i < IDX_NB; i++) {
            s_handle[i] = param->add_attr_tab.handles[i];
        }
        if (got != IDX_NB) {
            ESP_LOGE(TAG, "属性表数量异常: got=%u expected=%u", got, IDX_NB);
            return;
        }
        const esp_err_t start_ret = esp_ble_gatts_start_service(s_handle[IDX_SVC]);
        if (start_ret != ESP_OK) {
            ESP_LOGE(TAG, "启动配置服务失败: %s", esp_err_to_name(start_ret));
        }
        break;
    }

    case ESP_GATTS_START_EVT:
        if (param->start.status == ESP_GATT_OK) {
            ESP_LOGI(TAG, "配置服务就绪 handle=%u:%u 个分片 × %u 字节 = %u 字节",
                     param->start.service_handle, KBMIC_SVC_CHUNKS,
                     KBMIC_SVC_CHUNK_SIZE, KBMIC_CONFIG_SIZE);
        } else {
            ESP_LOGE(TAG, "配置服务启动事件失败: status=%d handle=%u",
                     param->start.status, param->start.service_handle);
        }
        break;

    case ESP_GATTS_CONNECT_EVT:
        s_conn_id = param->connect.conn_id;
        s_connected = true;
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        s_connected = false;
        s_evt_ccc = 0;
        s_staging_mask = 0;   // 断开后残留的半份写入没有意义,丢掉
        break;

    case ESP_GATTS_WRITE_EVT: {
        if (param->write.handle == s_handle[IDX_EVT_CCC]) {
            s_evt_ccc = param->write.value[0];
            break;
        }
        const int chunk = chunk_of_handle(param->write.handle);
        if (chunk < 0) {
            break;   // 不是我们的特征
        }
        if (param->write.is_prep) {
            // 分片设计就是为了绕开长写,客户端不该走到这里。
            ESP_LOGW(TAG, "拒绝 prepare write(分片协议不支持)");
            break;
        }
        if (param->write.offset != 0 || param->write.len > s_chunk_len[chunk]) {
            ESP_LOGW(TAG, "分片 %d 写入越界 offset=%u len=%u", chunk,
                     param->write.offset, param->write.len);
            break;
        }
        memcpy(((uint8_t *)&s_staging) + chunk * KBMIC_SVC_CHUNK_SIZE, param->write.value,
               param->write.len);
        s_staging_mask |= (1U << chunk);
        if (s_staging_mask == ((1U << KBMIC_SVC_CHUNKS) - 1)) {
            commit_staging();
        }
        break;
    }

    case ESP_GATTS_READ_EVT:
        // 配置分片不需要读回调:att_desc.value 已指向生效配置,Bluedroid 直接取。
        // 事件特征没有可读内容,明确回一个空响应,免得主机一直等。
        if (param->read.handle == s_handle[IDX_EVT_VAL] && param->read.need_rsp) {
            esp_gatt_rsp_t rsp = {.handle = param->read.handle};
            esp_ble_gatts_send_response(s_gatts_if, param->read.conn_id, param->read.trans_id,
                                        ESP_GATT_OK, &rsp);
        }
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// 全局 GATT 回调转发
// ---------------------------------------------------------------------------
// 配置服务的 GATTS 事件在 esp_hid 的 "ble_hidd_events" 任务里回调,那个
// 任务栈只有 4KB —— 建表链(尤其 v2 的 15 片属性表)在里面必然栈溢出
// (2026-10-06 真机踩坑,Stack protection fault 循环重启)。所以自家事件
// 只拷贝参数入队,真正的处理放到本模块自己的大栈任务里,顺序不变。
typedef struct {
    esp_gatts_cb_event_t event;
    esp_gatt_if_t gatts_if;
    esp_ble_gatts_cb_param_t *param;   // dispatch 里 malloc 的事件参数拷贝
} svc_msg_t;

static QueueHandle_t s_svc_queue;

// worker 的私有命令:0xFF = 执行 kbmic_ble_svc_init(GATTS app 注册)。
#define SVC_CMD_INIT ((esp_gatts_cb_event_t)0xFF)

static void svc_worker(void *arg)
{
    (void)arg;
    svc_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_svc_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (msg.event == SVC_CMD_INIT) {
            kbmic_ble_svc_init();
            continue;
        }
        svc_event(msg.event, msg.gatts_if, msg.param);
        free(msg.param);
    }
}

static void dispatch(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                     esp_ble_gatts_cb_param_t *param)
{
    // 自己的 app 自己处理,**不**再转给框架:框架只认它自己注册的那几个 gatts_if,
    // 转过去只会在它的日志里刷 "Unknown gatts_if"。反过来框架的事件也不能被
    // 我们吃掉 —— HID 报告特征就是靠它完成的。
    //
    // REG 事件要按 app_id 分流:自己的 gatts_if 是在这条事件里才发下来的,
    // 用 gatts_if 判断永远接不到自己的 REG(鸡生蛋),服务就永远注册不上
    // (2026-10-05 真机踩坑,日志表现为 BLE_HIDD "Unknown Application, 0x2040")。
    const bool ours = (event == ESP_GATTS_REG_EVT)
                          ? (param->reg.app_id == KBMIC_SVC_APP_ID)
                          : (s_registered && gatts_if == s_gatts_if);
    if (!ours) {
        esp_hidd_gatts_event_handler(event, gatts_if, param);
        return;
    }
    if (s_svc_queue == NULL) {
        svc_event(event, gatts_if, param);   // 队列未建(理论不可达)就原地跑
        return;
    }
    esp_ble_gatts_cb_param_t *copy = malloc(sizeof(*copy));
    if (copy == NULL) {
        ESP_LOGE(TAG, "事件参数拷贝失败,事件 %d 丢弃", event);
        return;
    }
    memcpy(copy, param, sizeof(*copy));
    if (event == ESP_GATTS_CREAT_ATTR_TAB_EVT) {
        const uint16_t count = param->add_attr_tab.num_handle;
        if (param->add_attr_tab.handles != NULL) {
            memcpy(s_created_handles, param->add_attr_tab.handles,
                   count < IDX_NB ? count * sizeof(s_created_handles[0])
                                  : sizeof(s_created_handles));
        }
        copy->add_attr_tab.handles = s_created_handles;
    }
    const svc_msg_t msg = {.event = event, .gatts_if = gatts_if, .param = copy};
    xQueueSend(s_svc_queue, &msg, 0);
}

esp_err_t kbmic_ble_svc_install_dispatch(void)
{
    // 时机约束:必须在 Bluedroid enable 之后、任何 GATTS app 注册之前调用。
    // 放早了(Bluedroid 还没起)esp_ble_gatts_register_callback 会静默失败,
    // 之后所有 GATTS 事件(包括 esp_hid 的 REG/建表事件)无人接收,
    // HID 服务永远建不起来,设备也就永远不会开始广播(2026-10-05 真机踩坑)。
    esp_err_t ret = esp_ble_gatts_register_callback(dispatch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GATTS 分发回调注册失败: %s", esp_err_to_name(ret));
        return ret;
    }
    if (s_svc_queue == NULL) {
        s_svc_queue = xQueueCreate(16, sizeof(svc_msg_t));
        if (s_svc_queue == NULL ||
            xTaskCreate(svc_worker, "kbmic_svc", 6144, NULL, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "svc 任务创建失败");
            return ESP_ERR_NO_MEM;
        }
    }
    return ret;
}

// ---------------------------------------------------------------------------
// 对外
// ---------------------------------------------------------------------------
void kbmic_ble_svc_request_register(void)
{
    const svc_msg_t msg = {.event = SVC_CMD_INIT, .gatts_if = 0, .param = NULL};
    if (s_svc_queue != NULL) {
        xQueueSend(s_svc_queue, &msg, 0);
    }
}

esp_err_t kbmic_ble_svc_init(void)
{
    // 配置先进内存,GATT 建表时才能把特征值指到正确的内容上。
    kbmic_config_load();

    const esp_err_t ret = esp_ble_gatts_app_register(KBMIC_SVC_APP_ID);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册 GATT app 失败: %s", esp_err_to_name(ret));
    }
    return ret;
}

void kbmic_ble_svc_notify(uint8_t type, uint8_t active, uint8_t aux, const char *name)
{
    if (!s_registered || !s_connected || s_evt_ccc == 0) {
        return;   // 没人订阅是常态,不是错误
    }
    uint8_t buf[KBMIC_EV_MAX];
    const size_t name_len = (name && *name) ? strnlen(name, KBMIC_EV_MAX - 5) : 0;
    buf[0] = type;
    buf[1] = active;
    buf[2] = aux;
    buf[3] = 0;
    buf[4] = (uint8_t)name_len;
    if (name_len) {
        memcpy(&buf[5], name, name_len);
    }
    const esp_err_t ret = esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id, s_handle[IDX_EVT_VAL],
                                                      5 + name_len, buf, false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "事件通知发送失败: %s", esp_err_to_name(ret));
    }
}
