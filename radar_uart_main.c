// MS60-1211S80M 雷达 —— UART 抓包程序（模拟官方上位机）
//
// 背景：官方上位机 ATRadarSettingTool 在 115200 下能读到模块数据，
//       说明模块的 UART 是活的、波特率是 115200。
//       但之前那版固件的判据有问题：只发版本查询 (0xFE) 等回复，
//       模块不回就判"无应答"——完全可能模块一直在吐数据、
//       只是不响应那条命令。那种情况下旧代码会把数据默默丢掉，
//       连"收到几个字节"都不打印，所以根本无法定位。
//
// 本程序改成三段式，先看有没有字节，再谈解析：
//   阶段 A  纯被动监听，统计字节数并打印十六进制头部
//           —— 这一条就能回答"雷达 TX 到底有没有接到 GPIO21"
//   阶段 B  按 0x5A 主动上报帧协议尝试解析并打印
//   阶段 C  模仿上位机轮询 0x30 查询帧，打印解析结果
//
// 串口输出：UART0 115200 8N1；雷达接 UART1 TX=GPIO14 RX=GPIO21

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/uart.h"

// ---------------- 按需修改 ----------------
#define RADAR_UART_PORT     UART_NUM_1
#define RADAR_UART_TX_GPIO  GPIO_NUM_14   // ESP32 TX -> 雷达 RX
#define RADAR_UART_RX_GPIO  GPIO_NUM_21   // ESP32 RX <- 雷达 TX

// OUT 联动测试用的引脚：发送 0x0A 命令强制驱动模块的 OUT，同时读这个脚
// 用来判断"OUT 那根线到底通不通"。接到别处就改这里。
#define OUT_TEST_GPIO       GPIO_NUM_42

// 上位机实测能连上的波特率排最前
static const int k_bauds[] = { 115200, 921600, 9600 };

#define LISTEN_MS           3000          // 阶段 A 监听时长
#define BUF_SIZE            2048

// ---------------- 工具 ----------------
static void hex_dump(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        printf("%02X ", p[i]);
        if ((i % 16) == 15) {
            printf("\n        ");
        }
    }
    printf("\n");
}

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static int16_t  rd_s16(const uint8_t *p) { return (int16_t)rd_u16(p); }
static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const char *det_desc(uint8_t d)
{
    static char buf[64];
    buf[0] = '\0';
    if (d & 0x01) strcat(buf, "靠近 ");
    if (d & 0x02) strcat(buf, "远离 ");
    if (d & 0x04) strcat(buf, "运动 ");
    if (d & 0x08) strcat(buf, "微动 ");
    if (d & 0x10) strcat(buf, "呼吸 ");
    if (buf[0] == '\0') strcat(buf, "无");
    return buf;
}

// ---------------- 阶段 A：纯被动监听 ----------------
// 只统计字节数，并保留最开始的一段用于打印。不发送任何数据。
static void listen_raw(uint32_t ms, uint8_t *head, size_t head_cap,
                       size_t *head_len, uint32_t *total)
{
    uint8_t tmp[256];
    *head_len = 0;
    *total = 0;
    const int64_t t0 = esp_timer_get_time();

    while ((esp_timer_get_time() - t0) < (int64_t)ms * 1000) {
        int r = uart_read_bytes(RADAR_UART_PORT, tmp, sizeof(tmp), pdMS_TO_TICKS(50));
        if (r <= 0) {
            continue;
        }
        *total += (uint32_t)r;
        if (*head_len < head_cap) {
            size_t c = head_cap - *head_len;
            if (c > (size_t)r) {
                c = (size_t)r;
            }
            memcpy(head + *head_len, tmp, c);
            *head_len += c;
        }
    }
}

// ---------------- 阶段 B：在缓冲区里找 0x5A 主动上报帧 ----------------
static int scan_reports(const uint8_t *buf, size_t n)
{
    int found = 0;
    size_t i = 0;
    while (i + 2 < n) {
        if (buf[i] != 0x5A) {
            i++;
            continue;
        }
        const size_t len = buf[i + 1];
        const size_t total = 2 + len + 1;
        if (i + total > n) {
            break;
        }
        uint8_t sum = 0;
        for (size_t k = 0; k < total - 1; k++) {
            sum = (uint8_t)(sum + buf[i + k]);
        }
        const bool ok = (sum == buf[i + total - 1]);
        printf("    找到 0x5A 帧 @偏移%u  LEN=%u  校验%s\n",
               (unsigned)i, (unsigned)len, ok ? "正确" : "错误");
        if (ok && len >= 1) {
            const uint8_t type = buf[i + 2];
            printf("      TYPE=%u  载荷:", (unsigned)type);
            hex_dump(&buf[i + 2], len);
            // 按 TYPE 0 的布局试解一遍（TYPE 会自动包含在载荷里）
            if (len >= 21) {
                const uint8_t *f = &buf[i + 3];
                printf("      试解: detected=%u det=0x%02X(%s) 距离=%u mm 角度=%d 帧号=%u\n",
                       (unsigned)f[0], (unsigned)f[1], det_desc(f[1]),
                       (unsigned)rd_u16(&f[2]), (int)rd_s16(&f[4]),
                       (unsigned)rd_u32(&f[16]));
            }
            found++;
        }
        i += total;
    }
    return found;
}

// ---------------- 阶段 C：模仿上位机主动发命令 ----------------
// 按 AT6010 协议构造发送帧：58 <CMD> <LEN> <参数...> <校验低> <校验高>
// 校验 = 前面所有字节之和，取 16 位小端。
// 例：0x30 查询 -> 58 30 00 88 00（0x58+0x30+0x00 = 0x88）
static int s_reply_count = 0;

// 发送带 1 字节参数的命令（用于 0x0A 设置 OUT 电平）。
// 返回值 = 实际收到的字节数；只有真收到字节才计入 s_reply_count。
static int send_cmd1(uint8_t cmd, uint8_t para, bool print)
{
    uint8_t frame[6] = { 0x58, cmd, 0x01, para, 0x00, 0x00 };
    const uint16_t sum = (uint16_t)(0x58 + cmd + 0x01 + para);
    frame[4] = (uint8_t)(sum & 0xFF);
    frame[5] = (uint8_t)(sum >> 8);

    if (print) {
        printf("      发送:");
        for (size_t i = 0; i < sizeof(frame); i++) {
            printf(" %02X", frame[i]);
        }
        printf("\n");
    }
    uart_flush_input(RADAR_UART_PORT);
    uart_write_bytes(RADAR_UART_PORT, frame, sizeof(frame));
    uart_wait_tx_done(RADAR_UART_PORT, pdMS_TO_TICKS(100));

    uint8_t buf[64];
    const int r = uart_read_bytes(RADAR_UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(300));
    if (r > 0) {
        s_reply_count++;
        if (print) {
            printf("      收到 %d 字节: ", r);
            hex_dump(buf, (size_t)r);
        }
    } else if (print) {
        printf("      无回复\n");
    }
    return r;
}

// OUT 联动测试：用 0x0A 命令把模块的 OUT 强制拉有效/无效，同时读 ESP32 侧
// 这一步能一刀切开"OUT 线没接"和"模块 OUT 没驱动"
static void out_link_test(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << OUT_TEST_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        printf("  OUT 引脚配置失败，跳过\n\n");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(30));

    // 先用内部下拉 / 上拉探一遍：能区分"引脚悬空"和"有东西在驱动"
    uint32_t hi = 0;
    for (int i = 0; i < 100; i++) {
        if (gpio_get_level(OUT_TEST_GPIO)) {
            hi++;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    const bool pd_high = (hi > 50);
    gpio_set_pull_mode(OUT_TEST_GPIO, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(30));
    hi = 0;
    for (int i = 0; i < 100; i++) {
        if (gpio_get_level(OUT_TEST_GPIO)) {
            hi++;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    const bool pu_high = (hi > 50);

    printf("  【OUT 联动测试】GPIO%d\n", (int)OUT_TEST_GPIO);
    printf("    内部下拉读到「%s」，内部上拉读到「%s」\n",
           pd_high ? "高" : "低", pu_high ? "高" : "低");
    if (!pd_high && pu_high) {
        printf("    => **GPIO%d 悬空**：模块的 OUT 没接到这个脚，或没共地。\n",
               (int)OUT_TEST_GPIO);
        printf("       先解决接线，下面用 0x0A 命令做的测试才有意义。\n\n");
        return;
    }
    printf("    => 有外部驱动（接上了）。下面用 0x0A 命令驱动模块 OUT 看电平变不变。\n");
    const int before = gpio_get_level(OUT_TEST_GPIO);

    send_cmd1(0x0A, 0x01, true);          // 置 OUT 有效
    vTaskDelay(pdMS_TO_TICKS(300));
    const int lv_on = gpio_get_level(OUT_TEST_GPIO);

    send_cmd1(0x0A, 0x00, true);          // 置 OUT 无效
    vTaskDelay(pdMS_TO_TICKS(300));
    const int lv_off = gpio_get_level(OUT_TEST_GPIO);

    printf("    GPIO%d 电平：测试前 %d -> 置有效后 %d -> 置无效后 %d\n",
           (int)OUT_TEST_GPIO, before, lv_on, lv_off);
    if (lv_on != lv_off) {
        printf("    => **OUT 接线是通的！** 电平跟着命令变了\n");
    } else {
        printf("    => OUT 没跟着变。两种可能：\n");
        printf("       (a) 模块的 OUT 没接到 GPIO%d（或者接到别的脚）\n", (int)OUT_TEST_GPIO);
        printf("       (b) 模块和 ESP32 没共地\n");
    }
    printf("\n");
}

static void poll_cmd(uint8_t cmd, const char *label)
{
    uint8_t frame[5] = { 0x58, cmd, 0x00, 0x00, 0x00 };
    const uint16_t sum = (uint16_t)(0x58 + cmd + 0x00);
    frame[3] = (uint8_t)(sum & 0xFF);
    frame[4] = (uint8_t)(sum >> 8);
    uint8_t buf[256];

    printf("    %s  发送:", label);
    for (size_t i = 0; i < sizeof(frame); i++) {
        printf(" %02X", frame[i]);
    }
    printf("\n");

    uart_flush_input(RADAR_UART_PORT);
    uart_write_bytes(RADAR_UART_PORT, frame, sizeof(frame));
    uart_wait_tx_done(RADAR_UART_PORT, pdMS_TO_TICKS(100));

    int r = uart_read_bytes(RADAR_UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(400));
    if (r <= 0) {
        printf("      -> 无回复\n");
        return;
    }
    printf("      -> 收到 %d 字节:\n           ", r);
    s_reply_count++;
    hex_dump(buf, (size_t)r);
    if (r >= 3 && buf[0] == 0x59 && buf[1] == 0x30) {
        const uint8_t len = buf[2];
        printf("      解析为 0x59 回复，载荷 %u 字节\n", (unsigned)len);
        if (len >= 20 && r >= 5 + len) {
            const uint8_t *f = &buf[3];
            printf("        detected=%u det=0x%02X(%s) 距离=%u mm 角度=%d "
                   "速度=%d 置信度=%u/%u 帧号=%u\n",
                   (unsigned)f[0], (unsigned)f[1], det_desc(f[1]),
                   (unsigned)rd_u16(&f[2]), (int)rd_s16(&f[4]), (int)rd_s16(&f[6]),
                   (unsigned)f[14], (unsigned)f[15], (unsigned)rd_u32(&f[16]));
        }
    }
}

// ---------------- 主流程 ----------------
// ---------------- 阶段 0：RX 线自检 ----------------
// 回答一个很具体的问题：雷达的 TX 到底有没有接到 ESP32 的 RX 脚上？
// 手法和测 OUT 一样：用一个很弱的内部上拉 / 下拉去"探"这个脚。
//   下拉读高 + 上拉读高 -> 有外部器件在驱动高（接上了，UART 空闲态就是这个）
//   下拉读低 + 上拉读低 -> 有外部器件在驱动低
//   下拉读低 + 上拉读高 -> 悬空，什么都没接
//
// 注意：必须在 uart_driver_install / uart_set_pin 之前调用，
//       因为一旦交给 UART 驱动，这个脚就不再是普通 GPIO 了。
static void rx_line_check(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << RADAR_UART_RX_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        printf("  GPIO 配置失败，跳过\n\n");
        return;
    }

    uint32_t hi = 0;
    vTaskDelay(pdMS_TO_TICKS(30));
    for (int i = 0; i < 200; i++) {
        if (gpio_get_level(RADAR_UART_RX_GPIO)) {
            hi++;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    const bool pd_high = (hi > 100);

    gpio_set_pull_mode(RADAR_UART_RX_GPIO, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(30));
    hi = 0;
    for (int i = 0; i < 200; i++) {
        if (gpio_get_level(RADAR_UART_RX_GPIO)) {
            hi++;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    const bool pu_high = (hi > 100);

    printf("  内部下拉读到「%s」，内部上拉读到「%s」\n",
           pd_high ? "高" : "低", pu_high ? "高" : "低");
    if (pd_high && pu_high) {
        printf("  判定：外部在驱动「高」-- 雷达 TX 接上了（UART 空闲高，正常）\n");
    } else if (!pd_high && !pu_high) {
        printf("  判定：外部在驱动「低」-- 接上了，但一直拉低\n");
    } else if (!pd_high && pu_high) {
        printf("  判定：**引脚悬空** -- 雷达的 TX 没接到 GPIO%d！\n",
               (int)RADAR_UART_RX_GPIO);
        printf("        或者：雷达的 GND 和 ESP32 的 GND 没有接在一起（没共地）\n");
    } else {
        printf("  判定：读数异常（下拉时高、上拉时低）\n");
    }
    printf("\n");
}

// ---------------- 主流程 ----------------
static void banner(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    printf("\n");
    printf("==========================================================\n");
    printf("  MS60-1211S80M 雷达 —— UART 抓包程序（模拟上位机）\n");
    printf("==========================================================\n");
    printf("  芯片      : %s  核心 %d  IDF %s\n",
           CONFIG_IDF_TARGET, info.cores, esp_get_idf_version());
    printf("  雷达串口  : UART%d  ESP32 TX=GPIO%d -> 雷达 RX\n",
           (int)RADAR_UART_PORT, (int)RADAR_UART_TX_GPIO);
    printf("                       ESP32 RX=GPIO%d <- 雷达 TX\n", (int)RADAR_UART_RX_GPIO);
    printf("  供电      : 雷达 VCC 接 3V3，GND 共地\n");
    printf("  上位机实测: 115200 可以连上，所以这里优先试 115200\n");
    printf("----------------------------------------------------------\n\n");
}

static bool uart_setup(int baud)
{
    uart_config_t cfg = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_param_config(RADAR_UART_PORT, &cfg) != ESP_OK) {
        return false;
    }
    return (uart_set_baudrate(RADAR_UART_PORT, baud) == ESP_OK);
}

void app_main(void)
{
    banner();

    printf("[阶段 0] RX 线自检：雷达的 TX 到底有没有接到 ESP32 的 GPIO%d ...\n",
           (int)RADAR_UART_RX_GPIO);
    rx_line_check();

    printf("[初始化] 安装 UART%d 驱动 ...\n", (int)RADAR_UART_PORT);
    esp_err_t err = uart_driver_install(RADAR_UART_PORT, 8192, 1024, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("  失败: %s\n", esp_err_to_name(err));
        return;
    }
    if (uart_set_pin(RADAR_UART_PORT, RADAR_UART_TX_GPIO, RADAR_UART_RX_GPIO,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        printf("  引脚绑定失败\n");
        return;
    }
    printf("  就绪\n\n");

    static uint8_t head[BUF_SIZE];
    size_t head_len = 0;
    uint32_t total = 0;
    int working_baud = 0;

    // ---------- 阶段 A ----------
    for (size_t b = 0; b < sizeof(k_bauds) / sizeof(k_bauds[0]); b++) {
        printf("[阶段 A] 波特率 %d：纯被动监听 %d ms（不发任何命令）...\n",
               k_bauds[b], LISTEN_MS);
        if (!uart_setup(k_bauds[b])) {
            printf("  本机设置失败\n\n");
            continue;
        }
        uart_flush_input(RADAR_UART_PORT);
        listen_raw(LISTEN_MS, head, sizeof(head), &head_len, &total);
        printf("  收到 %u 字节", (unsigned)total);
        if (total == 0) {
            printf("  <-- 一个字节都没有\n\n");
            continue;
        }
        printf("\n  头部 %u 字节:\n        ", (unsigned)head_len);
        hex_dump(head, head_len);
        printf("\n");
        working_baud = k_bauds[b];
        break;
    }

    if (working_baud == 0) {
        printf("==========================================================\n");
        printf("  被动监听阶段没收到任何字节。\n");
        printf("  但**这不一定是故障**：这颗模块是从机，默认不主动说话，\n");
        printf("  只有收到命令才会回话。你用逻辑分析仪测到的波形，\n");
        printf("  也是在上位机点了\"连接\"之后才出现的。\n");
        printf("  所以继续往下走 —— 主动发命令试试（这才是关键的一步）。\n");
        printf("==========================================================\n");
        printf("\n");
        working_baud = 115200;   // 按上位机实测能连上的值继续
    }

    // ---------- 阶段 B ----------
    printf("[阶段 B] 在已收到的字节里找 0x5A 主动上报帧 ...\n");
    int n = 0;
    if (total > 0) {
        n = scan_reports(head, head_len);
    } else {
        printf("  阶段 A 没收到字节，无内容可解析（正常，见上）\n");
    }
    printf("  共找到 %d 个 0x5A 帧\n\n", n);

    // ---------- 阶段 C ----------
    // 命令字来自对官方上位机 lib\airhost.dll 的反汇编结果：
    //   0x30 读取检测信息（上位机"连接"后就是靠它拿数据流）
    //   0x31 算法类型  0x32 算法边界值  0x33 感应配置  0xD0 雷达开关
    // 注意：0xFE（版本查询）不在上位机的命令表里，模块很可能不支持，已移除。
    printf("[阶段 C] 模仿上位机，主动发命令（这是最关键的一步）...\n");
    int good_baud = 0;
    for (size_t b = 0; b < sizeof(k_bauds) / sizeof(k_bauds[0]); b++) {
        // 注意：uart_setup 返回 bool，不是 esp_err_t。写成 != ESP_OK 会永远成立，
        // 整个循环体被 continue 跳过——上一版阶段 C 一行都没打印就是这个原因。
        if (!uart_setup(k_bauds[b])) {
            continue;
        }
        printf("  波特率 %d：\n", k_bauds[b]);
        const int before = s_reply_count;
        poll_cmd(0x30, "0x30 读取检测信息");
        vTaskDelay(pdMS_TO_TICKS(300));
        poll_cmd(0x33, "0x33 读取感应配置");
        vTaskDelay(pdMS_TO_TICKS(300));
        poll_cmd(0x32, "0x32 读取算法边界值");
        vTaskDelay(pdMS_TO_TICKS(300));
        poll_cmd(0x31, "0x31 读取算法类型");
        vTaskDelay(pdMS_TO_TICKS(300));
        poll_cmd(0xD0, "0xD0 读取雷达开关");
        vTaskDelay(pdMS_TO_TICKS(300));
        if (s_reply_count > before && good_baud == 0) {
            good_baud = k_bauds[b];
        }
    }

    // ---------- 阶段 D：OUT 联动测试 ----------
    printf("\n[阶段 D] OUT 引脚联动测试（用 0x0A 命令驱动模块 OUT）...\n");
    if (good_baud != 0) {
        uart_setup(good_baud);
    }
    out_link_test();

    printf("\n结论提示：\n");
    if (s_reply_count > 0) {
        printf("  * 收到了 %d 次回复 -> 通信通了！上面能看到原始字节和解析结果。\n",
               s_reply_count);
    } else if (n > 0) {
        printf("  * 只收到 0x5A 主动上报、命令无回复 -> 模块在推数据但不理命令。\n");
    } else {
        printf("  * 主动发命令也完全没回复。这时要区分两种情况：\n");
        printf("      (a) 雷达的 TX 没接到 GPIO%d —— 用逻辑分析仪直接测模块的\n",
               (int)RADAR_UART_RX_GPIO);
        printf("          TX 脚，上位机连着的时候应该能看到波形。看不到就是模块没发。\n");
        printf("      (b) 接了但电平/共地有问题 —— 量一下雷达 GND 和 ESP32 GND 通不通。\n");
        printf("    用逻辑分析仪同时抓模块 TX 和 RX，把两路解码出的十六进制发我，\n");
        printf("    那就是协议本体，我按它改驱动。\n");
    }
}
