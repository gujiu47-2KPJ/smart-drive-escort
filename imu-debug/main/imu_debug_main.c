/*
 * HW-991 (Bosch BMI270) 六轴 IMU 串口调试程序
 *
 * 目的：在不接雷达、不跑融合的前提下，单独验证 IMU 能否正常工作，
 *       并把全部参数通过串口完整打印出来。
 *
 * 输出：
 *   1. 启动信息：芯片型号、IDF 版本、I2C 引脚与速率、量程配置
 *   2. I2C 总线扫描结果
 *   3. 初始化每一步的结果（芯片 ID / 软复位 / 配置块加载）
 *   4. 0x00~0x7F 全寄存器快照
 *   5. 循环输出每帧的原始码值与物理量，外加滚动统计
 *
 * 串口：UART0，115200 8N1（见 sdkconfig.defaults）
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "imu_bmi270.h"

/* ---------------- 按需修改的接线配置 ---------------- */
/* 沿用《技术路线与调试记录》里定下的相机板预留脚。这是规划值，接线前请确认。 */
#define IMU_SDA_GPIO        GPIO_NUM_1
#define IMU_SCL_GPIO        GPIO_NUM_47
#define IMU_I2C_PORT        I2C_NUM_0
#define IMU_I2C_SPEED_HZ    400000

#define IMU_ACC_RANGE       IMU_ACC_RANGE_4G
#define IMU_GYR_RANGE       IMU_GYR_RANGE_1000DPS

/* 采样周期与打印节流 */
#define SAMPLE_PERIOD_MS    20      /* 20ms 约等于 50 Hz */
#define PRINT_EVERY_N       1       /* 每 N 帧打印一行数据 */
#define STATS_PERIOD_MS     2000    /* 每 2 秒打印一次统计 */

/* ---------------- 滚动统计 ---------------- */
typedef struct {
    float acc_min[3], acc_max[3], acc_sum[3];
    float gyr_min[3], gyr_max[3], gyr_sum[3];
    float temp_min, temp_max, temp_sum;
    uint32_t count;
    int64_t  first_us;
} stats_t;

static void stats_reset(stats_t *s)
{
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < 3; i++) {
        s->acc_min[i] = 1e9f;
        s->acc_max[i] = -1e9f;
        s->gyr_min[i] = 1e9f;
        s->gyr_max[i] = -1e9f;
    }
    s->temp_min = 1e9f;
    s->temp_max = -1e9f;
}

static void stats_update(stats_t *s, const imu_bmi270_sample_t *d)
{
    if (s->count == 0) {
        s->first_us = d->timestamp_us;
    }
    for (int i = 0; i < 3; i++) {
        if (d->accel_g[i] < s->acc_min[i])  s->acc_min[i] = d->accel_g[i];
        if (d->accel_g[i] > s->acc_max[i])  s->acc_max[i] = d->accel_g[i];
        s->acc_sum[i] += d->accel_g[i];
        if (d->gyro_dps[i] < s->gyr_min[i]) s->gyr_min[i] = d->gyro_dps[i];
        if (d->gyro_dps[i] > s->gyr_max[i]) s->gyr_max[i] = d->gyro_dps[i];
        s->gyr_sum[i] += d->gyro_dps[i];
    }
    if (d->temperature_c < s->temp_min) s->temp_min = d->temperature_c;
    if (d->temperature_c > s->temp_max) s->temp_max = d->temperature_c;
    s->temp_sum += d->temperature_c;
    s->count++;
}

static void stats_print(const stats_t *s)
{
    if (s->count == 0) {
        return;
    }
    const float n = (float)s->count;
    const float span_s = (float)(esp_timer_get_time() - s->first_us) / 1e6f;
    const float hz = (span_s > 0.0f) ? (n / span_s) : 0.0f;
    static const char *axis[3] = { "X", "Y", "Z" };

    printf("\n----- 统计 %u 帧 / %.1f 秒 / 实测 %.1f Hz -----\n",
           (unsigned)s->count, span_s, hz);
    printf("  加速度(g)      最小      最大      平均\n");
    for (int i = 0; i < 3; i++) {
        printf("    %s        %8.4f  %8.4f  %8.4f\n",
               axis[i], s->acc_min[i], s->acc_max[i], s->acc_sum[i] / n);
    }
    printf("  角速度(dps)    最小      最大      平均\n");
    for (int i = 0; i < 3; i++) {
        printf("    %s        %8.3f  %8.3f  %8.3f\n",
               axis[i], s->gyr_min[i], s->gyr_max[i], s->gyr_sum[i] / n);
    }
    printf("  温度(C)  平均 %.2f  最小 %.2f  最大 %.2f\n",
           s->temp_sum / n, s->temp_min, s->temp_max);
    printf("------------------------------------------------\n\n");
}

/* ---------------- 启动信息 ---------------- */
static void print_banner(const imu_bmi270_config_t *cfg)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    printf("\n");
    printf("==========================================================\n");
    printf("  HW-991 (Bosch BMI270) 六轴 IMU 串口调试程序\n");
    printf("==========================================================\n");
    printf("  芯片        : %s  核心数 %d  硅片版本 v%d.%d\n",
           CONFIG_IDF_TARGET, info.cores, info.revision / 100, info.revision % 100);
    printf("  ESP-IDF     : %s\n", esp_get_idf_version());
    printf("  I2C 端口    : %d\n", (int)cfg->i2c_port);
    printf("  SDA / SCL   : GPIO%d / GPIO%d  (规划引脚，请确认实际接线)\n",
           (int)cfg->sda_gpio, (int)cfg->scl_gpio);
    printf("  I2C 速率    : %u Hz\n", (unsigned)cfg->scl_speed_hz);
    printf("  加速度量程  : %s  (%.1f LSB/g)\n",
           imu_bmi270_acc_range_name(cfg->acc_range),
           imu_bmi270_acc_lsb_per_g(cfg->acc_range));
    printf("  陀螺量程    : %s  (%.4f LSB/(deg/s))\n",
           imu_bmi270_gyr_range_name(cfg->gyr_range),
           imu_bmi270_gyr_lsb_per_dps(cfg->gyr_range));
    printf("  传感器 ODR  : 100 Hz\n");
    printf("  采样周期    : %d ms\n", SAMPLE_PERIOD_MS);
    printf("  串口        : UART0  115200 8N1\n");
    printf("----------------------------------------------------------\n\n");
}

static void print_expected_registers(void)
{
    printf("初始化后的寄存器期望值（第 3 步的快照可对照）:\n");
    printf("  ACC_CONF(0x40) = 0xA8    ACC_RANGE(0x41) = 0x%02X\n",
           (unsigned)IMU_ACC_RANGE);
    printf("  GYR_CONF(0x42) = 0xA8    GYR_RANGE(0x43) = 0x%02X\n",
           (unsigned)IMU_GYR_RANGE);
    printf("  PWR_CTRL(0x7D) = 0x0E    (加速度 + 陀螺 + 温度 使能)\n\n");
}

void app_main(void)
{
    imu_bmi270_config_t cfg = IMU_BMI270_CONFIG_DEFAULT();
    cfg.i2c_port     = IMU_I2C_PORT;
    cfg.sda_gpio     = IMU_SDA_GPIO;
    cfg.scl_gpio     = IMU_SCL_GPIO;
    cfg.scl_speed_hz = IMU_I2C_SPEED_HZ;
    cfg.acc_range    = IMU_ACC_RANGE;
    cfg.gyr_range    = IMU_GYR_RANGE;

    print_banner(&cfg);

    /* --- 步骤 1：扫描总线 --- */
    printf("[步骤 1/4] 扫描 I2C 总线 ...\n");
    uint8_t found[16] = { 0 };
    size_t  found_count = 0;
    esp_err_t err = imu_bmi270_scan_bus(&cfg, found, sizeof(found), &found_count);
    if (err != ESP_OK) {
        printf("  扫描失败: %s\n", esp_err_to_name(err));
        return;
    }
    if (found_count == 0) {
        printf("  总线上没有发现任何设备。\n");
        printf("  请检查：VCC 是否供电、SDA/SCL 是否接反、是否 3.3V 供电、\n");
        printf("          以及 SDA/SCL 上是否有 4.7k 上拉电阻。\n");
        return;
    }
    printf("  发现 %u 个设备:", (unsigned)found_count);
    for (size_t i = 0; i < found_count && i < sizeof(found); i++) {
        printf(" 0x%02X", found[i]);
    }
    printf("\n\n");

    /* --- 步骤 2：初始化 --- */
    printf("[步骤 2/4] 初始化 BMI270 ...\n");
    err = imu_bmi270_init(&cfg);
    if (err != ESP_OK) {
        printf("  初始化失败: %s\n", esp_err_to_name(err));
        if (err == ESP_ERR_NOT_FOUND) {
            printf("  0x68 与 0x69 都无应答：确认 ADO 引脚接法，或改用扫描到的地址。\n");
        } else if (err == ESP_ERR_INVALID_RESPONSE) {
            printf("  芯片 ID 不是 0x24：可能不是 BMI270，或总线被别的驱动占用。\n");
        } else if (err == ESP_FAIL) {
            printf("  配置块加载失败：供电不稳或 I2C 速率过高，可降到 100000 再试。\n");
        }
        return;
    }
    printf("  初始化成功，从机地址 0x%02X\n\n", imu_bmi270_address());

    /* --- 步骤 3：寄存器快照 --- */
    printf("[步骤 3/4] 寄存器快照 (0x00 - 0x7F) ...\n");
    print_expected_registers();
    if (imu_bmi270_dump_registers() != ESP_OK) {
        printf("  寄存器读取失败\n");
    }
    printf("\n");

    /* --- 步骤 4：连续采集 --- */
    printf("[步骤 4/4] 开始连续采集，每 %d ms 一帧 ...\n", SAMPLE_PERIOD_MS);
    printf("  格式: 帧号 | 加速度原始值(X,Y,Z) | 加速度(g) X/Y/Z | 模长 |");
    printf(" 陀螺原始值(X,Y,Z) | 角速度(dps) X/Y/Z | 模长 | 温度\n");

    stats_t stats;
    stats_reset(&stats);
    int64_t last_stats_us = esp_timer_get_time();
    uint32_t frame = 0;
    uint32_t read_errors = 0;
    imu_bmi270_sample_t d;

    while (1) {
        err = imu_bmi270_read(&d);
        if (err != ESP_OK) {
            read_errors++;
            if (read_errors <= 5 || (read_errors % 50) == 0) {
                printf("  读取失败 #%u: %s\n", (unsigned)read_errors, esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
            continue;
        }

        stats_update(&stats, &d);

        if (PRINT_EVERY_N <= 1 || (frame % PRINT_EVERY_N) == 0) {
            printf("%6u  A_raw(%6d,%6d,%6d)  A(%7.3f,%7.3f,%7.3f)g  |a|=%6.3f  "
                   "G_raw(%6d,%6d,%6d)  G(%8.2f,%8.2f,%8.2f)dps  |w|=%7.2f  T=%6.2fC\n",
                   (unsigned)frame,
                   d.accel_raw[0], d.accel_raw[1], d.accel_raw[2],
                   d.accel_g[0], d.accel_g[1], d.accel_g[2], d.accel_norm_g,
                   d.gyro_raw[0], d.gyro_raw[1], d.gyro_raw[2],
                   d.gyro_dps[0], d.gyro_dps[1], d.gyro_dps[2], d.gyro_norm_dps,
                   d.temperature_c);
        }
        frame++;

        if ((esp_timer_get_time() - last_stats_us) >= (int64_t)STATS_PERIOD_MS * 1000) {
            stats_print(&stats);
            stats_reset(&stats);
            last_stats_us = esp_timer_get_time();
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}