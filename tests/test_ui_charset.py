#!/usr/bin/env python3
"""中文字形覆盖门禁:上屏文案 ⊆ 框架 16px 字库。

界面使用 components/framework/appfw/fonts/ 的 app_font_16。该字号现在使用
GB2312 全量清单加框架额外字符;改了上屏文案而字库缺字,真机就是方框。
本测试让这种改动在门禁期失败,而不是在用户眼前失败。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHARSET = ROOT / "components" / "framework" / "appfw" / "fonts" / "appfw_gb2312_charset.txt"

# 会把文本送进 LVGL 渲染的源码:界面构造 + 状态机(动态标题/底注/模式名)。
SOURCES = [
    ROOT / "main" / "kbmic_ui.c",
    ROOT / "main" / "main.c",
    ROOT / "main" / "kbmic_model.c",
]


def main() -> int:
    if not CHARSET.is_file():
        print(f"缺少框架字符清单 {CHARSET}(先更新 components/framework 子模块)")
        return 1
    charset = set(CHARSET.read_text(encoding="utf-8").strip())

    used: set[str] = set()
    for path in SOURCES:
        text = path.read_text(encoding="utf-8")
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        text = re.sub(r"//[^\n]*", "", text)
        for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', text):
            used |= {c for c in lit if ord(c) > 0x7F}

    missing = sorted(used - charset)
    if missing:
        print("字形覆盖检查失败,以下字符不在框架字库中(真机会显示为方框):")
        for ch in missing:
            print(f"  U+{ord(ch):04X} {ch!r}")
        print("补入 appfw_gb2312_charset.txt 并重跑 appfw/fonts/gen_fonts.py,"
              "或改用字库内字符。")
        return 1
    print(f"字形覆盖检查通过:{len(used)} 个非 ASCII 字符全部在框架字库中")
    return 0


if __name__ == "__main__":
    sys.exit(main())
