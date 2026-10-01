#!/usr/bin/env python3
"""把抓下来的 changelog HTML 粗略抽成纯文本，便于检索 Codex 相关条目。"""

import html
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/cdx.html"
raw = open(path, encoding="utf-8", errors="ignore").read()

# 去掉 script/style
raw = re.sub(r"(?is)<(script|style)[^>]*>.*?</\1>", " ", raw)
# 块级标签转换行
raw = re.sub(r"(?i)</(p|div|li|h[1-6]|tr|section|article)>", "\n", raw)
raw = re.sub(r"(?i)<br\s*/?>", "\n", raw)
# 其他标签去掉
raw = re.sub(r"<[^>]+>", "", raw)
raw = html.unescape(raw)

lines = [re.sub(r"\s+", " ", ln).strip() for ln in raw.splitlines()]
lines = [ln for ln in lines if ln]

print("total_lines:", len(lines))
for ln in lines:
    if re.search(r"codex|0\.\d+\.\d+|sandbox|windows|mcp|skill", ln, re.I):
        print(ln[:300])
