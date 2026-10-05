// main/kbmic_web.h —— 门户里的键盘设置卡片(网页端配置)。
#pragma once

#include <stdbool.h>

struct httpd_req;

// 注册 /api/kbmic* 端点。由 appfw_prov_cfg_t.on_httpd_ready 在门户起来时调用。
bool kbmic_web_register(void *httpd);

// 阶段二管理页注入的 HTML 片段(键盘设置卡片)。
const char *kbmic_web_html(void);
