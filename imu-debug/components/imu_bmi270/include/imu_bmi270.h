/*
 * HW-991 (Bosch BMI270) 六轴 IMU 驱动 —— ESP-IDF 组件
 *
 * 硬件事实（来源：HW-991 产品说明书 + 博世 BMI270 官方 SDK）：
 *   - HW-991 模块板载芯片为 Bosch BMI270，6 轴（3 加速度 + 3 陀螺），16 位
 *   - 支持 I2C 或 SPI，本驱动使用 I2C
 *   - I2C 7 位地址由 ADO/MISO 引脚决定：接地 = 0x68，接高 = 0x69
 *   - 模块供电 5V 或 3.3V，强烈建议 3.3V，避免 I2C 电平不匹配
 *
 * 重要：BMI270 上电后必须先灌入一段 8192 字节的官方配置块，
 *       否则加速度计和陀螺仪的数据寄存器不会更新。详见 bmi270_config_file.c。
 *
 * 寄存器与时序依据：Bosch Sensortec BMI270 SensorAPI（BSD-3-Clause）
 *   https://github.com/boschsensortec/BMI270_SensorAPI
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* BMI270 芯片 ID 寄存器（0x00）的期望值 */
#define IMU_BMI270_CHIP_ID           0x24

/* ADO 引脚决定的两个可能地址 */
#define IMU_BMI270_ADDR_PRIMARY      0x68
#define IMU_BMI270_ADDR_SECONDARY    0x69

/* 单次 I2C 传输超时 */
#define IMU_BMI270_I2C_TIMEOUT_MS    100

/* 加速度计量程 */
typedef enum {
    IMU_ACC_RANGE_2G = 0,
    IMU_ACC_RANGE_4G = 1,
    IMU_ACC_RANGE_8G = 2,
    IMU_ACC_RANGE_16G = 3,
} imu_acc_range_t;

/* 陀螺量程 */
typedef enum {
    IMU_GYR_RANGE_2000DPS = 0,
    IMU_GYR_RANGE_1000DPS = 1,
    IMU_GYR_RANGE_500DPS = 2,
    IMU_GYR_RANGE_250DPS = 3,
    IMU_GYR_RANGE_125DPS = 4,
} imu_gyr_range_t;

typedef struct {
    i2c_port_num_t  i2c_port;       /* I2C 控制器编号，如 I2C_NUM_0 */
    gpio_num_t      sda_gpio;       /* I2C SDA 引脚 */
    gpio_num_t      scl_gpio;       /* I2C SCL 引脚 */
    uint32_t        scl_speed_hz;   /* 建议 400000；读取异常时降到 100000 */
    uint8_t         device_address; /* 0 = 自动探测 0x68 与 0x69 */
    imu_acc_range_t acc_range;
    imu_gyr_range_t gyr_range;
} imu_bmi270_config_t;

/*
 * 默认配置。
 * 引脚沿用《技术路线与调试记录》里定下的相机板预留脚：
 *   SDA = GPIO1, SCL = GPIO47
 * 这是规划值，不是实测值——接线前请自行确认，改这里一处即可。
 */
#define IMU_BMI270_CONFIG_DEFAULT()                    \
    (imu_bmi270_config_t) {                            \
        .i2c_port = I2C_NUM_0,                         \
        .sda_gpio = GPIO_NUM_1,                        \
        .scl_gpio = GPIO_NUM_47,                       \
        .scl_speed_hz = 400000,                        \
        .device_address = 0,                           \
        .acc_range = IMU_ACC_RANGE_4G,                 \
        .gyr_range = IMU_GYR_RANGE_1000DPS,            \
    }

/* 一次采样：同时给出原始码值与物理量 */
typedef struct {
    int16_t accel_raw[3];      /* X, Y, Z 原始码值 */
    int16_t gyro_raw[3];       /* X, Y, Z 原始码值 */
    int16_t temp_raw;          /* 温度原始码值 */
    float   accel_g[3];        /* 单位 g */
    float   gyro_dps[3];       /* 单位 deg/s */
    float   accel_norm_g;      /* 加速度模长，静止时应接近 1.0 g */
    float   gyro_norm_dps;     /* 角速度模长 */
    float   temperature_c;     /* 单位 摄氏度 */
    int64_t timestamp_us;      /* 读取时刻，来自 esp_timer_get_time() */
} imu_bmi270_sample_t;

/* 扫描 I2C 总线，把响应的 7 位地址写进 found[]（最多 max 个） */
esp_err_t imu_bmi270_scan_bus(const imu_bmi270_config_t *cfg,
                              uint8_t *found, size_t max, size_t *count);

/* 初始化：建总线 -> 探测地址 -> 软复位 -> 灌配置块 -> 配置量程/ODR -> 使能传感器 */
esp_err_t imu_bmi270_init(const imu_bmi270_config_t *cfg);

/* 读取一次完整数据（加速度 + 陀螺 + 温度，并换算成物理量） */
esp_err_t imu_bmi270_read(imu_bmi270_sample_t *out);

/* 打印 0x00-0x7F 全部寄存器，用于排查配置是否真的写进去了 */
esp_err_t imu_bmi270_dump_registers(void);

/* 初始化成功后可以查到实际使用的地址与配置 */
uint8_t                    imu_bmi270_address(void);
const imu_bmi270_config_t *imu_bmi270_active_config(void);

/* 把枚举翻译成人类可读的名字与换算系数 */
const char *imu_bmi270_acc_range_name(imu_acc_range_t r);
const char *imu_bmi270_gyr_range_name(imu_gyr_range_t r);
float       imu_bmi270_acc_lsb_per_g(imu_acc_range_t r);
float       imu_bmi270_gyr_lsb_per_dps(imu_gyr_range_t r);

#ifdef __cplusplus
}
#endif
