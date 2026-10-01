# IMU 调试工程（HW-991 / Bosch BMI270）

独立的最小 ESP-IDF 工程，只做一件事：**确认 IMU 能工作，并把全部参数从串口打出来**。
不依赖雷达、融合等模块，符合 PROJECT.md「所有模块必须可独立测试」的原则。

## 目录

```
imu-debug/
├── CMakeLists.txt
├── sdkconfig.defaults
├── main/
│   └── imu_debug_main.c            调试程序（打印全部参数）
└── components/
    └── imu_bmi270/
        ├── include/imu_bmi270.h    驱动对外接口
        ├── imu_bmi270.c            驱动实现
        ├── bmi270_config_file.c    博世官方 8192 字节配置块（自动生成）
        └── CMakeLists.txt
```

## 快速开始

用 VS Code 打开 `G:\codex-workspace\imu-debug`，然后在 ESP-IDF 终端里执行：

```
idf.py set-target esp32s3
idf.py build
idf.py -p COM编号 flash monitor
```

串口固定 **115200 8N1**。

## 接线

| 模块引脚 | 接到 ESP32-S3 | 说明 |
|---|---|---|
| VCC | 3V3 | 建议 3.3V，避免 I2C 电平不匹配 |
| GND | GND | 必须共地 |
| SCL / SCLK | GPIO47 | 见 imu_debug_main.c 顶部宏 |
| SDA / MOSI | GPIO1 | 同上 |
| ADO / MISO | GND 或悬空 | 决定地址：接地 0x68，接高 0x69 |
| CS | 悬空或接高 | 本驱动走 I2C，CS 需保持非选中 |
| INT1 / INT2 | 暂不接 | 后续做中断唤醒时再接 |

GPIO1 / GPIO47 是《技术路线与调试记录》里定下的**规划引脚，不是实测结果**。
接线前请确认，改 `imu_debug_main.c` 顶部的四个宏即可。
建议 SDA/SCL 各加一个 4.7k 上拉到 3.3V。

## 串口输出内容

1. 启动横幅：芯片型号、IDF 版本、I2C 引脚与速率、两轴量程与换算系数
2. **步骤 1**：I2C 总线扫描（0x08-0x77），列出所有响应的地址
3. **步骤 2**：初始化过程，逐项打印芯片 ID 校验、软复位、8192 字节配置块上传、加载状态
4. **步骤 3**：期望寄存器值提示 + 0x00-0x7F 全寄存器快照
5. **步骤 4**：连续采集，每帧一行，同时给出**原始码值**和**物理量**：

```
帧号  A_raw(X,Y,Z)  A(g) X/Y/Z  模长  G_raw(X,Y,Z)  G(dps) X/Y/Z  模长  温度
```

每 2 秒额外打印一次最小 / 最大 / 平均值统计，以及**实测采样率**（用来验证调度是否跟得上）。

## 关键背景：为什么需要那 8192 字节配置块

BMI270 内部有一个特征引擎。上电复位后，必须把博世的一段二进制配置通过
`INIT_CTRL / INIT_ADDR / INIT_DATA` 灌进芯片，加速度计和陀螺仪的数据寄存器
才会更新。**在灌入之前读出来恒为 0**。这是 BMI270 与 MPU6050 那类老芯片
最大的区别，也是最容易踩的坑。

这段数据是博世的算法参数，不是寄存器时序，无法自行推导，只能原样使用。
`bmi270_config_file.c` 由 `tools/gen-bmi270-config.py` 从博世官方
BMI270_SensorAPI 提取，**不要手工编辑**。需要重新生成时：

```
python G:\codex-workspace\tools\gen-bmi270-config.py <bmi270.c 路径> <输出路径>
```

## 排错对照

| 现象 | 可能原因 | 怎么办 |
|---|---|---|
| 总线上一个设备都没有 | 供电、接线、缺上拉 | 查 VCC/GND、SDA/SCL 是否接反、补 4.7k 上拉 |
| 有设备但地址不是 0x68 / 0x69 | ADO 接法不同，或总线上还有别的芯片 | 按程序打印的扫描结果改 `cfg.device_address` |
| 芯片 ID 不是 0x24 | 不是 BMI270，或总线被占用 | 确认模块型号，检查是否有别的驱动抢先初始化 |
| 芯片 ID 正确但数据全是 0 | 配置块没加载成功 | 看 `INTERNAL_STATUS` 打印；把 I2C 速率降到 100000 重试 |
| 数据偶尔跳变或读取失败 | I2C 速率过高、线太长、无上拉 | 降到 100000、缩短杜邦线、补上拉电阻 |
| 静止时模长明显偏离 1.0 g | 量程设错，或模块没放平 | 核对 `IMU_ACC_RANGE`，水平静置再看 |

## 与主工程的关系

这个组件之后要并入主固件：把 `components/imu_bmi270/` 整个目录复制到
`G:\codex-workspace\firmware\components\` 即可，对外接口不用改。
