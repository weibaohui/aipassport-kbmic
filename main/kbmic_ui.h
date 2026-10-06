// main/kbmic_ui.h —— 界面渲染(appfw_ui 框架版)。
//
// 主页与设置菜单都由框架 appfw_ui 渲染:本模块只提供
//   * 主页内容(home_build/home_poll):语音大块 + 状态行
//   * 「键盘设置」导航子页(nav_*):模式列表/按键配置/动作选择三层,
//     入口在框架设置菜单里(长按 OK 进菜单)
// 按键事件经框架 home_key 回调进 main.c 的槽位执行器。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

// ---- 主页(框架 UI_MAIN 的应用区) ----
void kbmic_home_build(lv_obj_t *parent);
void kbmic_home_poll(void);

// ---- 键盘设置子页(框架菜单 menu_navs 注入) ----
void kbmic_nav_enter(void);                 // 进入:重置到模式列表层
void kbmic_nav_build(lv_obj_t *parent);     // 建页(框架容器)
void kbmic_nav_poll(void);                  // 周期刷新
bool kbmic_nav_key(int btn, int ev);        // 按键;返回 false 退回框架菜单

// ---- 说话状态(主页大块显示;main 在按键回调里更新) ----
void kbmic_ui_set_voice(bool active, const char *btn_label);
