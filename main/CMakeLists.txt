# 当前固件只验证 Mac BLE 键盘输入：不编入网络、配网、MCP、配置服务或 UI。
# 按键映射在 main.c 固定；保留 BSP ADC 按键与 ESP-IDF BLE HID。
idf_component_register(
    SRCS "main.c"
         "kbmic_hid.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_hid bt nvs_flash esp_timer
    PRIV_REQUIRES freertos
)
