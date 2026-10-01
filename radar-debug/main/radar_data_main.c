// MS60-1211S80M 雷达 —— 数据读取程序（UART 版）
//
// 与上一版"抓包工具"的区别：通信已经实测打通，这里不再做诊断，
// 而是像 imu-debug 那样，稳定地把雷达数据打出来给调试用。
//
// 实测确认的协议（115200 / 8N1）：
//   帧头 0x58=上位机命令，0x59=模块回复，0x5A=主动上报
//   帧格式 [HEAD][CMD][LEN][参数...][校验低][校验高]，校验=前面字节之和(16位小端)
//   数据流靠轮询 0x30 获取，回复含：检测类型、距离(mm)、角度(°)、置信度、帧号
//
// 不使用 OUT 引脚 —— 数据全部从 UART 取。
// 接线只需 4 根：VCC->3V3、GND->GND、雷达TX->GPIO21、雷达RX->GPIO14、

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "radar_at6010.h"

// ---------------- 按需修改 ----------------
#define POLL_PERIOD_MS      100     // 轮询周期（实测协议每帧约 30ms 更新）
#define STATS_PERIOD_MS     5000    // 统计打印周期

// 波特率已实测确认为 115200，不再扫描其它值
#define RADAR_BAUD_CONFIRMED    115200

// ---------------- 统计 ----------------
typedef struct {
    uint32_t polls;
    uint32_t replies;
    uint32_t timeouts;
    uint32_t detected;
    uint32_t by_type[6];        // 按 det_result 位统计：靠近/远离/运动/微动/呼吸
    uint32_t min_range_mm;
    uint32_t max_range_mm;
    int16_t  min_angle;
    int16_t  max_angle;
    int64_t  first_us;
    int64_t  last_detected_us;
} run_stats_t;

static run_stats_t s_st;

static void stats_reset(void)
{
    memset(&s_st, 0, sizeof(s_st));
    s_st.min_range_mm = 0xFFFFFFFFu;
    s_st.min_angle = 32767;
    s_st.max_angle = -32768;
    s_st.first_us = esp_timer_get_time();
    s_st.last_detected_us = -1;
}

static void stats_update(const radar_report_t *r, bool ok)
{
    s_st.polls++;
    if (!ok) {
        s_st.timeouts++;
        return;
    }
    s_st.replies++;
    if (!r->is_detected) {
        return;
    }
    s_st.detected++;
    s_st.last_detected_us = esp_timer_get_time();

    static const uint8_t bit[5] = {
        RADAR_DET_APPROACHING, RADAR_DET_RECEDING, RADAR_DET_MOTION,
        RADAR_DET_MICRO, RADAR_DET_BREATH
    };
    for (int i = 0; i < 5; i++) {
        if (r->det_result & bit[i]) {
            s_st.by_type[i]++;
        }
    }

    if (r->range_mm < s_st.min_range_mm) {
        s_st.min_range_mm = r->range_mm;
    }
    if (r->range_mm > s_st.max_range_mm) {
        s_st.max_range_mm = r->range_mm;
    }
    if (r->angle_deg < s_st.min_angle) {
        s_st.min_angle = r->angle_deg;
    }
    if (r->angle_deg > s_st.max_angle) {
        s_st.max_angle = r->angle_deg;
    }
}

static void stats_print(void)
{
    const int64_t now = esp_timer_get_time();
    const double span = (double)(now - s_st.first_us) / 1e6;
    const radar_stats_t *ds = radar_at6010_stats();

    printf("\n----- 统计（运行 %.1f 秒）-----\n", span);
    printf("  轮询/回复/超时 : %u / %u / %u   实测 %.1f Hz\n",
           (unsigned)s_st.polls, (unsigned)s_st.replies, (unsigned)s_st.timeouts,
           (span > 0.0) ? ((double)s_st.polls / span) : 0.0);
    printf("  检测到目标     : %u 次（%.1f%%）\n", (unsigned)s_st.detected,
           (s_st.replies > 0) ? (100.0 * s_st.detected / s_st.replies) : 0.0);
    printf("  按类型统计     : 靠近 %u / 远离 %u / 运动 %u / 微动 %u / 呼吸 %u\n",
           (unsigned)s_st.by_type[0], (unsigned)s_st.by_type[1],
           (unsigned)s_st.by_type[2], (unsigned)s_st.by_type[3],
           (unsigned)s_st.by_type[4]);
    if (s_st.detected > 0) {
        printf("  距离范围       : %u ~ %u mm\n",
               (unsigned)s_st.min_range_mm, (unsigned)s_st.max_range_mm);
        printf("  角度范围       : %d ~ %d 度\n", s_st.min_angle, s_st.max_angle);
        if (s_st.last_detected_us >= 0) {
            printf("  距上次检测     : %.1f 秒前\n",
                   (double)(now - s_st.last_detected_us) / 1e6);
        }
    }
    printf("  底层链路       : 发命令 %u / 收回复 %u / 超时 %u / 校验错 %u\n",
           (unsigned)ds->cmd_sent, (unsigned)ds->reply_ok,
           (unsigned)ds->reply_timeout, (unsigned)ds->checksum_error);
    printf("--------------------------------\n\n");
}

// ---------------- 启动信息 ----------------
static void banner(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    printf("\n");
    printf("==========================================================\n");
    printf("  MS60-1211S80M 雷达 —— 数据读取程序（UART 版）\n");
    printf("==========================================================\n");
    printf("  芯片       : %s  核心 %d  IDF %s\n",
           CONFIG_IDF_TARGET, info.cores, esp_get_idf_version());
    printf("  雷达串口   : UART%d  115200 8N1（实测确认）\n", (int)RADAR_AT6010_CONFIG_DEFAULT().uart_port);
    printf("  接线       : 雷达 TX->GPIO%d  雷达 RX->GPIO%d  VCC->3V3  GND->GND\n",
           (int)RADAR_AT6010_CONFIG_DEFAULT().rx_gpio,
           (int)RADAR_AT6010_CONFIG_DEFAULT().tx_gpio);
    printf("  数据来源   : 轮询命令 0x30（不用 OUT 引脚）\n");
    printf("----------------------------------------------------------\n\n");
}

// ---------------- 连通性确认 ----------------
// 波特率 115200 是实测确认过的（921600 / 9600 都无应答），所以不再扫描。
// 这里只确认一次通信是否正常；失败就是接线问题。
static bool check_link(void)
{
    radar_report_t r;
    for (int k = 0; k < 3; k++) {
        if (radar_at6010_query_detection(&r) == ESP_OK) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

// ---------------- 读一次模块配置 ----------------
static void dump_config(void)
{
    bool on = false;
    if (radar_at6010_get_sensing(&on) == ESP_OK) {
        printf("  雷达感应开关 : %s\n", on ? "开" : "关");
    }

    radar_algo_config_t c;
    if (radar_at6010_get_algo_config(&c) == ESP_OK) {
        printf("  运动检测     : %u ~ %u cm   灵敏度 %u\n",
               (unsigned)c.mot_min_cm, (unsigned)c.mot_max_cm, (unsigned)c.mot_sensitivity);
        printf("  微动检测     : %u ~ %u cm   灵敏度 %u\n",
               (unsigned)c.micro_min_cm, (unsigned)c.micro_max_cm,
               (unsigned)c.micro_sensitivity);
        printf("  呼吸检测     : %u ~ %u cm   灵敏度 %u\n",
               (unsigned)c.bhr_min_cm, (unsigned)c.bhr_max_cm, (unsigned)c.bhr_sensitivity);
    } else {
        printf("  感应配置     : 读取失败\n");
    }

    radar_bounds_t b;
    if (radar_at6010_get_bounds(&b) == ESP_OK) {
        printf("  可设上限     : 运动 %u cm / 微动 %u cm / 呼吸 %u cm\n",
               (unsigned)b.mot_max_cm, (unsigned)b.micro_max_cm, (unsigned)b.bhr_max_cm);
    }
}

void app_main(void)
{
    banner();

    radar_at6010_config_t cfg = RADAR_AT6010_CONFIG_DEFAULT();
    cfg.baud_rate = RADAR_BAUD_CONFIRMED;

    printf("[步骤 1/3] 初始化 UART%d ...\n", (int)cfg.uart_port);
    esp_err_t err = radar_at6010_init(&cfg);
    if (err != ESP_OK) {
        printf("  失败: %s\n", esp_err_to_name(err));
        return;
    }
    printf("  就绪\n\n");

    printf("[步骤 2/3] 确认通信（%d，已实测确认，不再扫其它波特率）...\n",
           RADAR_BAUD_CONFIRMED);
    if (!check_link()) {
        printf("  无应答。请检查接线：\n");
        printf("    雷达 TX（接口2.1）-> GPIO%d\n", (int)cfg.rx_gpio);
        printf("    雷达 RX（接口2.3）-> GPIO%d\n", (int)cfg.tx_gpio);
        printf("    VCC -> 3V3，GND 与 ESP32 共地\n");
        return;
    }
    printf("  通信正常\n\n");

    printf("[步骤 3/3] 读取模块当前配置 ...\n");
    dump_config();
    printf("\n");

    printf("开始连续轮询（每 %d ms 一次）...\n", POLL_PERIOD_MS);
    printf("  说明：数据来自 0x30 轮询，不用 OUT 引脚\n\n");

    stats_reset();
    int64_t last_stats = esp_timer_get_time();
    uint32_t seq = 0;
    radar_report_t r;

    while (1) {
        const bool ok = (radar_at6010_query_detection(&r) == ESP_OK);
        if (ok) {
            char stamp[24];
            snprintf(stamp, sizeof(stamp), "%5u  ", (unsigned)seq++);
            if (r.is_detected) {
                radar_report_print(&r, stamp);
            } else {
                printf("%s未检测到目标（帧号 %u）\n", stamp, (unsigned)r.frame_index);
            }
        } else {
            printf("%5u  读取失败\n", (unsigned)seq++);
        }
        stats_update(&r, ok);

        const int64_t now = esp_timer_get_time();
        if ((now - last_stats) >= (int64_t)STATS_PERIOD_MS * 1000) {
            stats_print();
            last_stats = now;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}
