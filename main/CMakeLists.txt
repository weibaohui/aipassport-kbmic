# 快捷键应用：BLE HID、可由桌面 MCP 走 BLE GATT 配置、低内存条件下可选 Wi-Fi/HTTP。
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

# 中文字库由框架提供；不要在应用重复编入同名字库。
