#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从博世官方 BMI270 SensorAPI 源码中提取配置块，生成本工程使用的 C 文件。

来源  : https://github.com/boschsensortec/BMI270_SensorAPI  (BSD-3-Clause)
        文件 bmi270.c 中的 const uint8_t bmi270_config_file[]
用途  : BMI270 上电后必须把这段 8192 字节的配置灌进芯片，
        否则加速度计和陀螺仪的数据寄存器不会更新。这段数据是
        博世内部算法的二进制配置，无法用寄存器时序推导，只能原样使用。

用法:
    python gen-bmi270-config.py <bmi270.c 路径> <输出 .c 路径>
"""

import re
import sys

EXPECTED_LEN = 8192


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    src_path, out_path = sys.argv[1], sys.argv[2]
    with open(src_path, "r", encoding="utf-8", errors="strict") as fh:
        src = fh.read()

    m = re.search(
        r"const\s+uint8_t\s+bmi270_config_file\s*\[\s*\]\s*=\s*\{(.*?)\};",
        src,
        re.S,
    )
    if not m:
        print("ERROR: 在源文件中找不到 bmi270_config_file[] 数组")
        return 1

    body = m.group(1)
    tokens = re.findall(r"0[xX][0-9a-fA-F]+|\b\d+\b", body)
    values = [int(t, 0) for t in tokens]

    if len(values) != EXPECTED_LEN:
        print(f"ERROR: 期望 {EXPECTED_LEN} 字节，实际解析出 {len(values)} 字节")
        return 1
    if any(v < 0 or v > 255 for v in values):
        print("ERROR: 解析出的数值超出字节范围")
        return 1

    lines = []
    for i in range(0, len(values), 12):
        chunk = ", ".join(f"0x{v:02x}" for v in values[i:i + 12])
        lines.append(f"    {chunk},")
    body_text = "\n".join(lines)

    header = f"""/*
 * BMI270 配置块（{EXPECTED_LEN} 字节）
 *
 * 本文件由 tools/gen-bmi270-config.py 自动生成，请勿手工编辑。
 *
 * 来源：Bosch Sensortec BMI270 SensorAPI
 *       https://github.com/boschsensortec/BMI270_SensorAPI
 *       文件 bmi270.c 中的 const uint8_t bmi270_config_file[]
 * 许可：BSD-3-Clause（Copyright (c) Bosch Sensortec GmbH）
 *
 * 为什么必须有它：
 *   BMI270 内部带一个特征引擎，上电复位后必须把这段二进制配置
 *   通过 INIT_CTRL / INIT_ADDR / INIT_DATA 灌进芯片。在灌入之前，
 *   加速度计和陀螺仪的数据寄存器不会更新（读出来恒为 0）。
 *   这段数据是博世的算法参数，不是寄存器时序，无法自行推导。
 */

#include <stdint.h>

const uint32_t bmi270_config_file_len = {EXPECTED_LEN}U;

const uint8_t bmi270_config_file[{EXPECTED_LEN}] = {{
{body_text}
}};
"""

    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(header)

    print(f"OK  {EXPECTED_LEN} 字节 -> {out_path}")
    print(f"    首字节: 0x{values[0]:02x} 0x{values[1]:02x} 0x{values[2]:02x} 0x{values[3]:02x}")
    print(f"    末字节: 0x{values[-1]:02x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
