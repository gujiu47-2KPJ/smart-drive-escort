/**
 * 智行护航 - 串口摄像头推流固件
 *
 * 作用：把 OV2640 采集到的 JPEG 画面，通过 USB 串口(UART0 -> COM5)
 *       以「帧头 + JPEG 数据」的二进制格式推送到电脑，供 Python 端实时显示。
 *
 * 为什么用串口而不是 WiFi：
 *   1) 串口是开发板上最稳定、最直接的通道，不需要路由器/热点；
 *   2) 适合快速验证摄像头画面，以及后续在电脑上做视觉算法调试。
 *
 * 帧协议(小端)：
 *   [0xAA][0xBB][长度低8位][长度高8位][JPEG 数据...]
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "driver/uart.h"

static const char *TAG = "serial_cam";

/* OV2640 引脚定义（与 Freenove ESP32-S3-WROOM / ESP32-S3-EYE 一致） */
#define CAM_PIN_D0     11
#define CAM_PIN_D1      9
#define CAM_PIN_D2      8
#define CAM_PIN_D3     10
#define CAM_PIN_D4     12
#define CAM_PIN_D5     18
#define CAM_PIN_D6     17
#define CAM_PIN_D7     16
#define CAM_PIN_XCLK   15
#define CAM_PIN_PCLK   13
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_SIOD    4   /* SCCB SDA */
#define CAM_PIN_SIOC    5   /* SCCB SCL */
#define CAM_PIN_PWDN   -1
#define CAM_PIN_RESET  -1

/* 串口帧头魔数，用于电脑端定位每一帧的起点 */
#define FRAME_MAGIC0  0xAA
#define FRAME_MAGIC1  0xBB

/* 串口波特率：921600 比 115200 快 8 倍，是流畅显示的关键 */
#define SERIAL_BAUD   921600
/* ESP32-S3 UART0 默认引脚（板载 CH343 转 USB 走 UART0） */
#define UART_TX_PIN   43
#define UART_RX_PIN   44

static void uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate  = SERIAL_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* 已通过 CONFIG_ESP_CONSOLE_NONE 关闭 UART0 控制台，这里独占 UART0 */
    uart_driver_install(UART_NUM_0, 8192, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_0, &uart_config);
    uart_set_pin(UART_NUM_0, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

static esp_err_t camera_init(void)
{
    /* 关键配置：
     * - pixel_format = JPEG：让摄像头直接输出压缩好的 JPEG，
     *   避免 RGB565 原始数据量过大（DVP 带宽/串口都扛不住）。
     * - xclk_freq_hz = 20MHz：OV2640 稳定工作的时钟。
     * - fb_location = PSRAM：帧缓冲放到 8MB 外部 PSRAM。
     * - fb_count = 2：双缓冲，采集与发送可以并行。
     */
    camera_config_t config = {
        .pin_pwdn      = CAM_PIN_PWDN,
        .pin_reset     = CAM_PIN_RESET,
        .pin_xclk      = CAM_PIN_XCLK,
        .pin_sccb_sda  = CAM_PIN_SIOD,
        .pin_sccb_scl  = CAM_PIN_SIOC,
        .pin_d7        = CAM_PIN_D7,
        .pin_d6        = CAM_PIN_D6,
        .pin_d5        = CAM_PIN_D5,
        .pin_d4        = CAM_PIN_D4,
        .pin_d3        = CAM_PIN_D3,
        .pin_d2        = CAM_PIN_D2,
        .pin_d1        = CAM_PIN_D1,
        .pin_d0        = CAM_PIN_D0,
        .pin_vsync     = CAM_PIN_VSYNC,
        .pin_href      = CAM_PIN_HREF,
        .pin_pclk      = CAM_PIN_PCLK,
        .xclk_freq_hz  = 20000000,
        .pixel_format  = PIXFORMAT_JPEG,
        .frame_size    = FRAMESIZE_QVGA,   /* 320x240，串口带宽下的帧率最优解 */
        .jpeg_quality  = 12,               /* 0-63，越小越清晰 */
        .fb_count      = 2,
        .grab_mode     = CAMERA_GRAB_WHEN_EMPTY,
        .fb_location   = CAMERA_FB_IN_PSRAM,
        .sccb_i2c_port = -1,               /* 使用引脚方式，不占用已有 I2C 总线 */
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed: 0x%x", err);
        return err;
    }

    /* 开启 50Hz 抗频闪（banding filter），消除室内灯光造成的横向条纹。
     * COM3(0x0C) 位[2:1] = 0b10 -> 50Hz；COM8(0x13) 位5 -> 使能带通滤波。
     * reg 高字节 bit8 = bank（0=与驱动 COM7 同组）。 */
    sensor_t *s = esp_camera_sensor_get();
    if (s && s->set_reg) {
        int r1 = s->set_reg(s, 0x000C, 0x06, 0x04);
        int r2 = s->set_reg(s, 0x0013, 0x20, 0x20);
        ESP_LOGI(TAG, "anti-flicker(50Hz) set: com3=%d com8=%d", r1, r2);
    }

    ESP_LOGI(TAG, "camera init OK");
    return ESP_OK;
}

void app_main(void)
{
    /* 配置 UART0 并独占使用（控制台已关闭） */
    uart_init();

    if (camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed, halt");
        return;
    }

    /* 等摄像头稳定出流 */
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "start streaming at %d baud", SERIAL_BAUD);

    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == NULL) {
            ESP_LOGW(TAG, "fb get failed");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* 组帧：4 字节帧头 + JPEG 数据 */
        uint8_t hdr[4] = {
            FRAME_MAGIC0,
            FRAME_MAGIC1,
            (uint8_t)(fb->len & 0xFF),
            (uint8_t)((fb->len >> 8) & 0xFF),
        };

        /* 用 uart_write_bytes 直接写原始字节（UART0 已被本程序独占）。
         * 阻塞式写入天然起到「背压」作用：串口忙时不会无限积压，防止丢帧/内存爆掉。 */
        uart_write_bytes(UART_NUM_0, (const char *)hdr, sizeof(hdr));
        uart_write_bytes(UART_NUM_0, (const char *)fb->buf, fb->len);

        esp_camera_fb_return(fb);
    }
}
