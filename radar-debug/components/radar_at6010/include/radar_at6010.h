// 觅感 MS60-1211S80M 毫米波雷达驱动 —— ESP-IDF 组件
//
// 硬件事实（来源：《MS60-1211S80M 产品手册》+《AT6010 SOC HCI Protocol V1.0》）：
//   - 模组核心是隔空科技 AT6010 60GHz 雷达 SOC
//   - 1T2R FMCW，59~64 GHz，探测距离 <= 10 m，水平/俯仰视角 ±60°
//   - 供电 3.0~5.5 V，平均 80 mA（建议 3.3 V，与 ESP32-S3 电平一致）
//   - 对外有两个 3pin 接口：
//       接口1: OUT(GPIO 输出) / GND / VCC
//       接口2: TX / GND / RX        <- 本驱动用这个
//   - UART 默认波特率 921600 bps，可切到 9600 / 115200
//
// 帧格式（全部小端）：
//   命令帧 0x58 : [HEAD][CMD][LEN][PARAM...][校验低][校验高]   校验 = 前面所有字节之和(16 位)
//   回复帧 0x59 : 同上
//   主动上报 0x5A: [HEAD][LEN][TYPE][字段...][校验]            校验 = 前面所有字节之和(8 位)
//
// 关键行为：只有检测到目标才会主动上报，没有目标时完全不发数据。
//           所以"收不到"不等于"没目标"，必须靠超时判定，不能当成传感器掉线。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 三种帧头
#define RADAR_HEAD_CMD      0x58
#define RADAR_HEAD_REPLY    0x59
#define RADAR_HEAD_REPORT   0x5A

// 出厂默认波特率
#define RADAR_BAUD_DEFAULT  921600

// det_result 位域（可组合）
#define RADAR_DET_APPROACHING   0x01   // 靠近
#define RADAR_DET_RECEDING      0x02   // 远离
#define RADAR_DET_MOTION        0x04   // 运动
#define RADAR_DET_MICRO         0x08   // 微动
#define RADAR_DET_BREATH        0x10   // 呼吸

// 主动上报 TYPE
#define RADAR_RPT_FULL          0      // 完整检测信息，21 字节
#define RADAR_RPT_HEIGHT        1      // 测高，5 字节
#define RADAR_RPT_OCCUPANCY     2      // 占位检测，5 字节
#define RADAR_RPT_MOTION        3      // 运动存在，9 字节
#define RADAR_RPT_BREATH        4      // 呼吸心率，9 字节
#define RADAR_RPT_REGION        5      // 分区检测，17 字节

typedef struct {
    uart_port_t uart_port;
    gpio_num_t  tx_gpio;    // ESP32 的 TX → 雷达的 RX
    gpio_num_t  rx_gpio;    // ESP32 的 RX ← 雷达的 TX
    int         baud_rate;
} radar_at6010_config_t;

// 默认配置。
// 引脚沿用《技术路线与调试记录》里定下的相机板预留脚：TX = GPIO14, RX = GPIO21。
// 这是规划值，不是实测值——接线前请自行确认，改这里一处即可。
#define RADAR_AT6010_CONFIG_DEFAULT()                  \
    (radar_at6010_config_t) {                          \
        .uart_port = UART_NUM_1,                       \
        .tx_gpio = GPIO_NUM_14,                        \
        .rx_gpio = GPIO_NUM_21,                        \
        .baud_rate = RADAR_BAUD_DEFAULT,               \
    }

// 软硬件版本号（命令 0xFE）
typedef struct {
    uint8_t sw_major;
    uint8_t sw_minor;
    uint8_t sw_revision;
    uint8_t cust_major;
    uint8_t cust_minor;
    uint8_t hw_major;
    uint8_t hw_minor;
    uint8_t reserved;
} radar_version_t;

// 算法边界值（命令 0x32），单位 cm，只读
typedef struct {
    uint16_t mot_min_cm;
    uint16_t mot_max_cm;
    uint16_t micro_min_cm;
    uint16_t micro_max_cm;
    uint16_t bhr_min_cm;
    uint16_t bhr_max_cm;
    uint16_t sweep_bw;
} radar_bounds_t;

// 用户配置（命令 0x33）
typedef struct {
    uint16_t mot_min_cm;
    uint16_t mot_max_cm;
    uint8_t  mot_sensitivity;
    uint16_t micro_min_cm;
    uint16_t micro_max_cm;
    uint8_t  micro_sensitivity;
    uint16_t bhr_min_cm;
    uint16_t bhr_max_cm;
    uint8_t  bhr_sensitivity;
} radar_algo_config_t;

// 一帧检测结果（0x30 查询结果，或 0x5A 主动上报解析结果）
typedef struct {
    uint8_t  type;              // 来源类型 RADAR_RPT_xxx
    uint8_t  source;            // 0 = 0x30 查询，1 = 0x5A 主动上报
    uint8_t  is_detected;       // 运动/微动/存在综合判定
    uint8_t  det_result;        // 位域，见 RADAR_DET_xxx
    uint16_t range_mm;          // 距离，毫米
    int16_t  angle_deg;         // 角度，度
    int16_t  velocity;          // 速度：协议标注"目前预留"，拿不到有效值
    uint8_t  range_confidence;  // 0~16，小于 12 时距离可能不准
    uint8_t  angle_confidence;  // 0~16，小于 8 时角度可能不准
    uint32_t frame_index;
    uint16_t height_mm;         // TYPE 1 测高
    uint8_t  height_status;
    uint8_t  breath_rate;       // TYPE 4
    uint8_t  heart_rate;
    uint32_t object_num;        // TYPE 5 分区检测
    struct {
        uint16_t range_mm;
        int16_t  angle_deg;
    } region[3];
    int64_t  timestamp_us;
} radar_report_t;

// 调试统计
typedef struct {
    uint32_t cmd_sent;
    uint32_t reply_ok;
    uint32_t reply_timeout;
    uint32_t checksum_error;
    uint32_t unsolicited_count;
    uint32_t unsolicited_dropped;
} radar_stats_t;

// 初始化 UART 并安装驱动
esp_err_t radar_at6010_init(const radar_at6010_config_t *cfg);

// 只切本机波特率（不动模组）
esp_err_t radar_at6010_set_local_baud(int baud_rate);

// 让模组自己切换波特率（命令 0x19）
esp_err_t radar_at6010_set_module_baud(int new_baud);

// 版本、开关、边界值、用户配置
esp_err_t radar_at6010_get_version(radar_version_t *out);
esp_err_t radar_at6010_get_sensing(bool *on);
esp_err_t radar_at6010_set_sensing(bool on);
esp_err_t radar_at6010_get_bounds(radar_bounds_t *out);
esp_err_t radar_at6010_get_algo_config(radar_algo_config_t *out);
esp_err_t radar_at6010_save_settings(void);

// 主动查询一次检测信息（命令 0x30）
esp_err_t radar_at6010_query_detection(radar_report_t *out);

// 取回一条"顺带收到"的 0x5A 主动上报帧。返回 1 = 取到，0 = 暂无，负 = 出错
int radar_at6010_take_unsolicited(radar_report_t *out);

const radar_stats_t *radar_at6010_stats(void);

// 翻译函数
const char *radar_at6010_det_desc(uint8_t det_result, char *buf, size_t buf_len);
const char *radar_at6010_type_name(uint8_t type);

// 打印一帧全部字段，调试用
void radar_report_print(const radar_report_t *r, const char *prefix);

// 暴露 UART 端口号，供 main.c 做裸字节诊断
uart_port_t radar_at6010_get_uart_port(void);

// 清空接收缓冲区
void radar_at6010_flush_input(void);

#ifdef __cplusplus
}
#endif