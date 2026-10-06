// main/kbmic_mcp.c —— 设备侧 MCP 工具实现。
//
// 工具语义与 mcp_server/(BLE 桥)对齐;WiFi 管理与模拟触发是网络侧独有。
// 配置改动一律走 kbmic_config_commit(唯一生效路径,提交后 BLE 分片特征
// 指针指向的内容同步更新,无需额外通知)。
#include "kbmic_mcp.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "appfw_mcp.h"
#include "appfw_net.h"
#include "appfw_portal.h"
#include "appfw_netlist.h"
#include "appfw_storage.h"
#include "appfw_ui.h"   // APPFW_MENU_ITEM_* 使能位(只用位值,不接 UI)
#include "bsp_battery.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "kbmic_action.h"
#include "kbmic_hid.h"
#include "esp_gap_ble_api.h"
#include "esp_wifi.h"
#include "kbmic_store.h"

static const char *TAG = "kbmic_mcp";

static kbmic_sim_fn_t s_sim;

void kbmic_mcp_set_simulate(kbmic_sim_fn_t fn)
{
    s_sim = fn;
}

// ---------------------------------------------------------------------------
// 文本辅助
// ---------------------------------------------------------------------------
static const char *btn_name(int b)
{
    switch (b) {
    case KBMIC_BTN_UP:   return "Up";
    case KBMIC_BTN_DOWN: return "Down";
    default:             return "OK";
    }
}

static const char *slot_name(int s)
{
    switch (s) {
    case KBMIC_SLOT_DOUBLE: return "double-click";
    case KBMIC_SLOT_LONG:   return "long-press";
    default:                return "short-tap";
    }
}

// ---------------------------------------------------------------------------
// JSON ↔ 动作
// ---------------------------------------------------------------------------
static int parse_trigger(const cJSON *j)
{
    if (cJSON_IsNumber(j)) {
        const int v = j->valueint;
        return (v >= KBMIC_TRIG_NONE && v <= KBMIC_TRIG_DOUBLE) ? v : -1;
    }
    if (!cJSON_IsString(j) || !j->valuestring) return -1;
    const char *s = j->valuestring;
    if (!strcasecmp(s, "none"))  return KBMIC_TRIG_NONE;
    if (!strcasecmp(s, "click")) return KBMIC_TRIG_CLICK;
    if (!strcasecmp(s, "tap"))   return KBMIC_TRIG_TAP;
    if (!strcasecmp(s, "long"))  return KBMIC_TRIG_LONG;
    if (!strcasecmp(s, "double")) return KBMIC_TRIG_DOUBLE;
    return -1;
}

static int parse_step_kind(const cJSON *j)
{
    if (cJSON_IsNumber(j)) {
        const int v = j->valueint;
        return (v >= KBMIC_STEP_NONE && v <= KBMIC_STEP_APPLEFN) ? v : -1;
    }
    if (!cJSON_IsString(j) || !j->valuestring) return -1;
    const char *s = j->valuestring;
    if (!strcasecmp(s, "none"))     return KBMIC_STEP_NONE;
    if (!strcasecmp(s, "key"))      return KBMIC_STEP_KEY;
    if (!strcasecmp(s, "consumer")) return KBMIC_STEP_CONSUMER;
    if (!strcasecmp(s, "delay"))    return KBMIC_STEP_DELAY;
    if (!strcasecmp(s, "applefn"))  return KBMIC_STEP_APPLEFN;
    return -1;
}

// "Ctrl+Shift" / "win" 这样的修饰键串 → KBMIC_MOD_* 位或;纯数字原样。
static int parse_mods(const cJSON *j)
{
    if (cJSON_IsNumber(j)) {
        const int v = j->valueint;
        return (v >= 0 && v <= 0xFF) ? v : -1;
    }
    if (!cJSON_IsString(j) || !j->valuestring) return -1;
    int out = 0;
    char buf[40];
    snprintf(buf, sizeof(buf), "%s", j->valuestring);
    char *save = NULL;
    for (char *tok = strtok_r(buf, "+ ", &save); tok; tok = strtok_r(NULL, "+ ", &save)) {
        if (!strcasecmp(tok, "ctrl") || !strcasecmp(tok, "control")) out |= KBMIC_MOD_CTRL;
        else if (!strcasecmp(tok, "shift"))                          out |= KBMIC_MOD_SHIFT;
        else if (!strcasecmp(tok, "alt") || !strcasecmp(tok, "option")) out |= KBMIC_MOD_ALT;
        else if (!strcasecmp(tok, "win") || !strcasecmp(tok, "gui") ||
                 !strcasecmp(tok, "cmd") || !strcasecmp(tok, "meta") ||
                 !strcasecmp(tok, "super"))                          out |= KBMIC_MOD_GUI;
        else return -1;
    }
    return out;
}

bool kbmic_action_from_json(const cJSON *j, kbmic_action_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!cJSON_IsObject(j)) return false;
    const int trig = parse_trigger(cJSON_GetObjectItemCaseSensitive(j, "trigger"));
    if (trig < 0) return false;
    out->trigger = (uint8_t)trig;

    const cJSON *steps = cJSON_GetObjectItemCaseSensitive(j, "steps");
    if (!steps) return true;   // 缺省 = 空动作
    if (!cJSON_IsArray(steps)) return false;

    int n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, steps) {
        if (n >= KBMIC_SEQ_MAX) return false;
        const cJSON *kind_j = cJSON_GetObjectItemCaseSensitive(it, "kind");
        const int kind = kind_j ? parse_step_kind(kind_j) : KBMIC_STEP_NONE;
        if (kind < 0) return false;
        kbmic_step_t *st = &out->steps[n];
        st->kind = (uint8_t)kind;
        if (kind == KBMIC_STEP_KEY) {
            const int mods = parse_mods(cJSON_GetObjectItemCaseSensitive(it, "mods"));
            const cJSON *kc = cJSON_GetObjectItemCaseSensitive(it, "keycode");
            if (mods < 0 || !cJSON_IsNumber(kc) ||
                kc->valueint < 0 || kc->valueint > 0xFF) return false;
            st->mods = (uint8_t)mods;
            st->keycode = (uint8_t)kc->valueint;
        } else if (kind == KBMIC_STEP_CONSUMER) {
            const cJSON *us = cJSON_GetObjectItemCaseSensitive(it, "usage");
            if (!cJSON_IsNumber(us) || us->valueint < 0 || us->valueint > 0xFFFF) return false;
            st->usage = (uint16_t)us->valueint;
        } else if (kind == KBMIC_STEP_DELAY) {
            const cJSON *ms = cJSON_GetObjectItemCaseSensitive(it, "delay_ms");
            if (!cJSON_IsNumber(ms) || ms->valueint < 0 || ms->valueint > 0xFFFF) return false;
            st->delay_ms = (uint16_t)ms->valueint;
        }
        n++;
    }
    out->step_count = (uint8_t)n;
    return true;
}

void kbmic_action_to_json(const kbmic_action_t *a, cJSON *out)
{
    static const char *kind_n[] = { "none", "key", "consumer", "delay", "applefn" };
    cJSON_AddNumberToObject(out, "trigger", a->trigger);
    char name[KBMIC_ACTION_NAME_MAX];
    kbmic_action_name(a, name, sizeof(name));
    cJSON_AddStringToObject(out, "display", name);
    cJSON *steps = cJSON_AddArrayToObject(out, "steps");
    for (int i = 0; i < a->step_count && i < KBMIC_SEQ_MAX; i++) {
        const kbmic_step_t *st = &a->steps[i];
        cJSON *s = cJSON_CreateObject();
        cJSON_AddNumberToObject(s, "kind", st->kind);
        cJSON_AddStringToObject(s, "kind_name",
                                st->kind <= KBMIC_STEP_APPLEFN ? kind_n[st->kind] : "unknown");
        if (st->kind == KBMIC_STEP_KEY) {
            cJSON_AddNumberToObject(s, "mods", st->mods);
            cJSON_AddNumberToObject(s, "keycode", st->keycode);
        } else if (st->kind == KBMIC_STEP_CONSUMER) {
            cJSON_AddNumberToObject(s, "usage", st->usage);
        } else if (st->kind == KBMIC_STEP_DELAY) {
            cJSON_AddNumberToObject(s, "delay_ms", st->delay_ms);
        }
        cJSON_AddItemToArray(steps, s);
    }
}

// ---------------------------------------------------------------------------
// 配置工具
// ---------------------------------------------------------------------------
void kbmic_mcp_add_slots_json(cJSON *arr, const kbmic_profile_t *p)
{
    for (int b = 0; b < KBMIC_BTN_COUNT; b++) {
        for (int s = 0; s < KBMIC_SLOT_COUNT; s++) {
            cJSON *slot = cJSON_CreateObject();
            cJSON_AddStringToObject(slot, "button", btn_name(b));
            cJSON_AddNumberToObject(slot, "button_index", b);
            cJSON_AddStringToObject(slot, "slot", slot_name(s));
            cJSON_AddNumberToObject(slot, "slot_index", s);
            cJSON *act = cJSON_CreateObject();
            kbmic_action_to_json(&p->slots[b][s], act);
            cJSON_AddItemToObject(slot, "action", act);
            cJSON_AddItemToArray(arr, slot);
        }
    }
}

static int tool_get_config(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    const kbmic_config_t *cfg = kbmic_config_current();
    appfw_mcp_resp_addf(resp, "配置 v%u,%u 个模式,当前 #%u %s:",
                        cfg->version, cfg->count, cfg->active,
                        cfg->profiles[cfg->active].name);
    for (uint8_t i = 0; i < cfg->count && i < KBMIC_MAX_PROFILES; i++) {
        const kbmic_profile_t *p = &cfg->profiles[i];
        appfw_mcp_resp_addf(resp, "\n[%d]%s%s", i, p->name,
                            i == cfg->active ? "(使用中)" : (p->builtin ? "(内置)" : ""));
        char name[KBMIC_ACTION_NAME_MAX];
        for (int b = 0; b < KBMIC_BTN_COUNT; b++) {
            appfw_mcp_resp_addf(resp, "\n  %s 短按=%s",
                                btn_name(b),
                                kbmic_action_name(&p->slots[b][KBMIC_SLOT_TAP], name, sizeof(name)));
            appfw_mcp_resp_addf(resp, " 长按=%s",
                                kbmic_action_name(&p->slots[b][KBMIC_SLOT_LONG], name, sizeof(name)));
        }
    }
    appfw_mcp_resp_addf(resp, "\n用 kbmic_set_key(index,button,slot,preset|steps) 修改;");
    return 0;
}

static int tool_set_active_mode(cJSON *args, appfw_mcp_resp_t *resp)
{
    const cJSON *idx = cJSON_GetObjectItemCaseSensitive(args, "index");
    if (!cJSON_IsNumber(idx)) {
        appfw_mcp_resp_addf(resp, "参数 index(number)缺失");
        return 1;
    }
    kbmic_config_t work = *kbmic_config_current();
    if (idx->valueint < 0 || idx->valueint >= work.count) {
        appfw_mcp_resp_addf(resp, "index 越界(0..%d)", work.count - 1);
        return 1;
    }
    work.active = (uint8_t)idx->valueint;
    kbmic_config_commit(&work);
    appfw_mcp_resp_addf(resp, "已切换到模式 %s", work.profiles[work.active].name);
    return 0;
}

static int tool_add_mode(cJSON *args, appfw_mcp_resp_t *resp)
{
    const cJSON *nm = cJSON_GetObjectItemCaseSensitive(args, "name");
    const char *name = (cJSON_IsString(nm) && nm->valuestring[0]) ? nm->valuestring : "自定义";
    kbmic_config_t work = *kbmic_config_current();
    const int idx = kbmic_config_add_profile(&work, name);
    if (idx < 0) {
        appfw_mcp_resp_addf(resp, "添加失败:模式已满(%d)", KBMIC_MAX_PROFILES);
        return 1;
    }
    work.active = (uint8_t)idx;
    kbmic_config_commit(&work);
    appfw_mcp_resp_addf(resp, "已添加并切换到模式 [%d]%s", idx, work.profiles[idx].name);
    return 0;
}

static int tool_delete_mode(cJSON *args, appfw_mcp_resp_t *resp)
{
    const cJSON *idx = cJSON_GetObjectItemCaseSensitive(args, "index");
    if (!cJSON_IsNumber(idx)) {
        appfw_mcp_resp_addf(resp, "参数 index(number)缺失");
        return 1;
    }
    kbmic_config_t work = *kbmic_config_current();
    if (kbmic_config_delete_profile(&work, (uint8_t)idx->valueint) != 0) {
        appfw_mcp_resp_addf(resp, "删除失败:越界或内置模式不可删");
        return 1;
    }
    if (work.active >= work.count) work.active = work.count ? (uint8_t)(work.count - 1) : 0;
    kbmic_config_commit(&work);
    appfw_mcp_resp_addf(resp, "已删除,现存 %u 个模式", work.count);
    return 0;
}

static int tool_rename_mode(cJSON *args, appfw_mcp_resp_t *resp)
{
    const cJSON *idx = cJSON_GetObjectItemCaseSensitive(args, "index");
    const cJSON *nm = cJSON_GetObjectItemCaseSensitive(args, "name");
    if (!cJSON_IsNumber(idx) || !cJSON_IsString(nm) || !nm->valuestring[0]) {
        appfw_mcp_resp_addf(resp, "参数 index(number)/name(string)缺失");
        return 1;
    }
    kbmic_config_t work = *kbmic_config_current();
    if (idx->valueint < 0 || idx->valueint >= work.count) {
        appfw_mcp_resp_addf(resp, "index 越界");
        return 1;
    }
    snprintf(work.profiles[idx->valueint].name, KBMIC_NAME_MAX, "%s", nm->valuestring);
    kbmic_config_commit(&work);
    appfw_mcp_resp_addf(resp, "模式 %d 已改名 %s", idx->valueint, nm->valuestring);
    return 0;
}

static int tool_reset_mode(cJSON *args, appfw_mcp_resp_t *resp)
{
    const cJSON *idx = cJSON_GetObjectItemCaseSensitive(args, "index");
    if (!cJSON_IsNumber(idx)) {
        appfw_mcp_resp_addf(resp, "参数 index(number)缺失");
        return 1;
    }
    kbmic_config_t work = *kbmic_config_current();
    if (kbmic_config_reset_profile(&work, (uint8_t)idx->valueint) != 0) {
        appfw_mcp_resp_addf(resp, "重置失败:仅内置模式(0..%d)有出厂值", KBMIC_BUILTIN_MODES - 1);
        return 1;
    }
    kbmic_config_commit(&work);
    appfw_mcp_resp_addf(resp, "模式 %d 已恢复默认", idx->valueint);
    return 0;
}

static int tool_set_key(cJSON *args, appfw_mcp_resp_t *resp)
{
    const cJSON *idx = cJSON_GetObjectItemCaseSensitive(args, "index");
    const cJSON *btn = cJSON_GetObjectItemCaseSensitive(args, "button");
    const cJSON *slot = cJSON_GetObjectItemCaseSensitive(args, "slot");
    if (!cJSON_IsNumber(idx) || !cJSON_IsNumber(btn) || !cJSON_IsNumber(slot)) {
        appfw_mcp_resp_addf(resp, "参数 index/button/slot(number)缺失"
                                  "(button:0=Up 1=Down 2=OK;slot:0=短按 1=双击 2=长按)");
        return 1;
    }
    const cJSON *preset = cJSON_GetObjectItemCaseSensitive(args, "preset");
    const cJSON *steps_j = cJSON_GetObjectItemCaseSensitive(args, "steps");
    const cJSON *trig_j = cJSON_GetObjectItemCaseSensitive(args, "trigger");
    if (preset && steps_j) {
        appfw_mcp_resp_addf(resp, "preset 与 steps 只能给一个");
        return 1;
    }

    kbmic_action_t action;
    if (steps_j) {
        if (!kbmic_action_from_json(args, &action)) {   // steps 在动作对象同一层
            appfw_mcp_resp_addf(resp, "steps 解析失败(kind/mods/keycode/usage/delay_ms)");
            return 1;
        }
        if (trig_j) {
            const int t = parse_trigger(trig_j);
            if (t < 0) {
                appfw_mcp_resp_addf(resp, "trigger 非法(none/click/tap/long/double)");
                return 1;
            }
            action.trigger = (uint8_t)t;
        }
    } else if (preset) {
        const kbmic_catalog_item_t *item = NULL;
        if (cJSON_IsNumber(preset)) {
            item = kbmic_catalog_get((uint8_t)preset->valueint);
        } else if (cJSON_IsString(preset) && preset->valuestring[0]) {
            for (uint8_t i = 0; i < kbmic_catalog_count(); i++) {
                const kbmic_catalog_item_t *it = kbmic_catalog_get(i);
                if (!strcasecmp(it->name, preset->valuestring)) { item = it; break; }
            }
        }
        if (!item) {
            appfw_mcp_resp_addf(resp, "preset 不在目录里(用 kbmic_action_catalog 查看)");
            return 1;
        }
        action = item->action;
        if (trig_j) {
            const int t = parse_trigger(trig_j);
            if (t < 0) {
                appfw_mcp_resp_addf(resp, "trigger 非法(none/click/tap/long/double)");
                return 1;
            }
            action.trigger = (uint8_t)t;
        }
    } else {
        appfw_mcp_resp_addf(resp, "必须给 preset(目录 id/名字)或 steps");
        return 1;
    }

    // 普通动作在长按槽强制 LONG 触发:否则阈值到点后没人收尾,按住不放会一直重复。
    // Apple Fn/Consumer 是“按下保持、松手释放”的 TAP 语义,必须保留原触发。
    if (slot->valueint == KBMIC_SLOT_LONG &&
        action.trigger == KBMIC_TRIG_TAP &&
        !(action.step_count == 1 &&
          (action.steps[0].kind == KBMIC_STEP_APPLEFN ||
           action.steps[0].kind == KBMIC_STEP_CONSUMER))) {
        action.trigger = KBMIC_TRIG_LONG;
    }
    // 双击槽同理强制 DOUBLE(动作本体照发,只是触发语义跟槽走)。
    if (slot->valueint == KBMIC_SLOT_DOUBLE) {
        action.trigger = KBMIC_TRIG_DOUBLE;
    }
    kbmic_config_t work = *kbmic_config_current();
    if (idx->valueint < 0 || idx->valueint >= work.count ||
        btn->valueint < 0 || btn->valueint >= KBMIC_BTN_COUNT ||
        slot->valueint < 0 || slot->valueint >= KBMIC_SLOT_COUNT) {
        appfw_mcp_resp_addf(resp, "index/button/slot 越界");
        return 1;
    }
    work.profiles[idx->valueint].slots[btn->valueint][slot->valueint] = action;
    kbmic_config_commit(&work);

    char name[KBMIC_ACTION_NAME_MAX];
    appfw_mcp_resp_addf(resp, "模式 [%d]%s %s/%s 已设为 %s",
                        idx->valueint, work.profiles[idx->valueint].name,
                        btn_name(btn->valueint), slot_name(slot->valueint),
                        kbmic_action_name(&action, name, sizeof(name)));
    return 0;
}

static int tool_catalog(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    appfw_mcp_resp_addf(resp, "内置动作目录(id: 名字):");
    for (uint8_t i = 0; i < kbmic_catalog_count(); i++) {
        appfw_mcp_resp_addf(resp, "\n%d: %s", i, kbmic_catalog_get(i)->name);
    }
    return 0;
}

static int tool_simulate(cJSON *args, appfw_mcp_resp_t *resp)
{
    const cJSON *btn = cJSON_GetObjectItemCaseSensitive(args, "button");
    const cJSON *press = cJSON_GetObjectItemCaseSensitive(args, "press");
    if (!cJSON_IsNumber(btn) || btn->valueint < 0 || btn->valueint >= KBMIC_BTN_COUNT) {
        appfw_mcp_resp_addf(resp, "参数 button(number,0=Up 1=Down 2=OK)缺失");
        return 1;
    }
    int kind = 0;   // 0=短按 1=长按 2=双击
    if (cJSON_IsString(press) && !strcasecmp(press->valuestring, "long")) {
        kind = 1;
    } else if (cJSON_IsString(press) && !strcasecmp(press->valuestring, "double")) {
        kind = 2;
    } else if (cJSON_IsNumber(press)) {
        kind = press->valueint;
    }
    if (kind < 0 || kind > 2) {
        appfw_mcp_resp_addf(resp, "press 取值: tap / long / double");
        return 1;
    }
    if (!s_sim) {
        appfw_mcp_resp_addf(resp, "模拟触发未接入");
        return 1;
    }
    static const char *const kind_name[] = { "短按", "长按", "双击" };
    s_sim(btn->valueint, kind);
    appfw_mcp_resp_addf(resp, "已模拟 %s %s(按当前模式执行对应槽的动作)",
                        btn_name(btn->valueint), kind_name[kind]);
    return 0;
}

static int tool_get_state(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    const kbmic_config_t *cfg = kbmic_config_current();
    appfw_net_status_t st;
    appfw_net_get_status(&st);
    const int soc = bsp_battery_soc();
    char batt[8];
    snprintf(batt, sizeof(batt), soc >= 0 ? "%d%%" : "--", soc);
    appfw_mcp_resp_addf(resp, "模式 %s(%u/%u) | BLE %s | 电量 %s | WiFi %s %s%s%s",
                        cfg->profiles[cfg->active].name, cfg->active + 1, cfg->count,
                        kbmic_hid_connected() ? "已连接" : "未连接",
                        batt,
                        st.portal_active ? "配网热点" : (st.ip[0] ? "在线" : "离线"),
                        st.ip[0] ? st.ip : "",
                        st.ip[0] ? " " : "",
                        st.portal_active ? st.ap_ssid : "");
    return 0;
}

// ---------------------------------------------------------------------------
// WiFi 工具(与框架 builtin 的 wifi_status/connect_saved 互补:增删已存热点)
// ---------------------------------------------------------------------------
static int tool_wifi_add(cJSON *args, appfw_mcp_resp_t *resp)
{
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(args, "ssid");
    const cJSON *pwd = cJSON_GetObjectItemCaseSensitive(args, "password");
    const cJSON *now = cJSON_GetObjectItemCaseSensitive(args, "connect_now");
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0]) {
        appfw_mcp_resp_addf(resp, "参数 ssid(string)缺失");
        return 1;
    }
    const char *pwd_s = cJSON_IsString(pwd) ? pwd->valuestring : "";

    appfw_netlist_t list;
    appfw_netlist_reset(&list);
    (void)appfw_store_netlist_load(&list);
    if (!appfw_netlist_add(&list, ssid->valuestring, pwd_s)) {
        appfw_mcp_resp_addf(resp, "添加失败:列表已满或参数过长");
        return 1;
    }
    (void)appfw_netlist_select(&list, ssid->valuestring);   // 新加的设为首选
    (void)appfw_store_netlist_save(&list);
    appfw_net_reload_config();
    if (cJSON_IsTrue(now)) {
        appfw_net_connect_ssid(ssid->valuestring);
        appfw_mcp_resp_addf(resp, "已添加热点 %s 并正在连接,稍后用 wifi_status 查询结果",
                            ssid->valuestring);
    } else {
        appfw_mcp_resp_addf(resp, "已添加热点 %s(未连接)", ssid->valuestring);
    }
    return 0;
}

static int tool_wifi_list(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    appfw_netlist_t list;
    if (!appfw_store_netlist_load(&list) || list.count == 0) {
        appfw_mcp_resp_addf(resp, "没有已保存的热点");
        return 0;
    }
    appfw_mcp_resp_addf(resp, "已保存 %d 个热点:", list.count);
    for (int i = 0; i < list.count; i++) {
        const bool open = list.items[i].pwd[0] == '\0';
        appfw_mcp_resp_addf(resp, "\n%d. %s%s", i + 1, list.items[i].ssid,
                            open ? "(开放网络)" : "");
    }
    return 0;
}

static int tool_wifi_remove(cJSON *args, appfw_mcp_resp_t *resp)
{
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(args, "ssid");
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0]) {
        appfw_mcp_resp_addf(resp, "参数 ssid(string)缺失");
        return 1;
    }
    appfw_netlist_t list;
    appfw_netlist_reset(&list);
    bool found = false;
    if (appfw_store_netlist_load(&list)) {
        for (int i = 0; i < list.count; i++) {
            if (strcmp(list.items[i].ssid, ssid->valuestring) == 0) {
                found = appfw_netlist_remove(&list, (uint8_t)i);
                break;
            }
        }
    }
    if (!found) {
        appfw_mcp_resp_addf(resp, "删除失败:列表为空或没有这个热点");
        return 1;
    }
    (void)appfw_store_netlist_save(&list);
    appfw_net_reload_config();
    appfw_mcp_resp_addf(resp, "已删除热点 %s", ssid->valuestring);
    return 0;
}

// 管理页(网页设置)按需开关:空闲堆紧张(httpd 要 ~10KB),不常驻。
// start 只起 HTTP(设备保持在线,AP 不开);完整热点配网走设置菜单。
static int tool_web_start(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    appfw_net_status_t st;
    appfw_net_get_status(&st);
    if (st.ip[0] == '\0') {
        appfw_mcp_resp_addf(resp, "设备离线,网页不可用;请用设置菜单的『开启配网』配热点");
        return 1;
    }
    // 内存门槛:httpd 任务+控制块要 ~10KB。BLE+WiFi 共存下空闲堆紧张,
    // 硬起会把设备送进 OOM 重启(2026-10-06 真机踩坑),先验后动。
    const size_t heap_now = esp_get_free_heap_size();
    const size_t block_now = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (heap_now < 20 * 1024 || block_now < 8 * 1024) {
        appfw_mcp_resp_addf(resp, "内存不足(空闲 %u 字节/最大块 %u 字节),不敢启动管理页;"
                                  "可重启设备后再试,或改用本工具配置",
                            (unsigned)heap_now, (unsigned)block_now);
        return 1;
    }
    if (!appfw_portal_start()) {
        appfw_mcp_resp_addf(resp, "管理页启动失败,稍后再试");
        return 1;
    }
    appfw_mcp_resp_addf(resp, "管理页已启动: http://%s/ (键盘设置卡片在页面下方;"
                              "用完可 kbmic_web_stop 释放内存)", st.ip);
    return 0;
}

static int tool_web_stop(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    appfw_portal_stop();
    appfw_mcp_resp_addf(resp, "管理页已停止,内存已释放");
    return 0;
}

// 蓝牙复位(排障):清设备端绑定并重开广播。主机配对缓存/绑定打架时用;
// 调用后主机侧必须"忽略设备"再重新配对(会重新读取新的报告描述符)。
static int tool_ble_reset(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    const esp_err_t ret = kbmic_hid_reset_bonds();
    if (ret != ESP_OK) {
        appfw_mcp_resp_addf(resp, "复位失败: %s", esp_err_to_name(ret));
        return 1;
    }
    appfw_mcp_resp_addf(resp, "设备端蓝牙绑定已清并重新广播;"
                              "请在主机蓝牙设置里忽略本设备后重新配对");
    return 0;
}

// 射频排障:停/启 WiFi(C3 单射频,WiFi 常驻时 BLE 通知可能在空口被共存
// 调度丢弃 —— 表现为设备"发送无错"而主机收不到任何按键)。
static int tool_radio(cJSON *args, appfw_mcp_resp_t *resp)
{
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(args, "wifi");
    if (!cJSON_IsString(mode) || !mode->valuestring[0]) {
        appfw_mcp_resp_addf(resp, "参数 wifi(string): \"off\" 停 WiFi 只跑蓝牙,"
                                  "\"on\" 恢复 WiFi 连接");
        return 1;
    }
    if (!strcasecmp(mode->valuestring, "off")) {
        esp_wifi_stop();
        appfw_mcp_resp_addf(resp, "WiFi 已停(射频全归蓝牙)。现在测试按键;"
                                  "设备重启或 wifi=on 恢复");
    } else if (!strcasecmp(mode->valuestring, "on")) {
        appfw_net_reload_config();
        appfw_net_connect_saved();
        appfw_mcp_resp_addf(resp, "WiFi 已恢复连接");
    } else {
        appfw_mcp_resp_addf(resp, "wifi 只接受 off / on");
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 工具表与启动
// ---------------------------------------------------------------------------
static const appfw_mcp_tool_t k_tools[] = {
    { "kbmic_get_config", "查看键盘配置(全部模式的按键分配)",
      "{}", tool_get_config },
    { "kbmic_set_active_mode", "切换当前生效的键盘模式",
      "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\"}},\"required\":[\"index\"]}",
      tool_set_active_mode },
    { "kbmic_add_mode", "新增键盘模式并切换过去;内置动作名可含中文(≤15字节 UTF-8)",
      "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}}}",
      tool_add_mode },
    { "kbmic_delete_mode", "删除一个自定义模式(内置模式不可删)",
      "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\"}},\"required\":[\"index\"]}",
      tool_delete_mode },
    { "kbmic_rename_mode", "重命名一个模式",
      "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\"},\"name\":{\"type\":\"string\"}},\"required\":[\"index\",\"name\"]}",
      tool_rename_mode },
    { "kbmic_reset_mode", "把内置模式恢复出厂默认",
      "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\"}},\"required\":[\"index\"]}",
      tool_reset_mode },
    { "kbmic_set_key", "设置某模式某键某槽的动作。preset=目录 id/名字(先 kbmic_action_catalog 查看)"
                       "或 steps=[{kind:\"key\"|\"consumer\"|\"delay\"|\"applefn\",mods:\"Ctrl+Shift\",keycode,usage,delay_ms}]"
                       "(≤4步);button:0=Up 1=Down 2=OK;slot:0=短按 1=双击 2=长按;trigger 可覆盖触发方式",
      "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\"},\"button\":{\"type\":\"integer\"},"
      "\"slot\":{\"type\":\"integer\"},\"preset\":{},\"trigger\":{},\"steps\":{\"type\":\"array\"}},"
      "\"required\":[\"index\",\"button\",\"slot\"]}",
      tool_set_key },
    { "kbmic_action_catalog", "列出内置动作目录(id 与名字,供 kbmic_set_key 的 preset 用)",
      "{}", tool_catalog },
    { "kbmic_simulate_key", "模拟触发一个物理键(按当前模式执行对应槽的动作,等效真人按键)",
      "{\"type\":\"object\",\"properties\":{\"button\":{\"type\":\"integer\"},\"press\":{\"type\":\"string\","
      "\"enum\":[\"tap\",\"long\"]}},\"required\":[\"button\"]}",
      tool_simulate },
    { "kbmic_get_state", "设备状态:当前模式/BLE 连接/电量/WiFi",
      "{}", tool_get_state },
    { "kbmic_wifi_add", "添加已保存热点(名称+密码);connect_now=true 立即连接",
      "{\"type\":\"object\",\"properties\":{\"ssid\":{\"type\":\"string\"},\"password\":{\"type\":\"string\"},"
      "\"connect_now\":{\"type\":\"boolean\"}},\"required\":[\"ssid\"]}",
      tool_wifi_add },
    { "kbmic_wifi_list_saved", "列出已保存的热点",
      "{}", tool_wifi_list },
    { "kbmic_wifi_remove_hotspot", "从已保存列表删除一个热点",
      "{\"type\":\"object\",\"properties\":{\"ssid\":{\"type\":\"string\"}},\"required\":[\"ssid\"]}",
      tool_wifi_remove },
    { "kbmic_radio", "射频排障:wifi=\"off\" 停 WiFi 只跑蓝牙(测键盘是否被共存干扰),\"on\" 恢复",
      "{\"type\":\"object\",\"properties\":{\"wifi\":{\"type\":\"string\",\"enum\":[\"off\",\"on\"]}},\"required\":[\"wifi\"]}",
      tool_radio },
    { "kbmic_ble_reset", "蓝牙排障:清设备端全部绑定并重开广播(主机侧需忽略后重配对)",
      "{}", tool_ble_reset },
    { "kbmic_web_start", "启动网页管理页(浏览器打开返回的 URL,页面含键盘设置卡片)。"
                         "用户想用网页改配置时调用;内存紧张,用完建议 kbmic_web_stop",
      "{}", tool_web_start },
    { "kbmic_web_stop", "停止网页管理页,释放内存",
      "{}", tool_web_stop },
};

void kbmic_mcp_init(void)
{
    // 内置工具跟随使能位:WiFi 管理 + 设备信息 + 配网状态(取 IP 用)。
    // 刷新周期/熄屏/亮度是框架 UI 的概念,本应用没有。
    appfw_mcp_set_builtin_tools(APPFW_MENU_ITEM_WIFI_MANAGER |
                                APPFW_MENU_ITEM_DEVICE_INFO |
                                APPFW_MENU_ITEM_PROVISIONING);
    appfw_mcp_set_tools(k_tools, (int)(sizeof(k_tools) / sizeof(k_tools[0])));
    appfw_mcp_server_start();
    ESP_LOGI(TAG, "设备 MCP:%d 个应用工具 + 内置,端口 %d",
             (int)(sizeof(k_tools) / sizeof(k_tools[0])), appfw_mcp_server_port());
}
