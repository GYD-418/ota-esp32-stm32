# OTAESP32STM32 — 双 MCU 远程固件升级系统

## 📋 项目概述

通过 **ESP32-S3** 经 WiFi 从云端下载固件，再经 UART + SPI 转发给 **STM32F407**，实现两个 MCU 的远程 OTA（空中升级）。

- **ESP32-S3**：WiFi 联网 + Web 配网 + 自升级 + 为 STM32 转发固件
- **STM32F407VETx**：UART 命令 + SPI DMA 接收 + Flash 自写 + Bootloader 搬运验证

---

## 📁 目录结构

```
OTAESP32STM32/
├── ESPOTA/                          ← ESP32-S3 项目 (ESP-IDF v5.5.4)
│   ├── main/
│   │   ├── ESPOTA.c                 ← 主程序 (WiFi/Web/OTA 入口)
│   │   ├── ota_manager.c/h          ← ESP32 自升级模块
│   │   ├── stm32_forwarder.c/h      ← STM32 固件转发模块 (UART+SPI)
│   │   ├── status_led.c/h           ← WS2812 RGB 状态指示灯
│   │   ├── ota_config.h             ← OTA 服务端 URL 配置 ★
│   │   ├── dashboard.html           ← Web 仪表盘 (OTA 控制面板)
│   │   ├── wifi_config.html         ← Web 配网页面
│   │   ├── done.html                ← 配网完成页面
│   │   └── wifi_provisioning/       ← WiFi 配网模块 (AP + DNS + 凭证管理)
│   ├── partitions_ota.csv           ← OTA 分区表 (16MB Flash)
│   └── sdkconfig                    ← ESP-IDF 配置
│
├── OTAESP32STM32/                   ← STM32F407 项目 (CMake + HAL)
│   ├── Core/
│   │   ├── Inc/                     ← 头文件
│   │   │   ├── main.h               ← 主头文件 (分区/引脚宏定义)
│   │   │   ├── ota_receiver.h       ← OTA 接收器接口
│   │   │   ├── bootloader.h         ← Bootloader 接口
│   │   │   ├── firmware_header.h    ← 固件头结构体
│   │   │   ├── oled.h/i2c.h         ← OLED 显示驱动
│   │   │   └── oled_data.h          ← OLED 字库
│   │   └── Src/                     ← 源文件
│   │       ├── main.c               ← App 入口 (LED/USART/SPI/OTA)
│   │       ├── ota_receiver.c       ← OTA 接收逻辑 (UART 命令 + SPI DMA + Flash)
│   │       ├── bootloader.c         ← Bootloader (升级检测 + Flash 搬运 + 跳转)
│   │       ├── bootloader_main.c    ← Bootloader 入口
│   │       ├── oled.c/i2c.c         ← OLED 驱动 (硬件 I2C)
│   │       └── oled_data.c          ← OLED 字库数据
│   ├── bootloader.ld               ← Bootloader 链接脚本 (32K @ 0x08000000)
│   ├── STM32F407XX_FLASH.ld         ← App 链接脚本 (224K @ 0x08008000)
│   ├── cmake/
│   │   ├── gcc-arm-none-eabi.cmake  ← GCC 交叉编译工具链
│   │   ├── starm-clang.cmake        ← ST ARM Clang 工具链 (备用)
│   │   ├── gen_version.cmake        ← 自动生成 version.txt
│   │   └── stm32cubemx/             ← CubeMX 生成的 CMake
│   └── Drivers/                     ← CMSIS + STM32F4 HAL 驱动
│
└── README.md                        ← 本文件
```

---

## 🔌 硬件接线

### ESP32-S3 ↔ STM32F407

| ESP32-S3 引脚 | 方向 | STM32F407 引脚 | 功能 |
|--------------|------|---------------|------|
| GPIO17 | → | PD6 (USART2 RX) | UART 命令通道 |
| GPIO18 | ← | PD5 (USART2 TX) | UART 命令通道 |
| GPIO10 | → | PA5 (SPI1 SCK) | SPI 时钟 |
| GPIO11 | → | PA7 (SPI1 MOSI) | SPI 数据输出 |
| GPIO12 | ← | PA6 (SPI1 MISO) | SPI 数据输入 |
| GPIO13 | → | PB0 (SPI1 NSS) | SPI 片选 |
| GPIO14 | → | NRST (Pin 7) | STM32 硬件复位 (可选) |
| GND | ↔ | GND | 共地 |

### STM32F407 ↔ OLED (I2C)

| STM32F407 | OLED 模块 |
|-----------|----------|
| PB6 (SCL) | SCL |
| PB7 (SDA) | SDA |
| 3.3V | VCC |
| GND | GND |

### STM32F407 ↔ USB-TTL (调试)

| STM32F407 | USB-TTL |
|-----------|---------|
| PA9 (TX) | RX |
| PA10 (RX) | TX |
| GND | GND |

### STM32F407 状态指示灯

| STM32F407 | 外接 LED |
|-----------|---------|
| PA0 | 1KΩ 电阻 → LED 阳极 → GND |

---

## ⚙️ OTA 服务端配置

### 阿里云 OSS 目录结构

需要创建两个文件夹并上传文件：

```
STM32_OTA/
├── version.txt          ← 版本号 (如 20250617-120000\nOTAESP32STM32.bin)
└── OTAESP32STM32.bin    ← STM32 固件

ESP32_OTA/
├── version.txt          ← 版本号 (如 20250617-120000\nESPOTA.bin)
└── ESPOTA.bin           ← ESP32 固件
```

**version.txt 格式**（第一行版本号，第二行固件文件名）：
```
20250617-120000
OTAESP32STM32.bin
```

### 修改服务端地址

编辑 `ESPOTA/main/ota_config.h`：

```c
// STM32 固件下载地址
#define STM32_OTA_BASE_URL   "http://你的服务器地址/STM32_OTA"

// ESP32 固件下载地址
#define ESP32_OTA_BASE_URL   "http://你的服务器地址/ESP32_OTA"
```

---

## 🔨 编译与烧录

### 前置条件

- **ESP32-S3**：ESP-IDF v5.5.4 环境已配置（`C:\Espressif\frameworks\esp-idf-v5.5.4`）
- **STM32F407**：`arm-none-eabi-gcc` 在 PATH 中
- **STM32F407**：`STM32CubeProgrammer` (用于烧录) 已安装

### ESP32 编译烧录

```bat
cd ESPOTA
idf.py build flash monitor
```

### STM32 编译

```bat
cd OTAESP32STM32
cmake --preset Debug
cmake --build --preset Debug
```

编译后 `build/Debug/` 目录自动生成：
- `OTAESP32STM32.bin` — App 固件
- `bootloader.bin` — Bootloader 固件
- `version.txt` — 版本号文件（时间戳格式）

### STM32 烧录

```bat
# 完全抹除后烧录
cd OTAESP32STM32
STM32_Programmer_CLI -c port=SWD -e all
STM32_Programmer_CLI -c port=SWD -w build\Debug\bootloader.bin 0x08000000
STM32_Programmer_CLI -c port=SWD -w build\Debug\OTAESP32STM32.bin 0x08008000
```

**⚠️ 首次烧录完成后必须拔掉 ST-Link 调试器**，否则 OTA 完成后 STM32 可能无法自动复位。

---

## 🧠 STM32 Flash 分区

| 区域 | 起始地址 | 大小 | 用途 |
|------|---------|------|------|
| Bootloader | 0x08000000 | 32 KB | 上电检查升级标志 → 搬运固件 → 校验 → 跳转 |
| App | 0x08008000 | 224 KB | 业务固件 + OTA 接收逻辑 |
| Download | 0x08040000 | 240 KB | 暂存新固件 |
| Config | 0x0807C000 | 16 KB | 升级标志 + 版本号 + 启动计数 |

---

## 📡 通信协议

### UART 命令帧（ESP32 ↔ STM32 USART2）

```
┌──────┬──────┬──────┬─────────┬──────────┬────────┐
│ 0xAA │ 0x55 │ CMD  │ DataLen │  Data    │ CRC16  │
│ 1B   │ 1B   │ 1B   │ 2B (BE) │ N bytes  │ 2B (BE) │
└──────┴──────┴──────┴─────────┴──────────┴────────┘
```

| 命令 | 值 | 方向 | 说明 |
|------|-----|------|------|
| OTA_START | 0x01 | ESP→STM | 开始升级 (payload: size[4]+CRC32[4]+ver[4]) |
| OTA_DATA | 0x02 | STM→ESP | 块确认 (payload: block_index[2]) |
| OTA_VERIFY | 0x03 | ESP→STM | CRC32 校验请求 |
| OTA_ABORT | 0x04 | ESP→STM | 中止升级 |
| OTA_COMPLETE | 0x05 | STM→ESP | 升级完成，即将重启 |
| OTA_BOOTED | 0x06 | STM→ESP | 启动完成（升级成功后发送） |
| OTA_ERROR | 0xFE | 双向 | 错误 |

### SPI 数据块（ESP32 Master → STM32 Slave）

```
┌──────┬──────────┬──────────┬───────────┬────────┐
│ 0xA5 │ BlockIdx │ DataSize │   Data    │ CRC16  │
│ 1B   │ 2B (BE)  │ 2B (BE)  │ N bytes   │ 2B (BE) │
└──────┴──────────┴──────────┴───────────┴────────┘
```

- 块大小：256 字节
- 每块带 CRC16 校验
- 末块不足 256 字节时用 0xFF 填充

---

## 🖥️ Web 仪表盘

ESP32 连接 WiFi 后，浏览器访问 `http://<ESP32-IP>/`：

- **ESP32 OTA 升级**：输入 URL → 点击升级
- **STM32 OTA 升级**：输入 URL → 点击升级
- 显示当前固件版本

---

## 🟢 RGB 状态指示灯 (WS2812 GPIO48)

| 颜色 | 状态 |
|------|------|
| 🔵 蓝色呼吸 | 正在连接 WiFi |
| 🟢 绿色常亮 | WiFi 已连接，就绪 |
| 🟡 黄色慢闪 | AP 配网模式 |
| 🟣 紫色脉冲 | ESP32 OTA 升级中 |
| 🟠 橙色脉冲 | STM32 OTA 升级中 |
| 🔴 红色快闪 | 错误 |

---

## 🔘 BOOT 按钮功能

ESP32 的 BOOT 按钮（GPIO0）有以下功能：

| 当前状态 | 按 BOOT 效果 |
|---------|-------------|
| 未连接 WiFi | 自动开启 AP 配网模式 |
| 已连接 WiFi | 立即检查固件更新 (ESP32 + STM32) |

---

## 🚀 OTA 工作流程

### 手动触发（仪表盘）

1. 浏览器打开 ESP32 仪表盘
2. 输入固件 URL → 点击升级
3. ESP32 下载 → 校验 → 转发 STM32 → STM32 写 Flash → 校验 → 复位
4. Bootloader 检测升级标志 → 搬运 Download→App → 跳转新固件

### 自动触发（上电自检）

1. ESP32 上电 → WiFi 连接
2. 等 5 秒 → 自动 HTTP GET `version.txt`
3. 对比 NVS 中存储的上次版本号
4. 不同 → 自动下载固件 → OTA 升级
5. 相同 → 跳过

---

## 🛠️ 常用命令

### ESP32

```bat
# 编译
cd ESPOTA
idf.py build

# 烧录 + 串口监视
idf.py flash monitor

# 清除编译缓存
idf.py fullclean
```

### STM32

```bat
# 编译（Debug）
cd OTAESP32STM32
cmake --build --preset Debug

# 只烧录 App
STM32_Programmer_CLI -c port=SWD -w build\Debug\OTAESP32STM32.bin 0x08008000

# 只烧录 Bootloader
STM32_Programmer_CLI -c port=SWD -w build\Debug\bootloader.bin 0x08000000

# 彻底抹除
STM32_Programmer_CLI -c port=SWD -e all
```

### 本地 OTA 测试

```bat
# 起 HTTP 服务器
cd OTAESP32STM32\build\Debug
python -m http.server 8080

# 仪表盘 URL
http://<PC-IP>:8080/OTAESP32STM32.bin
```

---

## ⚠️ 注意事项

1. **STM32 CubeMX 重新生成代码会覆盖手动添加的文件**。新增文件请在 `CMakeLists.txt` 的"Add user sources here"位置添加
2. **STM32 编译后拔掉 ST-Link**，否则软件复位可能被阻止
3. **STM32 PA9/PA10 仅用于调试**，不要和 ESP32 的 UART 并联
4. **ESP32 固件版本号**在 `CMakeLists.txt` 的 `PROJECT_VER` 中设置（自动使用时间戳）
5. **STM32 固件版本号**在 `cmake/gen_version.cmake` 中设置（自动使用时间戳）
6. **首次配网**通过手机连接 `ESP32-OTA-XXXX` WiFi，浏览器访问 `192.168.4.1`
