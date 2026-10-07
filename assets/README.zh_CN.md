<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# 资源目录（Assets）

## 字库（fonts）

| 文件 | 规格 | 用途与来源 |
| --- | --- | --- |
| [`fonts/kbmic_charset.txt`](fonts/kbmic_charset.txt) | 纯文本，3616 码点 | 早期应用自带字体的历史生成输入 = 95 个可打印 ASCII + 21 个全角标点与界面符号 + 《通用规范汉字表》一级字表 3500 字。**由 `tools/gen_font_charset.py` 生成**，不手抄。 |
| [`fonts/common_3500.txt`](fonts/common_3500.txt) | 纯文本，3500 字 | 通用规范汉字表一级字表。来源：[shengdoushi/common-standard-chinese-characters-table](https://github.com/shengdoushi/common-standard-chinese-characters-table) 的 `level-1.txt`。 |
| [`fonts/NotoSansSC-Regular.otf`](fonts/NotoSansSC-Regular.otf) | 7.9 MB | 生成字体的源字库，OFL 1.1 许可，许可全文见 [`OFL.txt`](fonts/OFL.txt)。 |
| [`fonts/app_font_16.c`](fonts/app_font_16.c) | 16 px / 4 bpp / LVGL C 源码 | 早期应用自带字体的保留生成物。**不再编译**；当前 16px 界面字体由 `components/framework/appfw/fonts/app_font_16.c` 提供。 |

### 当前生效字库

当前界面字体由框架提供。16px 字符清单由 GB2312 全部图形字符、可打印 ASCII/空格以及框架既有额外字符生成。运行时自建的中文 profile 名和常见服务端返回的中文都不需要重新生成字体。

界面字体只有一份。标题、主页、设置菜单、模式列表、按键配置、动作选择全部共用它。

### 历史资产重新生成

```bash
# 1) 重建符号表（改了全角标点或换字表时必做）
python3 tools/gen_font_charset.py

# 2) 重新生成字体（工具版本固定 1.5.3）
npx lv_font_conv@1.5.3 \
  --font assets/fonts/NotoSansSC-Regular.otf \
  --size 16 --bpp 4 --format lvgl --no-compress \
  --lv-font-name app_font_16 --lv-include lvgl.h \
  --symbols "$(cat assets/fonts/kbmic_charset.txt)" \
  --output assets/fonts/app_font_16.c
```

源字库和旧字表仍随本仓提交，便于离线复现历史生成物。当前生效的框架字体请改用 `components/framework/appfw/fonts/gen_fonts.py` 重新生成。

### 字形覆盖验收

`tests/test_font_gb2312.py` 检查当前 GB2312 字库契约。`tests/test_ui_charset.py` 在 host 门禁里检查上屏字符串中的非 ASCII 字符都落在当前框架字符清单中。

注意它查的不只是汉字——全角冒号、界面自用的 `▸` 光标符号同样会缺字形，漏掉的后果和漏汉字一样。改了界面文案却忘了重新生成字体，门禁会直接失败。

### 为什么不用 LVGL 自带的 CJK 字体

LVGL 内置 `LV_FONT_SOURCE_HAN_SANS_SC_14/16_CJK`，但那两套各约 1.0/1.2 MB Flash，且字形是**随机选取的 1373 字**——实测缺 `键 盘 对 连 按 说 话 换 选 车 确 语 长 设 并` 等本应用文案必用的字。开它等于既占 1 MB 又显示不全，而且照样覆盖不了用户自起的模式名。
