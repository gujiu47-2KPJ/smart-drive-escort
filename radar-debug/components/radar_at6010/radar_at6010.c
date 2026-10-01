// 觅感 MS60-1211S80M（AT6010）雷达驱动实现 —— ESP-IDF
//
// 命令码、帧格式、字段布局全部来自《AT6010 SOC HCI Protocol V1.0》，
// 未做任何猜测。两个容易记错的点：
//   1) 命令帧的校验码是 2 字节小端（例：58 FE 00 56 01，0x58+0xFE+0x00 = 0x156）
//   2) 主动上报帧 0x5A 是 1 字节校验，且只在检测到目标时才发

#include "radar_at6010.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "radar_at6010";

// ---------------- 指令码（HCI Protocol 3.x 节） ----------------
#define CMD_SET_RADAR_ON        0xD1   // 雷达感应开关
#define CMD_GET_RADAR_ON        0xD0
#define CMD_GET_DET_INFO        0x30   // 获取雷达感应信息
#define CMD_GET_BOUNDS          0x32   // 算法边界值
#define CMD_GET_ALGO_CONFIG     0x33   // 用户配置
#define CMD_SET_BAUD            0x19   // 波特率切换
#define CMD_SAVE_SETTINGS       0x08   // 保存到 flash
#define CMD_GET_VERSION         0xFE   // 软硬件版本号

#define UART_RX_BUF             4096
#define UART_TX_BUF             1024
#define REPLY_TIMEOUT_MS        300

static radar_at6010_config_t s_cfg;
static bool                  s_ready = false;
static radar_stats_t         s_stats;

// 顺带收到的 0x5A 主动上报，暂存一条
static radar_report_t s_pending;
static bool           s_pending_valid = false;

static int64_t        s_last_unsolicited_us = 0;

// ---------------- 小端读取工具 ----------------
static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static int16_t  rd_s16(const uint8_t *p) { return (int16_t)rd_u16(p); }
static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---------------- UART ----------------
esp_err_t radar_at6010_init(const radar_at6010_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "配置为空");
    ESP_RETURN_ON_FALSE(!s_ready, ESP_ERR_INVALID_STATE, TAG, "已初始化，勿重复调用");

    s_cfg = *cfg;

    uart_config_t uart_cfg = {
        .baud_rate = cfg->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(cfg->uart_port, UART_RX_BUF, UART_TX_BUF, 0, NULL, 0),
                        TAG, "安装 UART 驱动失败");
    ESP_RETURN_ON_ERROR(uart_param_config(cfg->uart_port, &uart_cfg), TAG, "配置 UART 参数失败");
    ESP_RETURN_ON_ERROR(uart_set_pin(cfg->uart_port, cfg->tx_gpio, cfg->rx_gpio,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "绑定 UART 引脚失败");

    // 丢弃上电瞬间的杂波
    uart_flush_input(cfg->uart_port);
    memset(&s_stats, 0, sizeof(s_stats));
    s_ready = true;
    return ESP_OK;
}

esp_err_t radar_at6010_set_local_baud(int baud_rate)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "尚未初始化");
    ESP_RETURN_ON_ERROR(uart_set_baudrate(s_cfg.uart_port, baud_rate), TAG, "切换波特率失败");
    s_cfg.baud_rate = baud_rate;
    uart_flush_input(s_cfg.uart_port);
    return ESP_OK;
}

esp_err_t radar_at6010_set_module_baud(int new_baud)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "尚未初始化");
    uint8_t p[4] = {
        (uint8_t)(new_baud & 0xFF),
        (uint8_t)((new_baud >> 8) & 0xFF),
        (uint8_t)((new_baud >> 16) & 0xFF),
        (uint8_t)((new_baud >> 24) & 0xFF),
    };
    uint8_t buf[16];
    uint8_t frame[16];
    frame[0] = RADAR_HEAD_CMD;
    frame[1] = CMD_SET_BAUD;
    frame[2] = 4;
    memcpy(&frame[3], p, 4);
    uint16_t sum = 0;
    for (int i = 0; i < 7; i++) {
        sum = (uint16_t)(sum + frame[i]);
    }
    frame[7] = (uint8_t)(sum & 0xFF);
    frame[8] = (uint8_t)(sum >> 8);
    uart_write_bytes(s_cfg.uart_port, frame, 9);
    uart_wait_tx_done(s_cfg.uart_port, pdMS_TO_TICKS(100));
    s_stats.cmd_sent++;

    int64_t deadline = esp_timer_get_time() + (int64_t)REPLY_TIMEOUT_MS * 1000;
    while (esp_timer_get_time() < deadline) {
        int r = uart_read_bytes(s_cfg.uart_port, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (r > 0) {
            break;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    return radar_at6010_set_local_baud(new_baud);
}

// ---------------- 收发 ----------------
static int read_exact(uint8_t *dst, size_t n, int64_t deadline_us)
{
    size_t got = 0;
    while (got < n) {
        int64_t remain = deadline_us - esp_timer_get_time();
        if (remain <= 0) {
            return -(int)(n - got);
        }
        TickType_t t = pdMS_TO_TICKS((uint32_t)(remain / 1000));
        if (t == 0) {
            t = 1;
        }
        int r = uart_read_bytes(s_cfg.uart_port, dst + got, n - got, t);
        if (r > 0) {
            got += (size_t)r;
        }
    }
    return (int)got;
}

// 读一帧。返回 >0 = 帧长度，0 = 超时，<0 = 错误
static int read_frame(uint8_t *out, size_t max, int64_t deadline_us)
{
    uint8_t b;
    while (esp_timer_get_time() < deadline_us) {
        if (read_exact(&b, 1, deadline_us) != 1) {
            return 0;
        }
        if (b != RADAR_HEAD_CMD && b != RADAR_HEAD_REPLY && b != RADAR_HEAD_REPORT) {
            continue;   // 不是帧头，丢掉继续找
        }
        out[0] = b;

        if (b == RADAR_HEAD_REPORT) {
            // [HEAD][LEN][PAYLOAD...][8 位校验]
            if (read_exact(&out[1], 1, deadline_us) != 1) {
                return -1;
            }
            size_t total = 2u + out[1] + 1u;
            if (total > max) {
                return -3;
            }
            // 注意：read_exact 成功时返回「实际读到的字节数」，
            // 所以必须拿它和目标长度比，不能一律写 != 1。
            if (read_exact(&out[2], total - 2, deadline_us) != (int)(total - 2)) {
                return -1;
            }
            uint8_t s = 0;
            for (size_t i = 0; i + 1 < total; i++) {
                s = (uint8_t)(s + out[i]);
            }
            if (s != out[total - 1]) {
                s_stats.checksum_error++;
                return -2;
            }
            return (int)total;
        }

        // [HEAD][CMD][LEN][PARAM...][校验低][校验高]
        // 同上：这里要读 2 字节，成功返回 2，写成 != 1 会永远判为失败。
        if (read_exact(&out[1], 2, deadline_us) != 2) {
            return -1;
        }
        size_t total = 3u + out[2] + 2u;
        if (total > max) {
            return -3;
        }
        if (read_exact(&out[3], total - 3, deadline_us) != (int)(total - 3)) {
            return -1;
        }
        uint16_t s = 0;
        for (size_t i = 0; i + 2 < total; i++) {
            s = (uint16_t)(s + out[i]);
        }
        if ((uint8_t)(s & 0xFF) != out[total - 2] || (uint8_t)(s >> 8) != out[total - 1]) {
            s_stats.checksum_error++;
            return -2;
        }
        return (int)total;
    }
    return 0;
}

// 解析一帧检测数据体（fields 从 is_detected 开始，TYPE 0 布局）
static void parse_det_fields(const uint8_t *f, radar_report_t *r)
{
    r->is_detected = f[0];
    r->det_result = f[1];
    r->range_mm = rd_u16(&f[2]);
    r->angle_deg = rd_s16(&f[4]);
    r->velocity = rd_s16(&f[6]);
    // f[8..13] 为 reserved[6]
    r->range_confidence = f[14];
    r->angle_confidence = f[15];
    r->frame_index = rd_u32(&f[16]);
}

// 解析一帧 0x5A 主动上报
static void parse_unsolicited(const uint8_t *frame, size_t len, radar_report_t *r)
{
    memset(r, 0, sizeof(*r));
    r->source = 1;
    r->timestamp_us = esp_timer_get_time();

    size_t plen = frame[1];
    const uint8_t *p = &frame[2];
    if (plen == 0 || len < 2u + plen + 1u) {
        return;
    }
    r->type = p[0];

    switch (r->type) {
    case RADAR_RPT_FULL:            // 21 字节：TYPE + 20
        if (plen >= 21) {
            parse_det_fields(&p[1], r);
        }
        break;
    case RADAR_RPT_HEIGHT:          // 5 字节：TYPE + htm(2) + status(1) + reserved(1)
        if (plen >= 5) {
            r->height_mm = rd_u16(&p[1]);
            r->height_status = p[3];
        }
        break;
    case RADAR_RPT_OCCUPANCY:       // 5 字节：TYPE + rb(1) + 3 个 1 字节量
        if (plen >= 5) {
            r->range_confidence = p[3];     // 复用字段：带距离补偿后的能量
            r->angle_confidence = p[4];     // 复用字段：超阈邻域数
        }
        break;
    case RADAR_RPT_MOTION:          // 9 字节：TYPE + 8
        if (plen >= 9) {
            r->is_detected = p[1];
            r->det_result = p[2];
            r->range_mm = rd_u16(&p[3]);
            r->angle_deg = rd_s16(&p[5]);
            r->velocity = rd_s16(&p[7]);
        }
        break;
    case RADAR_RPT_BREATH:          // 9 字节：TYPE + det_result + br + hr + angle(1) + range(2) + padding(2)
        if (plen >= 9) {
            r->det_result = p[1];
            r->breath_rate = p[2];
            r->heart_rate = p[3];
            r->range_mm = rd_u16(&p[5]);
        }
        break;
    case RADAR_RPT_REGION:          // 17 字节：TYPE + obj_num(4) + 3*(range(2)+angle(2))
        if (plen >= 17) {
            r->object_num = rd_u32(&p[1]);
            for (int i = 0; i < 3; i++) {
                r->region[i].range_mm = rd_u16(&p[5 + i * 4]);
                r->region[i].angle_deg = rd_s16(&p[7 + i * 4]);
            }
        }
        break;
    default:
        break;
    }
}

static void cache_unsolicited(const uint8_t *frame, size_t len)
{
    s_stats.unsolicited_count++;
    s_last_unsolicited_us = esp_timer_get_time();
    radar_report_t r;
    parse_unsolicited(frame, len, &r);
    if (s_pending_valid) {
        s_stats.unsolicited_dropped++;   // 上一帧还没被取走
    }
    s_pending = r;
    s_pending_valid = true;
}

int radar_at6010_take_unsolicited(radar_report_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "输出指针为空");
    if (!s_pending_valid) {
        return 0;
    }
    *out = s_pending;
    s_pending_valid = false;
    return 1;
}

// 发命令并等回复；顺带收到的 0x5A 会被缓存而不是丢弃
static esp_err_t transact(uint8_t cmd, const uint8_t *params, uint8_t n,
                          uint8_t *out_params, uint8_t *out_len, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "尚未初始化");
    ESP_RETURN_ON_FALSE((size_t)n + 5u <= 64u, ESP_ERR_INVALID_SIZE, TAG, "参数过长");

    uint8_t frame[64];
    frame[0] = RADAR_HEAD_CMD;
    frame[1] = cmd;
    frame[2] = n;
    if (n > 0 && params != NULL) {
        memcpy(&frame[3], params, n);
    }
    uint16_t sum = 0;
    for (int i = 0; i < 3 + n; i++) {
        sum = (uint16_t)(sum + frame[i]);
    }
    frame[3 + n] = (uint8_t)(sum & 0xFF);
    frame[4 + n] = (uint8_t)(sum >> 8);

    uart_write_bytes(s_cfg.uart_port, frame, 5 + n);
    uart_wait_tx_done(s_cfg.uart_port, pdMS_TO_TICKS(100));
    s_stats.cmd_sent++;

    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    uint8_t buf[160];
    while (esp_timer_get_time() < deadline) {
        int r = read_frame(buf, sizeof(buf), deadline);
        if (r == 0) {
            break;
        }
        if (r < 0) {
            continue;   // 校验错或帧异常，继续找下一帧
        }
        if (buf[0] == RADAR_HEAD_REPORT) {
            cache_unsolicited(buf, (size_t)r);
            continue;
        }
        if (buf[0] != RADAR_HEAD_REPLY || buf[1] != cmd) {
            continue;
        }
        uint8_t len = buf[2];
        if (len > 0 && out_params != NULL) {
            memcpy(out_params, &buf[3], len);
        }
        if (out_len != NULL) {
            *out_len = len;
        }
        s_stats.reply_ok++;
        return ESP_OK;
    }
    s_stats.reply_timeout++;
    return ESP_ERR_TIMEOUT;
}

// ---------------- 具体命令 ----------------
esp_err_t radar_at6010_get_version(radar_version_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "输出指针为空");
    uint8_t p[16] = { 0 };
    uint8_t len = 0;
    ESP_RETURN_ON_ERROR(transact(CMD_GET_VERSION, NULL, 0, p, &len, REPLY_TIMEOUT_MS),
                        TAG, "读版本号失败");
    ESP_RETURN_ON_FALSE(len >= 8, ESP_ERR_INVALID_RESPONSE, TAG, "版本回复长度异常 %u", len);
    out->sw_major = p[0];
    out->sw_minor = p[1];
    out->sw_revision = p[2];
    out->cust_major = p[3];
    out->cust_minor = p[4];
    out->hw_major = p[5];
    out->hw_minor = p[6];
    out->reserved = p[7];
    return ESP_OK;
}

esp_err_t radar_at6010_get_sensing(bool *on)
{
    ESP_RETURN_ON_FALSE(on != NULL, ESP_ERR_INVALID_ARG, TAG, "输出指针为空");
    uint8_t p[4] = { 0 };
    uint8_t len = 0;
    ESP_RETURN_ON_ERROR(transact(CMD_GET_RADAR_ON, NULL, 0, p, &len, REPLY_TIMEOUT_MS),
                        TAG, "读感应开关失败");
    ESP_RETURN_ON_FALSE(len >= 1, ESP_ERR_INVALID_RESPONSE, TAG, "回复长度异常 %u", len);
    *on = (p[0] != 0);
    return ESP_OK;
}

esp_err_t radar_at6010_set_sensing(bool on)
{
    uint8_t p = on ? 1 : 0;
    return transact(CMD_SET_RADAR_ON, &p, 1, NULL, NULL, REPLY_TIMEOUT_MS);
}

esp_err_t radar_at6010_get_bounds(radar_bounds_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "输出指针为空");
    uint8_t p[16] = { 0 };
    uint8_t len = 0;
    ESP_RETURN_ON_ERROR(transact(CMD_GET_BOUNDS, NULL, 0, p, &len, REPLY_TIMEOUT_MS),
                        TAG, "读边界值失败");
    ESP_RETURN_ON_FALSE(len >= 14, ESP_ERR_INVALID_RESPONSE, TAG, "回复长度异常 %u", len);
    out->mot_min_cm = rd_u16(&p[0]);
    out->mot_max_cm = rd_u16(&p[2]);
    out->micro_min_cm = rd_u16(&p[4]);
    out->micro_max_cm = rd_u16(&p[6]);
    out->bhr_min_cm = rd_u16(&p[8]);
    out->bhr_max_cm = rd_u16(&p[10]);
    out->sweep_bw = rd_u16(&p[12]);
    return ESP_OK;
}

esp_err_t radar_at6010_get_algo_config(radar_algo_config_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "输出指针为空");
    uint8_t p[24] = { 0 };
    uint8_t len = 0;
    ESP_RETURN_ON_ERROR(transact(CMD_GET_ALGO_CONFIG, NULL, 0, p, &len, REPLY_TIMEOUT_MS),
                        TAG, "读算法配置失败");
    ESP_RETURN_ON_FALSE(len >= 18, ESP_ERR_INVALID_RESPONSE, TAG, "回复长度异常 %u", len);
    out->mot_min_cm = rd_u16(&p[0]);
    out->mot_max_cm = rd_u16(&p[2]);
    out->mot_sensitivity = p[4];
    out->micro_min_cm = rd_u16(&p[6]);
    out->micro_max_cm = rd_u16(&p[8]);
    out->micro_sensitivity = p[10];
    out->bhr_min_cm = rd_u16(&p[12]);
    out->bhr_max_cm = rd_u16(&p[14]);
    out->bhr_sensitivity = p[16];
    return ESP_OK;
}

esp_err_t radar_at6010_save_settings(void)
{
    uint8_t p = 1;
    return transact(CMD_SAVE_SETTINGS, &p, 1, NULL, NULL, 1000);
}

esp_err_t radar_at6010_query_detection(radar_report_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "输出指针为空");
    uint8_t p[24] = { 0 };
    uint8_t len = 0;
    ESP_RETURN_ON_ERROR(transact(CMD_GET_DET_INFO, NULL, 0, p, &len, REPLY_TIMEOUT_MS),
                        TAG, "查询检测信息失败");
    ESP_RETURN_ON_FALSE(len >= 20, ESP_ERR_INVALID_RESPONSE, TAG, "回复长度异常 %u", len);
    memset(out, 0, sizeof(*out));
    out->type = RADAR_RPT_FULL;
    out->source = 0;
    out->timestamp_us = esp_timer_get_time();
    parse_det_fields(p, out);
    return ESP_OK;
}

const radar_stats_t *radar_at6010_stats(void) { return &s_stats; }

// ---------------- 翻译与打印 ----------------
const char *radar_at6010_type_name(uint8_t type)
{
    switch (type) {
    case RADAR_RPT_FULL:      return "完整检测";
    case RADAR_RPT_HEIGHT:    return "测高";
    case RADAR_RPT_OCCUPANCY: return "占位检测";
    case RADAR_RPT_MOTION:    return "运动存在";
    case RADAR_RPT_BREATH:    return "呼吸心率";
    case RADAR_RPT_REGION:    return "分区检测";
    default:                  return "未知类型";
    }
}

const char *radar_at6010_det_desc(uint8_t det_result, char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return "";
    }
    buf[0] = '\0';
    struct {
        uint8_t bit;
        const char *name;
    } map[] = {
        { RADAR_DET_APPROACHING, "靠近" },
        { RADAR_DET_RECEDING,    "远离" },
        { RADAR_DET_MOTION,      "运动" },
        { RADAR_DET_MICRO,       "微动" },
        { RADAR_DET_BREATH,      "呼吸" },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (det_result & map[i].bit) {
            if (buf[0] != '\0') {
                strncat(buf, "+", buf_len - strlen(buf) - 1);
            }
            strncat(buf, map[i].name, buf_len - strlen(buf) - 1);
        }
    }
    if (buf[0] == '\0') {
        strncat(buf, "无", buf_len - 1);
    }
    return buf;
}

void radar_report_print(const radar_report_t *r, const char *prefix)
{
    char desc[64];
    radar_at6010_det_desc(r->det_result, desc, sizeof(desc));

    printf("%s[%s] type=%u(%s) detected=%u det=0x%02X(%s) 距离=%u.%03u m 角度=%d° "
           "速度=%d(预留) 置信度(距/角)=%u/%u 帧号=%u\n",
           prefix,
           r->source ? "上报" : "查询",
           (unsigned)r->type, radar_at6010_type_name(r->type),
           (unsigned)r->is_detected, (unsigned)r->det_result, desc,
           (unsigned)(r->range_mm / 1000u), (unsigned)(r->range_mm % 1000u),
           (int)r->angle_deg,
           (int)r->velocity, (unsigned)r->range_confidence, (unsigned)r->angle_confidence,
           (unsigned)r->frame_index);

    if (r->type == RADAR_RPT_HEIGHT) {
        printf("%s    测高: %u mm  状态=%u (0无效/1校准完成/2进入区域/3测量完成)\n",
               prefix, (unsigned)r->height_mm, (unsigned)r->height_status);
    } else if (r->type == RADAR_RPT_BREATH) {
        printf("%s    呼吸=%u  心率=%u\n", prefix,
               (unsigned)r->breath_rate, (unsigned)r->heart_rate);
    } else if (r->type == RADAR_RPT_REGION) {
        printf("%s    分区目标数=%u\n", prefix, (unsigned)r->object_num);
        for (int i = 0; i < 3; i++) {
            if (r->region[i].range_mm || r->region[i].angle_deg) {
                printf("%s      分区%d: 距离=%u mm 角度=%d°\n",
                       prefix, i, (unsigned)r->region[i].range_mm,
                       (int)r->region[i].angle_deg);
            }
        }
    }
}

uart_port_t radar_at6010_get_uart_port(void)
{
    return s_cfg.uart_port;
}

void radar_at6010_flush_input(void)
{
    if (s_ready) {
        uart_flush_input(s_cfg.uart_port);
    }
}
