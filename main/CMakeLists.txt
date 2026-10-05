# AI 小键盘 应用组件。
#
# 本应用只用框架的 bsp(显示/LVGL、按键、电池),不引入 appfw 的联网/门户/
# 客户端链路:语音识别发生在电脑/手机侧,设备只负责"按键 -> HID 报告",
# 因此不需要 Wi-Fi、mbedTLS 与 HTTP,固件体积与 RAM 都省一大截。
idf_component_register(
    SRCS "main.c"
         "kbmic_store.c"
         "kbmic_model.c"
         "kbmic_action.c"
         "kbmic_hid.c"
         "kbmic_ble_svc.c"
         "kbmic_ui.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_hid bt nvs_flash esp_timer
    PRIV_REQUIRES freertos
)

# 界面字体(生成物;来源、命令与符号表见 assets/README.zh_CN.md)。
# 符号表由 tools/gen_font_charset.py 依据 assets/fonts/common_3500.txt 生成,
# 覆盖《通用规范汉字表》一级字表,所以用户通过 MCP 自起的模式名不会出方块。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_16.c"
)
