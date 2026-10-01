# MS60-1211S80M / AT6010 串口命令表

**来源**：从官方上位机 `ATRadarSettingTool V1.2` 的 `lib\airhost.dll`（52 KB，x86-64）
导出表中提取。方法：解析 PE 导出表定位函数地址，再用 Capstone 反汇编，提取函数开头
写入命令寄存器的立即数。

**帧格式**（已从反汇编中确认）：

```
发送帧：[0x58][CMD][LEN][参数...][校验低][校验高]
        帧头固定 0x58；校验 = 前面所有字节之和，16 位小端
```

反汇编里 16 位立即数 `0x1358` 的写法就是"帧头 0x58 + 命令 0x13"打包成一个字，
和协议文档的示例完全一致。

---

## 一、基础命令

| 命令字 | 上位机函数 | 功能 | 协议文档对应 |
|---|---|---|---|
| 0x00 | `hci_reg_w` / `reg_write` | 寄存器写 | 3.5.1 Register Write |
| 0x01 | `hci_reg_r` / `reg_read` | 寄存器读 | 3.5.2 Register Read |
| 0x02 | `hci_sensing_dist_set` | 设置雷达感应等级 | 3.2.4 |
| 0x03 | `hci_sensing_dist_get` | 获取雷达感应等级 | 3.2.5 |
| 0x04 | （未单独导出） | 设置感应电平持续时间 | 3.1.1 |
| 0x05 | `hci_light_on_time_get` | 获取感应电平持续时间 | 3.1.2 |
| 0x06 | （未单独导出） | 设置光敏阈值 | 3.1.3 |
| 0x07 | `hci_lux_thr_get` | 获取光敏阈值 | 3.1.4 |
| 0x08 | `hci_radar_set_save` | **保存设置到 Flash** | 3.1.5 |
| 0x09 | （未单独导出） | 获取保存状态 | 3.1.6 |
| 0x0A | `hci_light_set` | **设置 OUT 引脚电平**（上位机的 light on/off 按钮） | 3.1.7 |
| 0x0B | （未单独导出） | 设置 PWM 占空比 | 3.1.8 |
| 0x0C | `ci_bhr_dist_set` | 呼吸检测距离设置 | 3.2.15 / 3.2.16 |
| 0x0D | `ci_bhr_dist_get` | 呼吸检测距离读取 | — |
| 0x0E | `ci_micro_dist_set` | 微动检测距离设置 | 3.2.12 / 3.2.13 |
| 0x0F | `ci_micro_dist_get` | 微动检测距离读取 | — |
| 0x10 | `hci_mem_w` | Memory 写 | 3.5.3 |
| 0x11 | `hci_mem_r` / `mem_read` | Memory 读 | 3.5.4 |
| 0x13 | `hci_sys_reset` | **系统复位** | 3.1.9 |
| 0x14 | `hci_flash_w` | Flash 写 | 3.5.5 |
| 0x15 | `i2c_reg_write` | I2C 寄存器写（外挂传感器） | — |
| 0x19 | `htol_ldov_measure_get` | 波特率切换 / 电压测量 | 3.1.11 |

## 二、雷达数据与配置（最常用）

| 命令字 | 上位机函数 | 功能 |
|---|---|---|
| **0x30** | **`at5820_fmcw_log_req`**（别名 `fmcw_fusion_out_get_req`） | **请求/读取雷达检测信息** ← 上位机"连接"后就是靠它拿数据 |
| 0x31 | `at5820_alg_type_query` | 获取当前算法类型 |
| 0x32 | `at5820_fmcw_boundary_get` | 获取算法边界值 |
| 0x33 | `at5820_fmcw_cfg_query` | 获取算法感应配置 |
| 0x34 | `at5820_instmov_min_dist_set` | 设置运动检测最近距离 |
| 0x35 | `at5820_instmov_sstv_set` | 设置运动检测灵敏度 |
| 0x36–0x3B | `at5820_tmintgmov_*` / `at5820_vitsgmov_*` | 微动 / 呼吸检测的距离与灵敏度 |
| 0x3C | `fmcw_instant_pwr_get_req` | 瞬时功率请求（上位机波形用） |
| 0xD0 | （未单独导出） | 获取雷达感应开关 |
| **0xD1** | `hci_radar_onoff_set` | **雷达感应功能开关** |
| 0xD2 | `at5820_instmov_max_dist_set` | 设置运动检测最远距离 |
| 0x40 | `spi_baud_scaler_set` | SPI 波特率分频 |

## 三、固件下载命令（配合 AT_Download_Tool，日常调参用不到）

`0x20` 复位 → `0x21` 准备下载 → `0x22` 擦除 Flash → `0x23` 烧写 →
`0x24` 分包发送 → `0x25` 发送结束 → `0x26` 校验 → `0x27` 运行

---

## 四、关键结论

1. **协议与我们实现的一致**：帧头 0x58、0x30 查询、0xD0/0xD1 开关、0x32/0x33 读配置，
   全部与《AT6010 SOC HCI Protocol》文档吻合。

2. **`0xFE`（版本查询）不在上位机的命令表里。** 我们最初那版固件发的就是 0xFE——
   **模块很可能根本不支持这条命令**，所以"无应答"是正常的，不能作为接线故障的证据。

3. **模块是从机**：默认不发数据。上位机点"连接"后靠 `0x30` 主动轮询才拿到数据流。
   被动监听一条本来就安静的线，收到 0 字节是预期结果。

4. 上位机里"连灯开关"（light on/off）走的是 **0x0A**——直接控制 OUT 引脚电平，
   调试 OUT 时可以用它验证。
