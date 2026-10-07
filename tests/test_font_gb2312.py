#!/usr/bin/env python3
"""校验框架 16px 字库清单确实覆盖 GB2312,并保留既有框架专用字符。"""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FONT_DIR = ROOT / "components" / "framework" / "appfw" / "fonts"
GB_CHARSET = FONT_DIR / "appfw_gb2312_charset.txt"
COMMON_CHARSET = FONT_DIR / "appfw_common_charset.txt"
FONT = FONT_DIR / "app_font_16.c"


def gb2312_chars() -> set[str]:
    decoded: set[str] = set()
    for high in range(0xA1, 0xF8):
        for low in range(0xA1, 0xFF):
            try:
                decoded.add(bytes((high, low)).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return decoded


def main() -> int:
    if not GB_CHARSET.is_file():
        print(f"缺少字库清单:{GB_CHARSET}")
        return 1
    if not COMMON_CHARSET.is_file():
        print(f"缺少字库清单:{COMMON_CHARSET}")
        return 1
    if not FONT.is_file():
        print(f"缺少生成字体:{FONT}")
        return 1

    charset_text = GB_CHARSET.read_text(encoding="utf-8")
    charset = set(charset_text)
    common = set(
        COMMON_CHARSET.read_text(encoding="utf-8").replace("\n", "").replace("\r", "")
    )
    gb = gb2312_chars()
    expected = set(chr(cp) for cp in range(0x20, 0x7F)) | gb | common
    errors: list[str] = []

    if len(gb) != 7445:
        errors.append(f"GB2312 图形字符数异常:{len(gb)} != 7445")
    if "\n" in charset_text or "\r" in charset_text:
        errors.append("字库清单必须是单行,不能包含换行符")
    if " " not in charset:
        errors.append("字库清单缺少空格(U+0020)")
    if len(charset) != len(charset_text):
        errors.append(
            f"字库清单含重复字符:文本 {len(charset_text)} 个字符,唯一 {len(charset)} 个"
        )
    if charset != expected:
        errors.append(
            f"字库清单与 GB2312∪ASCII∪现有清单不一致:{len(charset)} != {len(expected)}"
        )

    if errors:
        print("GB2312 字库清单检查失败:")
        for error in errors:
            print(f"  {error}")
        return 1

    print(f"GB2312 字库清单检查通过:{len(charset)} 个唯一字符")
    return 0


if __name__ == "__main__":
    sys.exit(main())
