# AI 小键盘 应用组件。
#
# 业务:BLE HID 键盘 + 按键映射 + 平台档案 + UI;联网能力(WiFi 引擎、
# 配网门户、设备 MCP)来自框架 appfw —— 网页设置与 AI 配置都走它。
idf_component_register(
    SRCS "main.c"
         "kbmic_store.c"
         "kbmic_model.c"
         "kbmic_action.c"
         "kbmic_hid.c"
         "kbmic_ble_svc.c"
         "kbmic_ui.c"
         "kbmic_mcp.c"
         "kbmic_web.c"
    INCLUDE_DIRS "."
    REQUIRES appfw bsp esp_hid bt nvs_flash esp_timer
    PRIV_REQUIRES freertos
)

# 中文字库由框架提供(appfw/fonts/,常见 3500 字全量)。
# 注意:不要在这里再挂一份同名的 app_font_16.c —— 框架经 -u 锚点已强制
# 引入默认字库,应用再来一份强符号就是重复定义(链接期报 multiple definition)。
# 本应用旧的 assets/fonts 管线已废弃,仅留作历史参考。
