# 固件烧录说明

本目录包含 `bleUart` 示例编译产出的固件（M5Cardputer / M5Cardputer-ADV 通用，ESP32-S3）：

| 文件 | 烧录地址 | 说明 |
| ---- | -------- | ---- |
| `bleUart.bootloader.bin` | `0x0` | Bootloader |
| `bleUart.partitions.bin`  | `0x8000` | 分区表 |
| `bleUart.firmware.bin`   | `0x10000` | 应用程序固件 |

## 编译信息

- 工具链：arduino-cli 1.5.1 + `m5stack:esp32@2.1.4`（ESP32 core 2.0.x 系列）
- 板型：`m5stack:esp32:m5stack_cardputer`（ADV 同芯片，通用）
- 依赖：M5Unified 0.2.19、M5GFX 0.2.26、NimBLE-Arduino 1.4.0
- 体积：程序 665,929 B（50%），全局变量 33,168 B（10%）

## 烧录方式（任选其一）

### 方式一：esptool（命令行）

```bat
esptool.py --chip esp32s3 --port COMx --baud 921600 write_flash ^
  0x0 bleUart.bootloader.bin ^
  0x8000 bleUart.partitions.bin ^
  0x10000 bleUart.firmware.bin
```

### 方式二：Arduino IDE 重新编译上传（推荐）

1. 打开 `examples/Basic/bleUart/bleUart.ino`
2. 板型选择 `M5Stack M5Cardputer`（ADV 同款）
3. 点击“上传”

### 方式三：Flash Download Tool（Windows GUI）

1. 下载 [ESP32 Flash Download Tool](https://www.espressif.com/en/support/download/other-tools)
2. 芯片选 ESP32-S3，按上表地址加载 3 个 bin
3. 连接串口后点击 START

## 使用验证

1. 上电后屏幕显示 `BLE: M5Cardputer [ADVERTISING]`
2. 手机用 nRF Connect / BLE 调试助手连接设备名 `M5Cardputer`
3. 手机向 RX 特征（`6E400002-...`）写入数据，Cardputer 屏幕实时显示
4. 订阅 TX 特征（`6E400003-...`），按动键盘即可在手机端收到字符
