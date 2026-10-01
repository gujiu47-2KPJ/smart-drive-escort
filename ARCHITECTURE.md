# 系统架构设计


## 总体架构


              Camera
                 |
                 |
            Vision Task
                 |
                 |
Radar -----> Fusion Task <----- IMU
 |              |
 |              |
Radar Task   IMU Task


                 |
                 |
            Risk Decision
                 |
                 |
          Alarm Output


---

# 模块划分


## Radar模块

功能：

读取MS60-1211S80毫米波雷达数据。


负责：

- UART通信
- 数据解析
- 输出目标信息


输出：

RadarData


包含：

- 距离
- 速度
- 角度
- 目标状态


---

## IMU模块


功能：

读取HW991惯性数据。


负责：

- I2C/SPI通信
- 数据转换


输出：

IMUData


包含：

- 加速度
- 角速度
- 姿态信息


---

## Camera模块


功能：

获取视觉信息。


负责：

- 摄像头初始化
- 图像采集
- 图像缓存


后续：

尝试轻量视觉模型。


---

## Fusion模块


输入：

RadarData

+

IMUData

+

VisionData


输出：

环境风险状态。


例如：

Normal

Warning

Danger


---

## Alarm模块


根据风险等级：

输出：

- 蜂鸣器
- LED
- 通信提示


---

# FreeRTOS任务设计


## RadarTask

职责：

周期读取雷达数据。


↓

RadarQueue


---

## IMUTask

职责：

周期读取IMU。


↓

IMUQueue


---

## CameraTask

职责：

采集图像。


↓

CameraQueue


---

## FusionTask

职责：

读取多个Queue。

进行：

数据融合


---

## DecisionTask

职责：

风险判断。


---

## AlarmTask

职责：

执行报警。


---

# 数据流


Sensor

↓

Driver

↓

Task

↓

Queue

↓

Fusion

↓

Decision

↓

Output