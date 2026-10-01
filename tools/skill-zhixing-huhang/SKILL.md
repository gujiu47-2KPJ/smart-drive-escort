---
name: zhixing-huhang
description: 智行护航项目专属开发规则与硬件事实——果园小型农机防撞头盔（ESP32-S3 + FreeRTOS + 毫米波雷达 + IMU + 摄像头多模态融合）。Use when developing, debugging, or reviewing the zhixing-huhang firmware, drivers, FreeRTOS tasks, or sensor-fusion/alarm logic under G:\codex-workspace. NOT for unrelated ESP32 projects or generic embedded questions.
---

# 智行护航（Zhixing Huhang）

果园小型农机防撞预警头盔。ESP32-S3 + FreeRTOS，端侧多模态融合：毫米波雷达 + IMU + 摄像头，做预测式主动安全。

本文是项目专属的「非显然事实」补充，不重复 `AGENTS.md`（已被自动加载）里的通用规则。

## 三条最重要的设计事实

1. **速度是预测式预警的第一性需求，但现有雷达不输出速度。**
   雷达（人存版）的 `velo_val` 字段是「预留」，无实际速度。因此**目标速度只能由「摄像头多帧跟踪 + IMU ego-motion」提供**。任何绕过这一点的「雷达差分测速」尝试都已论证不可行（单目标输出无法帧间关联）。

2. **传感器分工是固定的，别让传感器干别人的活。**
   - 雷达：全天候「有没有 + 多远」
   - 摄像头：身份 / 角度 / 速度（靠跟踪）
   - IMU：本体运动（ego-motion）/ 头姿

3. **硬件实情与 PROJECT.md 有几处不一致**（详见 references/hardware.md）：
   - 雷达实际型号是 **MS60-1211S80M（人存版）**，不是 PROJECT.md 写的 MS60-1211S80；芯片是 AT6010。
   - IMU（HW-991）实为 **BMI270**。

## 需要细节时按需读取

- 雷达 / IMU / 摄像头协议与引脚 → `references/hardware.md`
- 已排查的坑、结论、回滚点 → `references/debug-log.md`
- 构建 / 烧录 / 串口 / 工具链 → `references/toolchain.md`
