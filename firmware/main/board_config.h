/*
 * 板型与引脚配置
 *
 * 所有引脚号只在这一个文件里定义，其它代码一律引用宏，
 * 这样换板 / 改接线只改这里，不用动业务代码。
 *
 * 参考资料（Technical Support Document）：
 *   - Freenove ESP32-S3 WROOM 引脚图：资料/ESP32-S3 CAM开发板资料/ESP32S3_Pinout.png
 *   - Freenove 官方 Blink 例程：LED_BUILTIN = 2
 *   - Freenove 官方 WS2812 例程：WS2812_PIN = 48
 *   - Freenove 官方 SDMMC 例程：CLK=39, CMD=38, D0=40（请注意不可修改）
 *   - ESP32-S3-WROOM-1 数据手册 P11 注 b：
 *     N16R8 模组的 IO35 / IO36 / IO37 已接 OSPI PSRAM，不可作他用
 */

#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"

/* ============================================================
 * 1. 板型选择
 * ============================================================ */
#define BOARD_FREENOVE_S3_CAM   1   /* 带 DVP 摄像头底座 + microSD 的板 */
#define BOARD_MINIMAL_S3        2   /* 最小系统板（型号待确认） */

#ifndef BOARD_SELECT
#define BOARD_SELECT            BOARD_FREENOVE_S3_CAM
#endif

/* ============================================================
 * 2. Freenove ESP32-S3 WROOM（摄像头板）
 * ============================================================ */
#if BOARD_SELECT == BOARD_FREENOVE_S3_CAM

/* ---- 2.1 板载指示与按键 ---- */
#define BOARD_LED_GPIO          GPIO_NUM_2   /* 板载普通 LED，高电平点亮 */
#define BOARD_LED_ACTIVE_LEVEL  1
#define BOARD_WS2812_GPIO       GPIO_NUM_48  /* 可寻址 RGB 灯，需 RMT 驱动（后续再加） */
#define BOARD_HAS_BUTTON        1
#define BOARD_BUTTON_GPIO       GPIO_NUM_0   /* BOOT 键，按下为低电平 */

/* ---- 2.2 摄像头 OV2640（DVP 并口） ---- */
/* 对应 Arduino 例程中的 CAMERA_MODEL_ESP32S3_EYE，不是 ESP_EYE */
#define CAM_PIN_XCLK            GPIO_NUM_15
#define CAM_PIN_PCLK            GPIO_NUM_13
#define CAM_PIN_VSYNC           GPIO_NUM_6
#define CAM_PIN_HREF            GPIO_NUM_7
#define CAM_PIN_SIOD            GPIO_NUM_4
#define CAM_PIN_SIOC            GPIO_NUM_5
#define CAM_PIN_D0              GPIO_NUM_11   /* Y2 */
#define CAM_PIN_D1              GPIO_NUM_9    /* Y3 */
#define CAM_PIN_D2              GPIO_NUM_8    /* Y4 */
#define CAM_PIN_D3              GPIO_NUM_10   /* Y5 */
#define CAM_PIN_D4              GPIO_NUM_12   /* Y6 */
#define CAM_PIN_D5              GPIO_NUM_18   /* Y7 */
#define CAM_PIN_D6              GPIO_NUM_17   /* Y8 */
#define CAM_PIN_D7              GPIO_NUM_16   /* Y9 */
#define CAM_PIN_PWDN            (-1)
#define CAM_PIN_RESET           (-1)

/* ---- 2.3 microSD（SDMMC 1-bit 模式） ---- */
#define SD_PIN_CLK              GPIO_NUM_39
#define SD_PIN_CMD              GPIO_NUM_38
#define SD_PIN_D0               GPIO_NUM_40

/* ---- 2.4 剩余可用 GPIO（摄像头 / SD / PSRAM / USB / 串口占完之后只剩这 5 个） ---- */
/* 其中 GPIO2 已被板载 LED 占用，实际可外接的只有 1 / 14 / 21 / 47 */

/* ---- 2.5 传感器预留接线（Phase 3 使用，目前尚未接线） ---- */
/* 雷达 MS60-1211S80M：UART1，默认波特率 921600 */
#define RADAR_UART_PORT         UART_NUM_1
#define RADAR_UART_TX_GPIO      GPIO_NUM_14   /* ESP32 TX → 雷达 RX */
#define RADAR_UART_RX_GPIO      GPIO_NUM_21   /* ESP32 RX ← 雷达 TX */
#define RADAR_UART_BAUD         921600

/* IMU HW-991(BMI270)：I2C0 */
#define IMU_I2C_PORT            I2C_NUM_0
#define IMU_I2C_SDA_GPIO        GPIO_NUM_1
#define IMU_I2C_SCL_GPIO        GPIO_NUM_47

#endif /* BOARD_FREENOVE_S3_CAM */

/* ============================================================
 * 3. 最小系统板（型号待确认，先用 ESP32-S3-DevKitC-1 常见默认值占位）
 *    拿到板子型号后请补全，并同步更新 ARCHITECTURE.md 的硬件章节
 * ============================================================ */
#if BOARD_SELECT == BOARD_MINIMAL_S3

#define BOARD_LED_GPIO          GPIO_NUM_48  /* 多数 ESP32-S3 开发板为 GPIO48，待确认 */
#define BOARD_LED_ACTIVE_LEVEL  1
#define BOARD_WS2812_GPIO       GPIO_NUM_48
#define BOARD_HAS_BUTTON        1
#define BOARD_BUTTON_GPIO       GPIO_NUM_0

#define RADAR_UART_PORT         UART_NUM_1
#define RADAR_UART_TX_GPIO      GPIO_NUM_17   /* 待确认 */
#define RADAR_UART_RX_GPIO      GPIO_NUM_18   /* 待确认 */
#define RADAR_UART_BAUD         921600

#define IMU_I2C_PORT            I2C_NUM_0
#define IMU_I2C_SDA_GPIO        GPIO_NUM_8    /* 待确认 */
#define IMU_I2C_SCL_GPIO        GPIO_NUM_9    /* 待确认 */

#endif /* BOARD_MINIMAL_S3 */

/* ============================================================
 * 4. 应用参数
 * ============================================================ */
#define APP_SENSOR_QUEUE_LEN    8      /* 传感器队列深度 */
#define APP_SIM_PERIOD_MS       100    /* 模拟数据周期 */
#define APP_RISK_DANGER_MM      500    /* 危险阈值（示例值，需实测标定） */
#define APP_RISK_WARNING_MM     1500   /* 警告阈值（示例值，需实测标定） */
