// main/kbmic_web.c —— 键盘设置网页(门户卡片 + REST 端点)。
//
// 页面能力:查看全部模式与键位、切换生效模式、把某个槽改成分录里的预设动作。
// 自定义 steps(目录外的自由组合)走 MCP 的 kbmic_set_key,网页不重复做编辑器。
// HTML 经 appfw_prov_cfg_t.app_config_html 注入阶段二管理页(在线时)。
#include "kbmic_web.h"

#include <stdlib.h>
#include <string.h>

#include "appfw_portal.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "kbmic_action.h"
#include "kbmic_mcp.h"
#include "kbmic_store.h"

static const char *TAG = "kbmic_web";

// ---------------------------------------------------------------------------
// JSON 端点
// ---------------------------------------------------------------------------
// GET /api/kbmic[?mode=N] → {active,count,selected,modes:[{index,name,builtin}],
//                           slots:[…被选中模式的6个槽…],catalog:[…]}
// 瘦身设计:BLE+WiFi 共存后空闲堆仅 ~26KB,全量配置的 cJSON 树会在
// PrintUnformatted 时 OOM 返回 NULL,httpd_resp_send(USE_STRLEN) 里
// strlen(NULL) 直接把设备送崩(2026-10-06 真机踩坑,Load access fault,
// MTVAL=0)。所以默认只带激活模式的槽位;切模式由页面带 ?mode=N 拉取。
static esp_err_t handler_get_config(httpd_req_t *req)
{
    appfw_portal_touch();
    char q[16] = { 0 };
    int mode = -1;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[6] = { 0 };
        if (httpd_query_key_value(q, "mode", v, sizeof(v)) == ESP_OK) {
            mode = atoi(v);
        }
    }

    const kbmic_config_t *cfg = kbmic_config_current();
    if (mode < 0 || mode >= cfg->count) mode = cfg->active;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        appfw_prov_send_ok(req, false);
        return ESP_OK;
    }
    cJSON_AddNumberToObject(root, "active", cfg->active);
    cJSON_AddNumberToObject(root, "count", cfg->count);
    cJSON_AddNumberToObject(root, "selected", mode);
    cJSON *modes = cJSON_AddArrayToObject(root, "modes");
    for (uint8_t i = 0; i < cfg->count && i < KBMIC_MAX_PROFILES; i++) {
        cJSON *m = cJSON_CreateObject();
        if (!m) break;
        cJSON_AddNumberToObject(m, "index", i);
        cJSON_AddStringToObject(m, "name", cfg->profiles[i].name);
        cJSON_AddNumberToObject(m, "builtin", cfg->profiles[i].builtin);
        cJSON_AddItemToArray(modes, m);
    }
    // 槽位只放被选中的那个模式(其余模式页面拉取时再带)
    cJSON *slots = cJSON_AddArrayToObject(root, "slots");
    kbmic_mcp_add_slots_json(slots, &cfg->profiles[mode]);
    cJSON *catalog = cJSON_AddArrayToObject(root, "catalog");
    for (uint8_t i = 0; i < kbmic_catalog_count(); i++) {
        cJSON *it = cJSON_CreateObject();
        if (!it) break;
        cJSON_AddNumberToObject(it, "id", i);
        cJSON_AddStringToObject(it, "name", kbmic_catalog_get(i)->name);
        cJSON_AddItemToArray(catalog, it);
    }
    const char *out = cJSON_PrintUnformatted(root);   // OOM 时为 NULL
    if (!out) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"oom\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    cJSON_free((void *)out);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/kbmic/key {index,button,slot,preset,trigger?} — 预设写入一个槽
static esp_err_t handler_set_key(httpd_req_t *req)
{
    appfw_portal_touch();
    cJSON *root = appfw_prov_read_json(req);
    if (!root) return ESP_FAIL;
    const cJSON *idx = cJSON_GetObjectItemCaseSensitive(root, "index");
    const cJSON *btn = cJSON_GetObjectItemCaseSensitive(root, "button");
    const cJSON *slot = cJSON_GetObjectItemCaseSensitive(root, "slot");
    const cJSON *preset = cJSON_GetObjectItemCaseSensitive(root, "preset");
    const cJSON *trig = cJSON_GetObjectItemCaseSensitive(root, "trigger");
    bool ok = false;
    char msg[96] = "参数不完整";
    if (cJSON_IsNumber(idx) && cJSON_IsNumber(btn) && cJSON_IsNumber(slot) &&
        idx->valueint >= 0 && btn->valueint >= 0 && btn->valueint < KBMIC_BTN_COUNT &&
        slot->valueint >= 0 && slot->valueint < KBMIC_SLOT_COUNT) {
        kbmic_config_t work = *kbmic_config_current();
        if (idx->valueint < work.count && cJSON_IsNumber(preset)) {
            const kbmic_catalog_item_t *item = kbmic_catalog_get((uint8_t)preset->valueint);
            if (item) {
                kbmic_action_t action = item->action;
                if (slot->valueint == KBMIC_SLOT_LONG) action.trigger = KBMIC_TRIG_LONG;
                else if (cJSON_IsNumber(trig) &&
                         trig->valueint >= KBMIC_TRIG_NONE && trig->valueint <= KBMIC_TRIG_LONG) {
                    action.trigger = (uint8_t)trig->valueint;
                }
                work.profiles[idx->valueint].slots[btn->valueint][slot->valueint] = action;
                ok = kbmic_config_commit(&work) == ESP_OK;
                snprintf(msg, sizeof(msg), "已写入 %s", item->name);
            } else {
                snprintf(msg, sizeof(msg), "preset 越界");
            }
        }
    }
    cJSON_Delete(root);
    appfw_prov_send_ok(req, ok);
    return ESP_OK;
}

// POST /api/kbmic/activate {index}
static esp_err_t handler_activate(httpd_req_t *req)
{
    appfw_portal_touch();
    cJSON *root = appfw_prov_read_json(req);
    if (!root) return ESP_FAIL;
    const cJSON *idx = cJSON_GetObjectItemCaseSensitive(root, "index");
    bool ok = false;
    if (cJSON_IsNumber(idx)) {
        kbmic_config_t work = *kbmic_config_current();
        if (idx->valueint >= 0 && idx->valueint < work.count) {
            work.active = (uint8_t)idx->valueint;
            ok = kbmic_config_commit(&work) == ESP_OK;
        }
    }
    cJSON_Delete(root);
    appfw_prov_send_ok(req, ok);
    return ESP_OK;
}

// POST /api/kbmic/mode/add {name} — 新建自定义模式(不切换,页面手动启用)
static esp_err_t handler_mode_add(httpd_req_t *req)
{
    appfw_portal_touch();
    cJSON *root = appfw_prov_read_json(req);
    if (!root) return ESP_FAIL;
    const cJSON *nm = cJSON_GetObjectItemCaseSensitive(root, "name");
    bool ok = false;
    if (cJSON_IsString(nm) && nm->valuestring[0]) {
        kbmic_config_t work = *kbmic_config_current();
        const int idx = kbmic_config_add_profile(&work, nm->valuestring);
        ok = idx >= 0 && kbmic_config_commit(&work) == ESP_OK;
    }
    cJSON_Delete(root);
    appfw_prov_send_ok(req, ok);
    return ESP_OK;
}

// POST /api/kbmic/mode/rename {index,name} — 名字只是标签,内置模式也允许改;
// 内置模式锁的是"不可删除"。
static esp_err_t handler_mode_rename(httpd_req_t *req)
{
    appfw_portal_touch();
    cJSON *root = appfw_prov_read_json(req);
    if (!root) return ESP_FAIL;
    const cJSON *idx = cJSON_GetObjectItemCaseSensitive(root, "index");
    const cJSON *nm = cJSON_GetObjectItemCaseSensitive(root, "name");
    bool ok = false;
    if (cJSON_IsNumber(idx) && cJSON_IsString(nm) && nm->valuestring[0]) {
        kbmic_config_t work = *kbmic_config_current();
        if (idx->valueint >= 0 && idx->valueint < work.count) {
            snprintf(work.profiles[idx->valueint].name, KBMIC_NAME_MAX,
                     "%s", nm->valuestring);
            ok = kbmic_config_commit(&work) == ESP_OK;
        }
    }
    cJSON_Delete(root);
    appfw_prov_send_ok(req, ok);
    return ESP_OK;
}

// POST /api/kbmic/mode/delete {index} — 仅自定义模式;删的是激活模式时
// 把 active 夹回合法范围(与 MCP kbmic_delete_mode 同语义)。
static esp_err_t handler_mode_delete(httpd_req_t *req)
{
    appfw_portal_touch();
    cJSON *root = appfw_prov_read_json(req);
    if (!root) return ESP_FAIL;
    const cJSON *idx = cJSON_GetObjectItemCaseSensitive(root, "index");
    bool ok = false;
    if (cJSON_IsNumber(idx)) {
        kbmic_config_t work = *kbmic_config_current();
        if (kbmic_config_delete_profile(&work, (uint8_t)idx->valueint) == 0) {
            if (work.active >= work.count) {
                work.active = work.count ? (uint8_t)(work.count - 1) : 0;
            }
            ok = kbmic_config_commit(&work) == ESP_OK;
        }
    }
    cJSON_Delete(root);
    appfw_prov_send_ok(req, ok);
    return ESP_OK;
}

bool kbmic_web_register(void *httpd)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/api/kbmic",            .method = HTTP_GET,  .handler = handler_get_config },
        { .uri = "/api/kbmic/key",        .method = HTTP_POST, .handler = handler_set_key },
        { .uri = "/api/kbmic/activate",   .method = HTTP_POST, .handler = handler_activate },
        { .uri = "/api/kbmic/mode/add",    .method = HTTP_POST, .handler = handler_mode_add },
        { .uri = "/api/kbmic/mode/rename", .method = HTTP_POST, .handler = handler_mode_rename },
        { .uri = "/api/kbmic/mode/delete", .method = HTTP_POST, .handler = handler_mode_delete },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (httpd_register_uri_handler(httpd, &routes[i]) != ESP_OK) {
            ESP_LOGE(TAG, "注册 %s 失败", routes[i].uri);
            return false;
        }
    }
    ESP_LOGI(TAG, "键盘设置端点已注册");
    return true;
}

// ---------------------------------------------------------------------------
// 注入阶段二管理页的卡片(<!--APP_CONFIG_HTML--> 处)。原生 <details> 折叠,
// 免得把框架的 WiFi 卡片挤下去。
// ---------------------------------------------------------------------------
// 卡片脚本:拉配置渲染;下拉选预设 → POST 写入。键位行只展示当前动作名。
static const char k_card[] =
"<details id='kbmic_card' open>"
"<summary style='cursor:pointer;font-size:16px;margin:6px 0'>⌨️ 键盘设置</summary>"
"<div id='kbmic_modes'></div>"
"<div id='kbmic_slots' style='margin-top:8px'></div>"
"<p id='kbmic_msg' style='color:#0a0'></p>"
"</details>"
"<script>"
"var KM={sel:0,data:null};"
"function kmMsg(t){document.getElementById('kbmic_msg').textContent=t;}"
"function kmLoad(u){fetch(u||'/api/kbmic').then(r=>r.json()).then(d=>{KM.data=d;"
"KM.sel=d.selected;kmRender();});}"
"function kmRender(){var d=KM.data,s='';"
"s+='<label>模式 </label><select onchange=\"kmLoad(\\'/api/kbmic?mode=\\'+this.value)\">';"
"for(var i=0;i<d.count;i++)s+='<option value=\"'+i+'\"'+(i==KM.sel?' selected':'')+'>'+i+':'+d.modes[i].name+(i==d.active?' (使用中)':'')+'</option>';"
"s+='</select> <button onclick=\"kmAct()\">启用此模式</button> '"
"+'<button onclick=\"kmAdd()\">新建</button>'"
"+'<button onclick=\"kmRen()\">改名</button>'"
"+'<button onclick=\"kmDel()\">删除</button>';"
"document.getElementById('kbmic_modes').innerHTML=s;"
"var m={slots:d.slots},h='<table border=0 style=\"margin-top:6px\">';"
"for(var j=0;j<m.slots.length;j++){var sl=m.slots[j];"
"h+='<tr><td style=\"padding:2px 8px 2px 0\">'+sl.button+'/'+(sl.slot_index?'长按':'短按')+'</td>'"
"+'<td style=\"padding:2px 8px\">'+sl.action.display+'</td>'"
"+'<td><select id=\"km_p'+j+'\">';"
"for(var k=0;k<d.catalog.length;k++)h+='<option value=\"'+d.catalog[k].id+'\">'+d.catalog[k].name+'</option>';"
"h+='</select></td>'"
"+'<td><button onclick=\"kmSet('+j+')\">写入</button></td></tr>';}"
"h+='</table>';document.getElementById('kbmic_slots').innerHTML=h;}"
"function kmAdd(){var n=prompt('新模式名字(≤15字节中文/字母)');if(!n)return;"
"fetch('/api/kbmic/mode/add',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({name:n})}).then(r=>r.json())"
".then(o=>{kmMsg(o.ok?'已新建,记得点「启用此模式」':'新建失败(已满8个?)');kmLoad();});}"
"function kmRen(){var n=prompt('改名(≤15字节)',KM.data.modes[KM.sel].name);if(!n)return;"
"fetch('/api/kbmic/mode/rename',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({index:KM.sel,name:n})}).then(r=>r.json())"
".then(o=>{kmMsg(o.ok?'已改名':'改名失败');kmLoad('/api/kbmic?mode='+KM.sel);});}"
"function kmDel(){var m=KM.data.modes[KM.sel];"
"if(m.builtin){alert('内置模式不可删除');return;}"
"if(!confirm('删除模式 '+m.name+' ?'))return;"
"fetch('/api/kbmic/mode/delete',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({index:KM.sel})}).then(r=>r.json())"
".then(o=>{kmMsg(o.ok?'已删除':'删除失败');kmLoad();});}"
"function kmAct(){fetch('/api/kbmic/activate',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({index:KM.sel})}).then(()=>{kmMsg('已启用模式 '+KM.sel);kmLoad();});}"
"function kmSet(j){var sl=KM.data.slots[j];"
"fetch('/api/kbmic/key',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({index:KM.sel,button:sl.button_index,slot:sl.slot_index,"
"preset:+document.getElementById('km_p'+j).value})}).then(r=>r.json())"
".then(o=>{kmMsg(o.ok?'已写入':'写入失败');kmLoad('/api/kbmic?mode='+KM.sel);});}"
"kmLoad();"
"</script>";

const char *kbmic_web_html(void)
{
    return k_card;
}
