/*
 * HW-991 (Bosch BMI270) 驱动实现 —— ESP-IDF
 *
 * 寄存器地址与初始化时序全部取自博世官方 BMI270 SensorAPI：
 *   bmi2_defs.h  —— 寄存器地址与位域定义
 *   bmi2.c       —— bmi2_soft_reset() / write_config_file() / upload_file()
 *   bmi270.c     —— bmi270_init()
 * 关键常量在下面都标注了官方来源，未做任何猜测。
 * 特别注意：INIT_DATA 寄存器是 0x5E，不是 0x5D。
 */

#include "imu_bmi270.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "imu_bmi270";

/* ---------------- 寄存器地址（来源：bmi2_defs.h） ---------------- */
#define REG_CHIP_ID          0x00u
#define REG_ACC_DATA         0x0Cu   /* BMI2_ACC_X_LSB_ADDR，连续 6 字节 */
#define REG_GYR_DATA         0x12u   /* BMI2_GYR_X_LSB_ADDR，连续 6 字节 */
#define REG_INTERNAL_STATUS  0x21u   /* BMI2_INTERNAL_STATUS_ADDR */
#define REG_TEMPERATURE_0    0x22u   /* BMI2_TEMPERATURE_0_ADDR，2 字节 */
#define REG_ACC_CONF         0x40u   /* BMI2_ACC_CONF_ADDR */
#define REG_ACC_RANGE        0x41u   /* 与 ACC_CONF 相邻，可一次写 2 字节 */
#define REG_GYR_CONF         0x42u   /* BMI2_GYR_CONF_ADDR */
#define REG_GYR_RANGE        0x43u   /* 与 GYR_CONF 相邻，可一次写 2 字节 */
#define REG_INIT_CTRL        0x59u   /* BMI2_INIT_CTRL_ADDR */
#define REG_INIT_ADDR_0      0x5Bu   /* BMI2_INIT_ADDR_0，连写 2 字节覆盖 0x5B/0x5C */
#define REG_INIT_DATA        0x5Eu   /* BMI2_INIT_DATA_ADDR（注意是 0x5E） */
#define REG_PWR_CONF         0x7Cu   /* BMI2_PWR_CONF_ADDR */
#define REG_PWR_CTRL         0x7Du   /* BMI2_PWR_CTRL_ADDR */
#define REG_CMD              0x7Eu   /* BMI2_CMD_REG_ADDR */

/* ---------------- 命令与位域（来源：bmi2_defs.h） ---------------- */
#define CMD_SOFT_RESET              0xB6u  /* BMI2_SOFT_RESET_CMD */
#define PWR_CONF_APS_EN             0x01u  /* bit0: 高级省电模式使能 */
#define PWR_CTRL_GYR_EN             0x02u  /* BMI2_GYR_EN_MASK */
#define PWR_CTRL_ACC_EN             0x04u  /* BMI2_ACC_EN_MASK */
#define PWR_CTRL_TEMP_EN            0x08u  /* BMI2_TEMP_EN_MASK */
#define INIT_CTRL_LOAD_EN           0x01u  /* BMI2_CONF_LOAD_EN_MASK */
#define STATUS_MSG_MASK             0x0Fu  /* BMI2_CONFIG_LOAD_STATUS_MASK */
#define STATUS_MSG_CONFIG_OK        0x01u  /* BMI2_CONFIG_LOAD_SUCCESS */

/* ODR 与带宽（来源：bmi2_defs.h）
 * ACC_CONF(0x40): bit7=滤波性能模式, bits[6:4]=带宽, bits[3:0]=ODR
 *   BMI2_ACC_ODR_100HZ = 0x08，BMI2_ACC_NORMAL_AVG4 = 0x02
 * GYR_CONF(0x42): bit7=滤波性能模式, bits[5:4]=带宽, bits[3:0]=ODR
 *   BMI2_GYR_ODR_100HZ = 0x08，BMI2_GYR_NORMAL_MODE = 0x02
 */
#define ACC_CONF_100HZ_HIPERF  (0x80u | (0x02u << 4) | 0x08u)   /* 0xA8 */
#define GYR_CONF_100HZ_HIPERF  (0x80u | (0x02u << 4) | 0x08u)   /* 0xA8 */

/* 配置块按 32 字节分块上传（来源：bmi2.c 中 read_write_len 的用法） */
#define CONFIG_UPLOAD_CHUNK   32u

extern const uint8_t  bmi270_config_file[];
extern const uint32_t bmi270_config_file_len;

static i2c_master_bus_handle_t s_bus   = NULL;
static i2c_master_dev_handle_t s_dev   = NULL;
static imu_bmi270_config_t     s_cfg;
static uint8_t                 s_addr  = 0;
static bool                    s_ready = false;

/* ---------------- 基础寄存器读写 ---------------- */
static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, IMU_BMI270_I2C_TIMEOUT_MS);
}

static esp_err_t regs_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, IMU_BMI270_I2C_TIMEOUT_MS);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), IMU_BMI270_I2C_TIMEOUT_MS);
}

static esp_err_t regs_write(uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t tmp[1 + CONFIG_UPLOAD_CHUNK];
    if (len + 1 > sizeof(tmp)) {
        return ESP_ERR_INVALID_SIZE;
    }
    tmp[0] = reg;
    memcpy(&tmp[1], buf, len);
    return i2c_master_transmit(s_dev, tmp, len + 1, IMU_BMI270_I2C_TIMEOUT_MS);
}

/* 读-改-写：只改指定掩码的位 */
static esp_err_t reg_update(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t v = 0;
    ESP_RETURN_ON_ERROR(reg_read(reg, &v), TAG, "读寄存器 0x%02X 失败", reg);
    v = (uint8_t)((v & ~mask) | (value & mask));
    return reg_write(reg, v);
}

/* ---------------- 量程换算 ---------------- */
float imu_bmi270_acc_lsb_per_g(imu_acc_range_t r)
{
    switch (r) {
    case IMU_ACC_RANGE_2G:  return 16384.0f;
    case IMU_ACC_RANGE_4G:  return 8192.0f;
    case IMU_ACC_RANGE_8G:  return 4096.0f;
    case IMU_ACC_RANGE_16G: return 2048.0f;
    default:                return 8192.0f;
    }
}

float imu_bmi270_gyr_lsb_per_dps(imu_gyr_range_t r)
{
    switch (r) {
    case IMU_GYR_RANGE_2000DPS: return 16.384f;
    case IMU_GYR_RANGE_1000DPS: return 32.768f;
    case IMU_GYR_RANGE_500DPS:  return 65.536f;
    case IMU_GYR_RANGE_250DPS:  return 131.072f;
    case IMU_GYR_RANGE_125DPS:  return 262.144f;
    default:                    return 32.768f;
    }
}

const char *imu_bmi270_acc_range_name(imu_acc_range_t r)
{
    switch (r) {
    case IMU_ACC_RANGE_2G:  return "+-2g";
    case IMU_ACC_RANGE_4G:  return "+-4g";
    case IMU_ACC_RANGE_8G:  return "+-8g";
    case IMU_ACC_RANGE_16G: return "+-16g";
    default:                return "unknown";
    }
}

const char *imu_bmi270_gyr_range_name(imu_gyr_range_t r)
{
    switch (r) {
    case IMU_GYR_RANGE_2000DPS: return "+-2000dps";
    case IMU_GYR_RANGE_1000DPS: return "+-1000dps";
    case IMU_GYR_RANGE_500DPS:  return "+-500dps";
    case IMU_GYR_RANGE_250DPS:  return "+-250dps";
    case IMU_GYR_RANGE_125DPS:  return "+-125dps";
    default:                    return "unknown";
    }
}

uint8_t imu_bmi270_address(void) { return s_addr; }

const imu_bmi270_config_t *imu_bmi270_active_config(void) { return &s_cfg; }

/* ---------------- 总线扫描 ---------------- */
esp_err_t imu_bmi270_scan_bus(const imu_bmi270_config_t *cfg,
                              uint8_t *found, size_t max, size_t *count)
{
    i2c_master_bus_handle_t bus = NULL;
    size_t n = 0;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = cfg->i2c_port,
        .sda_io_num = cfg->sda_gpio,
        .scl_io_num = cfg->scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "建立 I2C 总线失败");

    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (i2c_master_probe(bus, a, 50) == ESP_OK) {
            if (found && n < max) {
                found[n] = a;
            }
            n++;
        }
    }
    if (count) {
        *count = n;
    }

    i2c_del_master_bus(bus);
    return ESP_OK;
}

/* ---------------- 灌配置块（时序逐行对应 bmi2.c） ---------------- */
static esp_err_t upload_config_blob(void)
{
    /* 1. 关掉高级省电模式（bmi2_set_adv_power_save(DISABLE)） */
    ESP_RETURN_ON_ERROR(reg_update(REG_PWR_CONF, PWR_CONF_APS_EN, 0), TAG, "关闭省电失败");

    /* 2. 关闭配置加载（set_config_load(DISABLE)） */
    ESP_RETURN_ON_ERROR(reg_update(REG_INIT_CTRL, INIT_CTRL_LOAD_EN, 0), TAG, "关闭配置加载失败");

    /* 3. 按 32 字节分块上传 */
    for (uint32_t index = 0; index < bmi270_config_file_len; index += CONFIG_UPLOAD_CHUNK) {
        uint16_t word = (uint16_t)(index / 2);   /* 官方注释：地址以 16 位为单位 */
        uint8_t addr[2] = {
            (uint8_t)(word & 0x0F),
            (uint8_t)((word >> 4) & 0xFF),
        };
        ESP_RETURN_ON_ERROR(regs_write(REG_INIT_ADDR_0, addr, sizeof(addr)), TAG,
                            "写配置地址失败 index=%u", (unsigned)index);
        ESP_RETURN_ON_ERROR(regs_write(REG_INIT_DATA, &bmi270_config_file[index],
                                       CONFIG_UPLOAD_CHUNK), TAG,
                            "写配置数据失败 index=%u", (unsigned)index);
    }

    /* 4. 打开配置加载（set_config_load(ENABLE)） */
    ESP_RETURN_ON_ERROR(reg_update(REG_INIT_CTRL, INIT_CTRL_LOAD_EN, INIT_CTRL_LOAD_EN), TAG,
                        "打开配置加载失败");

    /* 5. 重新打开高级省电模式 */
    ESP_RETURN_ON_ERROR(reg_update(REG_PWR_CONF, PWR_CONF_APS_EN, PWR_CONF_APS_EN), TAG,
                        "恢复省电失败");

    /* 6. 轮询等待配置加载完成（芯片内部需要时间校验，不能立刻读取） */
    uint8_t status = 0;
    int retry;
    for (retry = 0; retry < 50; retry++) {
        ESP_RETURN_ON_ERROR(reg_read(REG_INTERNAL_STATUS, &status), TAG, "读状态失败");
        if ((status & STATUS_MSG_MASK) == STATUS_MSG_CONFIG_OK) break;
        if ((status & STATUS_MSG_MASK) != 0x00) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_LOGI(TAG, "配置加载状态 INTERNAL_STATUS=0x%02X（低4位应为 0x1），轮询 %d 次",
             status, retry + 1);
    if ((status & STATUS_MSG_MASK) != STATUS_MSG_CONFIG_OK) {
        ESP_LOGE(TAG, "配置块加载失败：状态 0x%02X != 0x%02X",
                 (unsigned)(status & STATUS_MSG_MASK), (unsigned)STATUS_MSG_CONFIG_OK);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---------------- 初始化 ---------------- */
esp_err_t imu_bmi270_init(const imu_bmi270_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "配置为空");
    ESP_RETURN_ON_FALSE(s_dev == NULL, ESP_ERR_INVALID_STATE, TAG, "已初始化，勿重复调用");

    s_cfg = *cfg;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = cfg->i2c_port,
        .sda_io_num = cfg->sda_gpio,
        .scl_io_num = cfg->scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "建立 I2C 总线失败");

    /* 确定从机地址 */
    uint8_t addr = cfg->device_address;
    if (addr == 0) {
        if (i2c_master_probe(s_bus, IMU_BMI270_ADDR_PRIMARY, 100) == ESP_OK) {
            addr = IMU_BMI270_ADDR_PRIMARY;
        } else if (i2c_master_probe(s_bus, IMU_BMI270_ADDR_SECONDARY, 100) == ESP_OK) {
            addr = IMU_BMI270_ADDR_SECONDARY;
        } else {
            ESP_LOGE(TAG, "0x68 与 0x69 都无应答：检查接线、供电与上拉电阻");
            i2c_del_master_bus(s_bus);
            s_bus = NULL;
            return ESP_ERR_NOT_FOUND;
        }
    }
    s_addr = addr;
    ESP_LOGI(TAG, "从机地址 0x%02X", addr);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = cfg->scl_speed_hz,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), TAG, "挂载从机失败");

    /* 读芯片 ID 确认身份 */
    uint8_t chip_id = 0;
    ESP_RETURN_ON_ERROR(reg_read(REG_CHIP_ID, &chip_id), TAG, "读芯片 ID 失败");
    ESP_LOGI(TAG, "芯片 ID = 0x%02X（BMI270 应为 0x%02X）", chip_id, IMU_BMI270_CHIP_ID);
    if (chip_id != IMU_BMI270_CHIP_ID) {
        ESP_LOGE(TAG, "芯片 ID 不匹配，可能不是 BMI270");
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* 软复位（对应 bmi2_soft_reset：写 0xB6 后等 2ms） */
    ESP_LOGI(TAG, "软复位 ...");
    ESP_RETURN_ON_ERROR(reg_write(REG_CMD, CMD_SOFT_RESET), TAG, "软复位失败");
    esp_rom_delay_us(2000);

    /* 灌配置块 */
    ESP_LOGI(TAG, "上传配置块 %u 字节 ...", (unsigned)bmi270_config_file_len);
    ESP_RETURN_ON_ERROR(upload_config_blob(), TAG, "配置块上传失败");

    /* 配置加速度计：ACC_CONF(0x40) 与 ACC_RANGE(0x41) 一次写 2 字节 */
    uint8_t acc_cfg[2] = { ACC_CONF_100HZ_HIPERF, (uint8_t)cfg->acc_range };
    ESP_RETURN_ON_ERROR(regs_write(REG_ACC_CONF, acc_cfg, sizeof(acc_cfg)), TAG,
                        "配置加速度计失败");

    /* 配置陀螺仪：GYR_CONF(0x42) 与 GYR_RANGE(0x43) 一次写 2 字节 */
    uint8_t gyr_cfg[2] = { GYR_CONF_100HZ_HIPERF, (uint8_t)cfg->gyr_range };
    ESP_RETURN_ON_ERROR(regs_write(REG_GYR_CONF, gyr_cfg, sizeof(gyr_cfg)), TAG,
                        "配置陀螺仪失败");

    /* 使能加速度计 + 陀螺仪 + 温度（PWR_CTRL） */
    ESP_RETURN_ON_ERROR(reg_write(REG_PWR_CTRL,
                                  PWR_CTRL_ACC_EN | PWR_CTRL_GYR_EN | PWR_CTRL_TEMP_EN),
                        TAG, "使能传感器失败");

    /* 等第一组数据就绪 */
    vTaskDelay(pdMS_TO_TICKS(20));

    s_ready = true;
    ESP_LOGI(TAG, "初始化完成");
    return ESP_OK;
}

/* ---------------- 读取一次数据 ---------------- */
esp_err_t imu_bmi270_read(imu_bmi270_sample_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "输出指针为空");
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "尚未初始化");

    uint8_t raw[12] = { 0 };
    uint8_t temp[2] = { 0 };

    /* 加速度(0x0C~0x11) 与陀螺(0x12~0x17) 地址连续，一次读完 */
    ESP_RETURN_ON_ERROR(regs_read(REG_ACC_DATA, raw, sizeof(raw)), TAG, "读加速度/陀螺失败");
    ESP_RETURN_ON_ERROR(regs_read(REG_TEMPERATURE_0, temp, sizeof(temp)), TAG, "读温度失败");

    for (int i = 0; i < 3; i++) {
        out->accel_raw[i] = (int16_t)((uint16_t)raw[i * 2] | ((uint16_t)raw[i * 2 + 1] << 8));
        out->gyro_raw[i]  = (int16_t)((uint16_t)raw[6 + i * 2] |
                                      ((uint16_t)raw[6 + i * 2 + 1] << 8));
    }
    out->temp_raw = (int16_t)((uint16_t)temp[0] | ((uint16_t)temp[1] << 8));

    const float acc_lsb = imu_bmi270_acc_lsb_per_g(s_cfg.acc_range);
    const float gyr_lsb = imu_bmi270_gyr_lsb_per_dps(s_cfg.gyr_range);

    for (int i = 0; i < 3; i++) {
        out->accel_g[i]  = (float)out->accel_raw[i] / acc_lsb;
        out->gyro_dps[i] = (float)out->gyro_raw[i] / gyr_lsb;
    }
    out->accel_norm_g = sqrtf(out->accel_g[0] * out->accel_g[0] +
                              out->accel_g[1] * out->accel_g[1] +
                              out->accel_g[2] * out->accel_g[2]);
    out->gyro_norm_dps = sqrtf(out->gyro_dps[0] * out->gyro_dps[0] +
                               out->gyro_dps[1] * out->gyro_dps[1] +
                               out->gyro_dps[2] * out->gyro_dps[2]);

    /* 换算公式来自 BMI270 数据手册温度寄存器说明：T = raw/512 + 23 */
    out->temperature_c = ((float)out->temp_raw / 512.0f) + 23.0f;
    out->timestamp_us = esp_timer_get_time();
    return ESP_OK;
}

/* ---------------- 寄存器快照 ---------------- */
esp_err_t imu_bmi270_dump_registers(void)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "尚未初始化");

    for (uint8_t base = 0x00; base < 0x80; base += 16) {
        uint8_t buf[16] = { 0 };
        ESP_RETURN_ON_ERROR(regs_read(base, buf, sizeof(buf)), TAG, "读 0x%02X 失败", base);
        printf("    0x%02X:", base);
        for (int i = 0; i < 16; i++) {
            printf(" %02X", buf[i]);
        }
        printf("\n");
    }
    return ESP_OK;
}