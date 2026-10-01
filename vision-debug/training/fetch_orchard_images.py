#!/usr/bin/env python3
"""
从 Wikimedia Commons 抓取果园照片（含有人和无人的场景），用于测试行人检测模型。
需要经代理访问 Wikimedia（WSL 内 172.19.96.1:7897）。
"""

import json
import os
import urllib.parse
import urllib.request

PROXY = "http://172.19.96.1:7897"
OUT = os.path.expanduser("~/orchard_test")
UA = "Mozilla/5.0 (compatible; CodexResearch/1.0)"

os.environ["http_proxy"] = PROXY
os.environ["https_proxy"] = PROXY
os.makedirs(OUT, exist_ok=True)

# 前几个偏"有人在劳作"，后几个偏"只有果树/草地"（用来查误检）
QUERIES = [
    "apple orchard harvest worker",
    "orchard picking apples",
    "apple harvest people orchard",
    "apple orchard trees rows",
    "orchard grass ground",
    "pear orchard trees",
]


def fetch(url, timeout=60):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    return urllib.request.urlopen(req, timeout=timeout)


def main():
    n = 0
    for q in QUERIES:
        api = (
            "https://commons.wikimedia.org/w/api.php?action=query"
            "&generator=search&gsrsearch=" + urllib.parse.quote(q) +
            "&gsrnamespace=6&gsrlimit=4&prop=imageinfo"
            "&iiprop=url&iiurlwidth=1024&format=json"
        )
        try:
            data = json.load(fetch(api, 30))
        except Exception as e:
            print(f"[query fail] {q}: {e}")
            continue

        pages = (data.get("query") or {}).get("pages") or {}
        for _, p in pages.items():
            ii = p.get("imageinfo")
            if not ii:
                continue
            thumb = ii[0].get("thumburl")
            if not thumb:
                continue
            n += 1
            name = f"orchard_{n:02d}.jpg"
            path = os.path.join(OUT, name)
            try:
                with fetch(thumb, 60) as r, open(path, "wb") as f:
                    f.write(r.read())
                print(f"{name} <- {p.get('title')}")
            except Exception as e:
                print(f"[dl fail] {name}: {e}")

    print("total images:", n)


if __name__ == "__main__":
    main()
