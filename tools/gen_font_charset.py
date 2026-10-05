#!/usr/bin/env python3
"""生成 assets/fonts/kbmic_charset.txt —— 喂给 lv_font_conv 的完整符号表。

为什么是"完整"而不是"界面用到的字":用户可以通过 MCP 自建键盘模式并起中文名
("游戏"、"会议"、"远程"……),名字是运行时才出现的,没法靠扫描源码收集。与其猜用户
会起什么名,不如直接把《通用规范汉字表》一级字表(3500 常用字)整套编进字体 ——
编译后约 250 KB Flash,这个仓有 6 MB 富余,换来的好处是名字随便起都不会出方块。

真正的硬契约在 tests/test_ui_charset.py:界面静态文案必须落在本文件里。改了界面
文案却没重新生成字体,门禁会直接失败。

用法:仓库根目录执行  python3 tools/gen_font_charset.py
"""

from __future__ import annotations

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
CHARSET = ROOT / "assets" / "fonts" / "kbmic_charset.txt"
COMMON = ROOT / "assets" / "fonts" / "common_3500.txt"


def cjk_only(text: str) -> str:
    return "".join(c for c in text if "一" <= c <= "鿿")


def main() -> int:
    if not COMMON.is_file():
        print(f"FAIL: 缺少字表 {COMMON.relative_to(ROOT)}", file=sys.stderr)
        return 1

    ascii_printable = "".join(chr(c) for c in range(0x20, 0x7F))
    # 全角标点,以及界面自己用到的符号(▸ 是列表光标)。
    # 少一个符号就是屏幕上多一个方块,LVGL 没有字体回退,缺了只能画占位块。
    punct = "·—…、。《》（）【】：“”‘’％＋－／▸"

    common = cjk_only(COMMON.read_text(encoding="utf-8"))
    if len(common) != 3500:
        print(
            f"FAIL: {COMMON.relative_to(ROOT)} 应当是 3500 个不重复汉字,实际 {len(common)}",
            file=sys.stderr,
        )
        return 1

    symbols = ascii_printable + punct + common
    CHARSET.write_text(symbols, encoding="utf-8")
    print(
        f"符号表已生成:{len(ascii_printable)} ASCII + {len(punct)} 全角标点 "
        f"+ {len(common)} 汉字 = {len(symbols)} 码位"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
