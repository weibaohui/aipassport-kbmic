# AI 小键盘 应用工程根。
#
# 组件分两层:
#   main/                     应用业务(BLE HID、按键映射、平台档案、UI)
#   components/framework/     框架 submodule(aipassport-fw),内含 appfw 与 bsp
#
# ESP-IDF 只扫描 components/ 的直接子目录、不递归,因此把整个框架仓作为
# 一个 submodule 挂在 components/framework,再用 EXTRA_COMPONENT_DIRS 把它
# 加入组件搜索路径。好处是修复只落框架仓一处,各应用 submodule update 即得。
#
# 本应用只用框架的 bsp(显示/LVGL、按键、电池),不引入 appfw 的联网/门户/
# 客户端链路 —— 语音输入由电脑/手机自己的麦克风与输入法完成,设备侧不需要
# Wi-Fi、TLS 与 HTTP,少带一整条 mbedTLS + lwIP 的固件体积与 RAM 开销。
#
# 工程名保持 FoloToy-AI-Passport,不要带应用后缀:框架门禁 tools/validate.sh
# 的固件环节按固定名收 FoloToy-AI-Passport.bin 与 FoloToy-AI-Passport-full.bin,
# 改了名字 merge-bin 会因 flash_args 找不到镜像而失败。应用之间靠 git 仓库名区分。
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components/framework")

project(FoloToy-AI-Passport)
