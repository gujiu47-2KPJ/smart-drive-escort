#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
提取 PyInstaller 的 PYZ 归档内容（绕过 pyinstxtractor 的 Python 版本检查）。

背景：pyinstxtractor 在 Python 版本与打包时不一致时会跳过 PYZ 提取，
      而应用代码恰恰都在 PYZ 里。TOC 是 marshal 的简单类型（dict/list/tuple/
      str/int），跨版本可以正常解析，所以自己拆不难。

PYZ 结构：
    b'PYZ\\0'            4 字节魔数
    <pyc magic>          4 字节
    <toc offset>         4 字节，大端
    ... 各模块的 zlib 压缩数据 ...
    <TOC>                在该偏移处，marshal 的 {name: (ispkg, pos, len)}

每个条目解压后就是 marshal 的 code object；写盘时补上 16 字节 PEP 552 头。

用法：python pyz_extract.py <PYZ-00.pyz> <输出目录>
"""

import marshal
import os
import struct
import sys
import zlib


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    pyz_path, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)

    with open(pyz_path, "rb") as fh:
        magic = fh.read(4)
        if magic != b"PYZ\0":
            print(f"ERROR: 不是 PYZ 归档，头 4 字节是 {magic!r}")
            return 1

        pyc_magic = fh.read(4)
        (toc_pos,) = struct.unpack("!i", fh.read(4))
        fh.seek(toc_pos, os.SEEK_SET)
        toc = marshal.load(fh)

    if isinstance(toc, list):
        toc = dict(toc)

    print(f"pyc magic = {pyc_magic.hex()}  PYZ 内条目数 = {len(toc)}")

    count = 0
    for key, value in toc.items():
        ispkg, pos, length = value
        name = key.decode("utf-8") if isinstance(key, (bytes, bytearray)) else str(key)

        with open(pyz_path, "rb") as fh:
            fh.seek(pos, os.SEEK_SET)
            blob = fh.read(length)
        try:
            data = zlib.decompress(blob)
        except Exception as exc:                       # noqa: BLE001
            print(f"  跳过 {name}: 解压失败 {exc}")
            continue

        rel = name.replace("..", "__").replace(".", os.sep)
        if ispkg == 1:
            dest = os.path.join(out_dir, rel, "__init__.pyc")
        else:
            dest = os.path.join(out_dir, rel + ".pyc")
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as out:
            out.write(pyc_magic + b"\0" * 12)          # PEP 552 头
            out.write(data)
        count += 1
        print(f"  {len(data):>7} B  {name}")

    print(f"\n共写出 {count} 个 .pyc 到 {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
