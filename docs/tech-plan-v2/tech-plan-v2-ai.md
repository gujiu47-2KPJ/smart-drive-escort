# 技术方案 v2 —— 面向 AI 的执行规范（AI-READY）

> version: v2.0
> date: 2026-10-09
> status: 已定稿
> 场景: 果园作业（主）+ 转场上路（扩展）
> 总原则: 模块化优先，尽可能用现成模块，减少硬件调试

## 0. 项目一句话

面向果园小型农机的作业与转场全过程主动安全头盔：毫米波雷达 + 视觉 + IMU 多模态融合，对后方与侧向盲区检测、识别、分级预警。

## 1. 硬事实（不变量，禁止修改）

### 1.1 主控

- 板 A：果云 ESP32-S3-**N16R8**（16MB Flash / 8MB 八线 PSRAM），2×21 排母。
- 板 B：Freenove ESP32-S3 CAM，2×20 排母，板载 OV2640。
- **禁用 GPIO**：35/36/37（PSRAM）、43/44（UART0）、45/46（Strapping）、0（BOOT）。
- GPIO48 = 板载 WS2812B（板 A，避免使用）；GPIO19/20 = USB-OTG（不接 OTG 时可作 GPIO）。

### 1.2 毫米波雷达（60GHz，芯片 AT6010）

- 人存版：`MS60-1211S80M`；BSD 版：`MS60-1211S80M-BSD`。
- UART **115200** 8N1；从机模式：`0x58` 命令 / `0x59` 回复 / `0x5A` 主动上报。
- 人存版 `velo_val` 为预留（恒 0）；**BSD 版 TYPE=7 帧输出 s8 速度（1 m/s 分辨率，6~90 km/h）**。
- 供电 3.0~5.5V，电流 80mA/颗。
- 引脚：接口1 = OUT/GND/VCC；接口2 = TX/GND/RX。OUT 不接。
- 距离置信度 `rb_conf<12`、角度置信度 `angle_conf<8` 时应丢弃该目标。

### 1.3 IMU

- HW-991 = BMI270，I2C，地址 0x68（ADO 接 GND）。
- **上电必须先写入 8192 字节官方配置块**，否则数据恒 0。
- SDA/SCL 各 4.7k 上拉。

### 1.4 摄像头

- OV2640，DVP，引脚与 ESP32-S3-EYE 一致：
  `XCLK=15 PCLK=13 VSYNC=6 HREF=7 D0..D7=11,9,8,10,12,18,17,16 SIOD=4 SIOC=5`
- 输出用 **JPEG**，SCCB **100kHz**（400kHz 写 JPEG 寄存器表会 I2C 超时）。

### 1.5 执行器

- 振动马达：1034 扁平 ERM，3V 额定，90mA，PWM ≤20kHz；VM=5V 时占空比 ≤60%。
- TB6612FNG 模块 ×2：
  - H1（控制）: 1=PWMA 2=AIN2 3=AIN1 4=STBY 5=BIN1 6=BIN2 7=PWMB 8=GND
  - H2（电源/电机）: 1=VM 2=VCC 3=GND 4=AO1 5=AO2 6=BO2 7=BO1 8=GND
- 有源蜂鸣器模块：5V，IO 高电平响。

## 2. 传感器布局

| 位置 | 传感器 | 版本 | 角色 |
|---|---|---|---|
| 正后方 | 60G | BSD | 测距测速（转场主力）|
| 左侧 | 60G | 人存 | 存在检测 |
| 右侧 | 60G | 人存 | 存在检测 |
| 正后方 | OV2640 | — | 类别确认（广角优先）|
| 头顶 | BMI270 | — | ego-motion + 头姿 |

取消 24G LD2450。左右 60G 外置扎带，用 0.5mm 漆包线 20~30cm 焊回主板。

## 3. GPIO 分配（锁定值）

### 板 A（果云，J2）

| 功能 | 引脚 |
|---|---|
| IMU SDA / SCL | 1 / 47 |
| 后 60G(BSD) UART1 | TX=14, RX=21 |
| 左 60G(人存) UART2 | TX=2, RX=42 |
| TB6612#1（左/右马达）| PWMA=38, AIN1=48, AIN2=13, STBY=10, BIN1=12, BIN2=11, PWMB=39 |
| TB6612#2（后马达）| PWMA=8；AIN1/STBY 接 3V3，AIN2 接 GND |
| 蜂鸣器 IO | 9 |
| 按键 KEY1/2/3 | 4 / 5 / 6 |
| 跨板同步 SYNC | 40 |

### 板 B（Freenove，J3）

| 功能 | 引脚 |
|---|---|
| 右 60G(人存) UART | TX=1, RX=2 |
| OV2640 | 板载 DVP |
| 跨板同步 SYNC | 21 |

## 4. 网络连接表（可执行）

### 板 A

| 网络 | 连接 |
|---|---|
| IMU_SDA | J2.GPIO1 ↔ IMU.SDA |
| IMU_SCL | J2.GPIO47 ↔ IMU.SCL |
| RADB_TX / RADB_RX | J2.GPIO14/21 ↔ 后60G.Rx/Tx |
| RADL_TX / RADL_RX | J2.GPIO2/42 ↔ 左60G.Rx/Tx |
| PWMA/AIN1/AIN2/STBY/BIN1/BIN2/PWMB | J2 ↔ TB6612#1.H1 |
| MA1/MA2 | TB6612#1.AO1/AO2 ↔ J4（左马达）|
| MB1/MB2 | TB6612#1.BO1/BO2 ↔ J5（右马达）|
| MOTOR3_PWM | J2.GPIO8 ↔ TB6612#2.PWMA |
| MC1/MC2 | TB6612#2.AO1/AO2 ↔ J6（后马达）|
| BUZZ | J2.GPIO9 ↔ 蜂鸣器.IO |
| KEY1/2/3 | J2.GPIO4/5/6 ↔ 按键 |
| SYNC | J2.GPIO40 ↔ J3.GPIO21 |
| 3V3 | IMU.VCC、2×TB6612.VCC、上拉、TB6612#2.AIN1/STBY |
| 5V_MAIN | J2.5V、J3.5V、2×TB6612.VM、蜂鸣器.VCC |
| 5V_RAIL | 磁珠输出 → 3×60G.VCC |
| GND | 全部共地 |

### 板 B

| 网络 | 连接 |
|---|---|
| RADR_TX / RADR_RX | J3.GPIO1/2 ↔ 右60G.Rx/Tx |
| SYNC | J3.GPIO21 ↔ J2.GPIO40 |

## 5. 电源（模块化，两轨）

```text
12V → DC-005 → PPTC(3A) → 船型开关 → [现成 5V/3A 降压模块] → 5V_MAIN
                                                          ├─ 板A / 板B / 2×TB6612(VM) / 蜂鸣器
                                                          └─ 磁珠 → 5V_RAIL → 3×60G
```

- 板上只保留：DC 座、PPTC、开关、磁珠、5V_RAIL 100µF+100nF。
- 总 5V 峰值 ≈1.6A；12V 侧平均 ≈0.8A。
- 12V 禁止直接进任何模块。

## 6. 告警逻辑（结构化规则）

输入：后 BSD `{range_m, velo_mps, angle}`；左右人存 `{is_detected}`；视觉 `{class, conf}`；IMU `{pitch_deg}`。

| 级别 | 条件 | 输出 |
|---|---|---|
| L1 | BSD 目标距离 < 阈值 且 velo 靠近 | 后马达振 + 短蜂鸣 |
| L2 | L1 且视觉 class ∈ {person, cyclist, motorcyclist, car, truck} | 方向马达 + 蜂鸣 |
| L3 | pitch > 35° 持续 1.5s | 三马达振 + 长鸣 |
| L4 | L3 且 (L1 或 L2) | 三马达连续振 + 连续蜂鸣 |

规则：

- 雷达/视觉触发用**连续 3 帧确认**去抖。
- 低头判定**独立、硬实时**，不依赖雷达/摄像头。
- 低速目标速度（人存版无速度字段）由视觉多帧跟踪估算；后向速度直接用 BSD 字段。

## 7. 视觉模型需求

- 类别：`person / cyclist / motorcyclist / car / truck`（5 类，不再增加）。
- 输入 224×224（FPS 不足降 192×192）；INT8；ESP-DL。
- 性能目标：≥2 FPS（ESP32-S3）。
- 数据：后方视角，果园/乡村道路，含逆光/扬尘；COCO 预训练 + Bdd100k 迁移。
- 产出：`best.pt` / `best.onnx` / `best.espdl`。
- 分工：**雷达是唯一第一发现者，视觉只做类别确认**。

## 8. 待验证清单

| # | 事项 | 判据 |
|---|---|---|
| 1 | 3×60G 同处互扰 | 台架无鬼影；检查 0x31 算法类型/抗扰配置 |
| 2 | BSD 协议对齐 | 命令表（0x30/0x31）与人存版差异逐条确认 |
| 3 | 后置摄像头排线 | 实测花屏/丢帧临界长度 |
| 4 | 低头阈值 | 实戴 >35°/1.5s 无误报 |
| 5 | 三马达+蜂鸣器分级体验 | 强度/时长实测 |
| 6 | 60G 距离/速度标定 | 已知人存版距离 +10~14% 系统偏差，BSD 待测 |

## 9. 禁令（禁止做的事）

- 禁止在板上画离散电源电路（用现成模块）。
- 禁止使用 GPIO35/36/37/45/46/0。
- 禁止三颗 60G 同朝向安装。
- 禁止马达线与雷达 UART 平行长走。
- 禁止模型类别超过 5 类。
- 禁止在文档/答辩中写「保障道路安全」（只写「辅助盲区预警」）。
- 禁止把 12V 直接接任何模块。

## 10. 相关文档（工作区）

- `Technical Support Document/技术方案v2_双场景_核心思路记录_2026-10-09.md`
- `Technical Support Document/头盔集成板设计要求_v0.3.md`
- `Technical Support Document/画板任务书_EasyEDA_API版.md`
- `Technical Support Document/雷达AT6010_命令表(从上位机airhost.dll提取).md`
- `Technical Support Document/视觉模型训练需求_v2_2026-10-09.md`
