#!/usr/bin/env python3
"""字形覆盖门禁:main/kbmic_ui.c 字符串字面量里的每个非 ASCII 字符都必须
在 assets/fonts/kbmic_charset.txt 中(该文件就是 lv_font_conv 的 --symbols 输入)。

UTF-8 正确、编译成功,都不代表屏幕能显示 —— 字形不在字体子集里时 LVGL 会画
占位方块。本测试把"界面文案 ⊆ 字体子集"变成硬契约:改了文案没重新生成字体,
门禁立刻失败。

注意这里检查的是**所有非 ASCII 字符**,不是只查 CJK。全角冒号、界面自用的
▸ 光标符号同样会缺字形,漏掉它们的后果和漏汉字一模一样。

符号表本身用 tools/gen_font_charset.py 从 assets/fonts/common_3500.txt 加上
固定的全角标点生成,两边共用同一份来源,不会出现"测试查的和生成的不是同一批"。
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
CHARSET = ROOT / "assets" / "fonts" / "kbmic_charset.txt"
RENDERED = ROOT / "main" / "kbmic_ui.c"

# 界面文案里允许出现的非 ASCII 字符白名单(用于给出更好的错误提示)。
HINT = "非 ASCII 字符"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def main() -> int:
    if not CHARSET.is_file():
        print(f"FAIL: 缺少字符清单 {CHARSET.relative_to(ROOT)}", file=sys.stderr)
        return 1
    if not RENDERED.is_file():
        print(f"FAIL: 缺少 {RENDERED.relative_to(ROOT)}", file=sys.stderr)
        return 1

    covered = set(CHARSET.read_text(encoding="utf-8"))
    text = strip_comments(RENDERED.read_text(encoding="utf-8"))

    missing: list[str] = []
    for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', text):
        for ch in lit:
            if ord(ch) > 0x7F and ch not in covered and ch not in missing:
                missing.append(ch)

    if missing:
        print(
            f"FAIL: 以下{HINT}不在 {CHARSET.relative_to(ROOT)} 中,屏幕会显示方块:",
            file=sys.stderr,
        )
        print("  " + " ".join(f"{c}(U+{ord(c):04X})" for c in missing), file=sys.stderr)
        print("修复: python3 tools/gen_font_charset.py && 重新执行 lv_font_conv", file=sys.stderr)
        return 1

    print(f"OK: 界面文案字形全部落在字体子集内(符号表 {len(covered)} 个码位)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
