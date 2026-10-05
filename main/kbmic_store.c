// main/kbmic_store.c —— 配置的持久化与生效副本。
//
// 纯逻辑(出厂默认、合法性、增删、动作名)在 kbmic_model.c —— 那边不碰任何
// ESP-IDF 头,可以在主机上编译跑单测(kbmic_config.h)。这一半只管"把这份
// 结构体存进 NVS、读出来、换掉生效副本"。
//
// 这份结构体同时是 BLE 报文和 NVS 的载荷,所以任何写入都必须先过
// kbmic_config_valid(),否则已经配过对的设备会读出一份自己都不认识的配置。
#include "kbmic_store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "kbmic_config";

// ---------------------------------------------------------------------------
// 生效配置的唯一副本
//
// 放这里而不是 BLE 服务里,是因为"当前配置"有两类写者(界面和 MCP),
// 谁都不该持有别人正在改的那份内存。存储是内部静态的,地址运行期不变,
// BLE 分片特征的值指针指着它 —— 整块换内容不影响指针。
// ---------------------------------------------------------------------------
static kbmic_config_t s_current;
static kbmic_config_hook_t s_hook;

// 不要叫 NVS_NS:esp-idf 的 nvs.h 里已经有一个同名宏,顶掉它会让所有
// NVS 调用的命名空间静默变错。
#define KBMIC_NVS "kbmic"
#define NVS_KEY "cfg"

// ---------------------------------------------------------------------------
// NVS
//
// IDF 5.5 的 NVS 改成了句柄式(5.3 及以前是 nvs_get_blob(ns, key, ...)):
// 先 nvs_open 拿句柄,再拿句柄读写。句柄缓存起来复用,省得每次写都开关一次。
// ---------------------------------------------------------------------------
static nvs_handle_t s_nvs;

static esp_err_t nvs_open_cached(void)
{
    if (s_nvs != 0) {
        return ESP_OK;
    }
    const esp_err_t ret = nvs_open(KBMIC_NVS, NVS_READWRITE, &s_nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS 失败: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t kbmic_config_load(void)
{
    kbmic_config_t tmp;
    size_t len = sizeof(tmp);

    if (nvs_open_cached() == ESP_OK) {
        const esp_err_t ret = nvs_get_blob(s_nvs, NVS_KEY, &tmp, &len);
        if (ret == ESP_OK) {
            if (len == sizeof(tmp) && kbmic_config_valid(&tmp)) {
                s_current = tmp;
                ESP_LOGI(TAG, "配置已载入:%u 个模式,当前 %s", s_current.count,
                         s_current.profiles[s_current.active].name);
                return ESP_OK;
            }
            ESP_LOGW(TAG, "NVS 里的配置无效(版本/长度/字段),改用默认值");
        } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "读 NVS 失败: %s", esp_err_to_name(ret));
        }
    }

    kbmic_config_defaults(&s_current);
    // 顺手把默认值落盘,下次开机就不用再走这条路。失败不阻断:配置存不进去
    // 不该让键盘瘫掉,只是每次开机都要重来一遍而已。
    (void)kbmic_config_save(&s_current);
    return ESP_OK;
}

esp_err_t kbmic_config_save(const kbmic_config_t *cfg)
{
    if (!kbmic_config_valid(cfg)) {
        ESP_LOGE(TAG, "拒绝保存非法配置");
        return ESP_ERR_INVALID_ARG;
    }
    if (nvs_open_cached() != ESP_OK) {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    const esp_err_t ret = nvs_set_blob(s_nvs, NVS_KEY, cfg, sizeof(*cfg));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "写 NVS 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    return nvs_commit(s_nvs);
}

const kbmic_config_t *kbmic_config_current(void)
{
    return &s_current;
}

void kbmic_config_set_hook(kbmic_config_hook_t hook)
{
    s_hook = hook;
}

esp_err_t kbmic_config_commit(const kbmic_config_t *cfg)
{
    if (!kbmic_config_valid(cfg)) {
        ESP_LOGE(TAG, "拒绝提交非法配置");
        return ESP_ERR_INVALID_ARG;
    }
    s_current = *cfg;

    const esp_err_t ret = kbmic_config_save(&s_current);
    if (ret != ESP_OK) {
        // 落盘失败不回滚:内存里已经是新配置了,回滚会让界面和实际行为对不上。
        // 老实说"这次改动没存住,重启会丢",比默默回滚更容易排查。
        ESP_LOGE(TAG, "新配置已生效但落盘失败,重启后会回到旧配置: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "配置已更新:%u 个模式,当前 %s", s_current.count,
                 s_current.profiles[s_current.active].name);
    }
    if (s_hook) {
        s_hook();
    }
    return ret;
}
