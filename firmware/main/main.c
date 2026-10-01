/*
 * 智行护航 —— 面向果园小型农机的端侧多模态融合主动安全头盔系统
 * Phase 1 + Phase 2 骨架工程（ESP-IDF v6.0.2 / ESP32-S3-N16R8）
 *
 * 本文件只做两件事：
 *   1) Phase 1：把板载 LED 跑起来（用闪烁频率表示风险等级）
 *   2) Phase 2：跑通 Task → Queue → Task 的数据通路，
 *      并演示 Queue / Semaphore / Mutex 三种同步原语
 *
 * 注意：传感器数据目前是"模拟"出来的。Phase 3 接入真实雷达/IMU 时，
 * 只需替换 task_radar_sim，融合层（task_fusion）的代码不用改。
 * 模拟数据源与真实数据源使用同一个 sensor_msg_t 信封。
 *
 * 任务一览（对应 AGENTS.md 的 FreeRTOS 规则）：
 *   任务名            职责                     优先级  栈(字)  通信方式
 *   ---------------  -----------------------  ------  ------  ------------------------------------
 *   task_radar_sim   模拟雷达周期采集          5       3072    向 s_sensor_queue 发送 sensor_msg_t
 *   task_fusion      融合 + 风险判定 + 统计    4       4096    收 s_sensor_queue；统计受 Mutex 保护；
 *                                                              xQueueOverwrite 写 s_led_queue
 *   task_button      响应 BOOT 按键事件        3       3072    等 s_button_sem（ISR 给出）
 *   task_led         按风险等级驱动板载 LED    2       2048    收 s_led_queue（深度 1，只取最新值）
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"

#include "board_config.h"

static const char *TAG = "app";

/* ============================================================
 * 数据结构
 * ============================================================ */

/* 数据来源。融合层靠它分辨这一帧来自哪个传感器 */
typedef enum {
    SRC_RADAR = 0,
    SRC_IMU,
    SRC_CAMERA,
} sensor_src_t;

/* 传感器消息统一信封。
 * 关键点：一定要带 timestamp_us —— 三路传感器采样率不同，
 * 没有时间戳就无法做正确的时间对齐，融合会变成"把不同时刻的数据凑一起"。
 */
typedef struct {
    sensor_src_t src;
    uint32_t     seq;            /* 序号，用于统计丢帧 */
    int64_t      timestamp_us;   /* 采集时刻，单位微秒 */
    int32_t      distance_mm;    /* 示例负载：Phase 3 换成真正的雷达结构体 */
} sensor_msg_t;

typedef enum {
    RISK_NORMAL = 0,
    RISK_WARNING,
    RISK_DANGER,
} risk_level_t;

/* 需要被多个任务访问的共享数据，统一用 s_stats_mutex 保护 */
typedef struct {
    risk_level_t level;
    int32_t      distance_mm;
    uint32_t     rx_count;       /* 融合层收到的帧数 */
    uint32_t     drop_count;     /* 模拟采集端发送失败的次数 */
    uint32_t     button_count;   /* 按键事件次数 */
} app_stats_t;

/* ============================================================
 * 全局句柄与共享数据
 * ============================================================ */
static QueueHandle_t     s_sensor_queue;   /* 线程安全：队列本身即可跨任务传递 */
static QueueHandle_t     s_led_queue;      /* 深度 1，只保留最新风险等级 */
static SemaphoreHandle_t s_button_sem;     /* 二值信号量：ISR → 任务 的事件同步 */
static SemaphoreHandle_t s_stats_mutex;    /* 互斥锁：保护 s_stats */
static app_stats_t       s_stats;

/* ============================================================
 * 按键中断：演示 Semaphore 用于"同步事件"
 * 中断里只做一件事——给信号量，绝不做打印、计算这类耗时操作
 * ============================================================ */
static void IRAM_ATTR button_isr(void *arg)
{
    BaseType_t high_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_button_sem, &high_task_woken);
    if (high_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* ============================================================
 * 任务 1：模拟雷达采集（优先级 5，栈 3072 字）
 * 模拟一个目标"由远及近，再由近及远"的距离变化
 * ============================================================ */
static void task_radar_sim(void *arg)
{
    (void)arg;
    uint32_t seq = 0;
    int32_t  distance = APP_RISK_WARNING_MM;
    int32_t  step = -60;   /* 每周期变化量，单位 mm */
    uint32_t local_drop = 0;

    while (1) {
        distance += step;
        if (distance <= 200) {
            distance = 200;
            step = 60;         /* 掉头，开始远离 */
        } else if (distance >= APP_RISK_WARNING_MM) {
            distance = APP_RISK_WARNING_MM;
            step = -60;
        }

        sensor_msg_t msg = {
            .src = SRC_RADAR,
            .seq = seq++,
            .timestamp_us = esp_timer_get_time(),
            .distance_mm = distance,
        };

        /* 队列满时最多等 50ms 就放弃，避免采集任务被融合任务拖死。
         * 真实场景里这里应该"丢弃最旧数据、保留最新数据"，
         * 所以 Phase 3 的真实雷达队列建议用深度 1 + xQueueOverwrite。
         */
        if (xQueueSend(s_sensor_queue, &msg, pdMS_TO_TICKS(50)) != pdTRUE) {
            local_drop++;
            xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
            s_stats.drop_count = local_drop;
            xSemaphoreGive(s_stats_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(APP_SIM_PERIOD_MS));
    }
}

/* ============================================================
 * 任务 2：融合 + 决策（优先级 4，栈 4096 字）
 * 目前是规则判断；Phase 4 换成真正的融合算法
 * ============================================================ */
static void task_fusion(void *arg)
{
    (void)arg;
    sensor_msg_t msg;

    while (1) {
        if (xQueueReceive(s_sensor_queue, &msg, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;   /* 超时：真实系统里这里应该判"传感器掉线" */
        }

        risk_level_t level = RISK_NORMAL;
        if (msg.distance_mm < APP_RISK_DANGER_MM) {
            level = RISK_DANGER;
        } else if (msg.distance_mm < APP_RISK_WARNING_MM) {
            level = RISK_WARNING;
        }

        /* 把最新等级覆盖到深度 1 的队列，LED 任务永远只处理最新状态 */
        xQueueOverwrite(s_led_queue, &level);

        xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
        s_stats.level = level;
        s_stats.distance_mm = msg.distance_mm;
        s_stats.rx_count++;
        uint32_t rx = s_stats.rx_count;
        xSemaphoreGive(s_stats_mutex);

        if (rx % 10 == 0) {
            static const char *level_name[] = {"安全", "警告", "危险"};
            ESP_LOGI(TAG, "[融合] seq=%" PRIu32 " 距离=%" PRId32 "mm 等级=%s (已处理 %" PRIu32 " 帧)",
                     msg.seq, msg.distance_mm, level_name[level], rx);
        }
    }
}

/* ============================================================
 * 任务 3：按键事件（优先级 3，栈 3072 字）
 * 演示阻塞等待信号量。Phase 3 可以把它改成"雷达标定"/"切换模式"
 * ============================================================ */
static void task_button(void *arg)
{
    (void)arg;

    while (1) {
        if (xSemaphoreTake(s_button_sem, portMAX_DELAY) == pdTRUE) {
            xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
            s_stats.button_count++;
            uint32_t n = s_stats.button_count;
            xSemaphoreGive(s_stats_mutex);
            ESP_LOGI(TAG, "[按键] 收到第 %" PRIu32 " 次按下事件（这里将来接标定/切换逻辑）", n);
        }
    }
}

/* ============================================================
 * 任务 4：LED 指示（优先级 2，栈 2048 字）
 * 安全=1s 慢闪，警告=300ms 闪，危险=100ms 快闪
 * 注意：这里要等队列超时，所以不能无限阻塞
 * ============================================================ */
static void task_led(void *arg)
{
    (void)arg;
    risk_level_t level = RISK_NORMAL;
    int          on = 0;

    gpio_set_level(BOARD_LED_GPIO, !BOARD_LED_ACTIVE_LEVEL);

    while (1) {
        TickType_t wait_ticks;
        switch (level) {
        case RISK_DANGER:  wait_ticks = pdMS_TO_TICKS(100); break;
        case RISK_WARNING: wait_ticks = pdMS_TO_TICKS(300); break;
        default:           wait_ticks = pdMS_TO_TICKS(1000); break;
        }

        /* 有新等级就立刻采用，没有就等超时后照常翻灯 */
        (void)xQueueReceive(s_led_queue, &level, wait_ticks);

        on = !on;
        gpio_set_level(BOARD_LED_GPIO, on ? BOARD_LED_ACTIVE_LEVEL : !BOARD_LED_ACTIVE_LEVEL);
    }
}

/* ============================================================
 * 初始化
 * ============================================================ */
static void print_chip_info(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "芯片: %s  核心数: %d  硅片版本: v%d.%d",
             CONFIG_IDF_TARGET, info.cores, info.revision / 100, info.revision % 100);
    ESP_LOGI(TAG, "Flash: %" PRIu32 " MB", flash_size / (1024 * 1024));
#if CONFIG_SPIRAM
    ESP_LOGI(TAG, "PSRAM: %u KB", (unsigned)(esp_psram_get_size() / 1024));
#else
    ESP_LOGW(TAG, "PSRAM: 未启用（摄像头帧缓冲会不够用，请检查 sdkconfig）");
#endif
    ESP_LOGI(TAG, "ESP-IDF: %s", esp_get_idf_version());
}

static void init_gpio(void)
{
    /* 板载 LED */
    gpio_config_t led_cfg = {
        .pin_bit_mask = 1ULL << BOARD_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led_cfg));
    gpio_set_level(BOARD_LED_GPIO, !BOARD_LED_ACTIVE_LEVEL);

    /* BOOT 按键：按下为低电平，用下降沿中断 */
#if BOARD_HAS_BUTTON
    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << BOARD_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BOARD_BUTTON_GPIO, button_isr, NULL));
#endif
}

void app_main(void)
{
    ESP_LOGI(TAG, "===== 智行护航 firmware 启动 =====");
    print_chip_info();

    init_gpio();

    /* 队列：传递传感器数据（AGENTS.md：Queue 用于传递传感器数据） */
    s_sensor_queue = xQueueCreate(APP_SENSOR_QUEUE_LEN, sizeof(sensor_msg_t));
    s_led_queue = xQueueCreate(1, sizeof(risk_level_t));
    /* 信号量：ISR 与任务之间同步事件 */
    s_button_sem = xSemaphoreCreateBinary();
    /* 互斥锁：保护共享的统计结构体 */
    s_stats_mutex = xSemaphoreCreateMutex();

    if (s_sensor_queue == NULL || s_led_queue == NULL ||
        s_button_sem == NULL || s_stats_mutex == NULL) {
        ESP_LOGE(TAG, "RTOS 对象创建失败，系统中止");
        return;
    }

    /* 显式指定核心：Level 1 任务放 core 1，避免和 core 0 的 WiFi/蓝牙抢 */
    xTaskCreatePinnedToCore(task_radar_sim, "radar_sim", 3072, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(task_fusion,    "fusion",    4096, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(task_button,    "button",    3072, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(task_led,       "led",       2048, NULL, 2, NULL, 0);

    ESP_LOGI(TAG, "4 个任务已启动，按 BOOT 键可触发一次事件");
}
