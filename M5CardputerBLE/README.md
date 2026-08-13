# M5CardputerBLE

基于 M5Stack 官方 [M5Cardputer](https://github.com/m5stack/M5Cardputer) 驱动库（MIT 许可）生成的 **BLE 适配版**，适用于 M5Cardputer 与 M5Cardputer-ADV。

在保持官方库 API 完全兼容的基础上，新增了一个 **BLE UART 数据通道**（Nordic UART Service，NUS），实现 Cardputer 与手机 App 之间的**双向收发**：

- 手机 -> Cardputer：向 RX 特征写入数据（write / write-without-response）
- Cardputer -> 手机：通过 TX 特征通知（notify）发送数据

---

## 一、与官方库的关系

| 项目 | 说明 |
| ---- | ---- |
| 兼容性 | `M5Cardputer.h` 主头文件名、类名、成员名与官方库完全一致，原有代码无需改动 |
| 新增 API | `M5Cardputer.BLE`（`BLE_Class` 实例） |
| 依赖 | 官方库（M5Unified、M5GFX）+ **NimBLE-Arduino** |
| 注意事项 | **安装前请先卸载/删除原 M5Cardputer 库**，否则两个库都提供 `M5Cardputer.h`，会引发头文件冲突 |

驱动层源码（`src/utility/`、`src/M5Cardputer.cpp`）与官方库 1.1.1 保持一致，仅 `src/M5Cardputer.h` 增加了 BLE 模块的引入与成员。

## 〇、编译验证状态

已在本机用 arduino-cli 完成实际编译验证，**编译通过**，并附带了可直接烧录的固件：

| 项目 | 值 |
| ---- | ---- |
| 工具链 | arduino-cli 1.5.1 + `m5stack:esp32@2.1.4`（ESP32 core 2.0.x 系列） |
| 板型 | `m5stack:esp32:m5stack_cardputer`（ADV 同芯片，通用） |
| 依赖版本 | M5Unified 0.2.19、M5GFX 0.2.26、NimBLE-Arduino 1.4.0 |
| 编译结果 | 成功：程序 665,929 B（50%），全局变量 33,168 B（10%） |
| 固件产物 | 见 `firmware/` 目录（bootloader / partitions / firmware 三个 bin） |

固件烧录方法见 [firmware/FLASH.md](firmware/FLASH.md)。

> 说明：编译中曾发现并修复一处 NimBLE-Arduino 1.4.x 属性宏兼容问题（`NimBLECharacteristic::PROPERTY_*` 改为两版通用的 `NIMBLE_PROPERTY::*`），修复后编译通过。

## 二、环境要求与版本搭配

M5Cardputer-ADV 官方推荐使用 ESP32 Arduino core 2.0.17（M5Stack Board Manager 或 Espressif 官方均可）。本库通过条件编译同时兼容新旧两代 core 与 NimBLE：

| ESP32 core | NimBLE-Arduino | 说明 |
| ---------- | -------------- | ---- |
| 2.x（推荐） | **1.4.x** | M5Stack 官方推荐组合 |
| 3.0.x / 3.1.x / 3.2.x | **2.x** | core 3.x 已移除内置 BLE Arduino 库 |
| 3.3.0 及以上 | 2.4.x+ | core 3.3+ 自带 NimBLE（S3），与外部 NimBLE-Arduino 可能存在符号冲突，如遇链接错误请换回 core 2.x，或改用 core 自带 `BLEDevice.h` 自行移植 |

代码中通过 `ESP_ARDUINO_VERSION_MAJOR` 自动适配 1.x/2.x 的回调签名差异（`onWrite`、`onConnect`、`onDisconnect`），两个版本均可直接编译。

## 三、安装

### Arduino IDE

1. 下载本目录 `M5CardputerBLE` 整个文件夹
2. 移除已安装的原 `M5Cardputer` 库（`<Arduino 库目录>/libraries/M5Cardputer`）
3. 将 `M5CardputerBLE` 复制到 `<Arduino 库目录>/libraries/`
4. 在“库管理器”中安装依赖：`M5Unified`、`M5GFX`、`NimBLE-Arduino`（版本见上表）
5. 打开 `examples/Basic/bleUart/bleUart.ino`，板型选择 `M5Stack M5Cardputer-ADV`（或 M5Cardputer）烧录

### PlatformIO

```ini
[env:cardputer_adv]
platform = espressif32@^6.9.0        ; 对应 core 2.x；core 3.x 请用 platform 6.x+/espressif32 dev
board = m5stack-cardputer
framework = arduino
lib_deps =
    m5stack/M5Unified
    m5stack/M5GFX
    h2zero/NimBLE-Arduino@^1.4.0     ; core 3.x 请改为 @^2.3.9
lib_ldf_mode = deep+
```

## 四、快速开始

```cpp
#include "M5Cardputer.h"

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);

    M5Cardputer.BLE.setConnectionCallback([](bool c) {
        Serial.printf("BLE %s\n", c ? "connected" : "disconnected");
    });
    M5Cardputer.BLE.begin("M5Cardputer", true);  // 设备名 + 自动广播
}

void loop() {
    M5Cardputer.update();

    // 发送：键盘输入实时发给手机
    if (M5Cardputer.Keyboard.isChange()) {
        auto& st = M5Cardputer.Keyboard.keysState();
        for (char c : st.word) M5Cardputer.BLE.send(c);
        if (st.enter) M5Cardputer.BLE.sendLine("");  // "\r\n"
    }

    // 接收：读取手机发来的数据并回显
    while (M5Cardputer.BLE.available()) {
        char c = (char)M5Cardputer.BLE.read();
        Serial.print(c);
    }
}
```

## 五、API 参考（`M5Cardputer.BLE`）

### 生命周期

| 方法 | 说明 |
| ---- | ---- |
| `begin(const char* deviceName = "M5Cardputer", bool autoAdvertise = true)` | 初始化并广播；重复调用仅重新广播 |
| `end()` | 停止广播并断开连接（可再次 `begin()` 恢复） |
| `connected()` | 是否有手机连接 |
| `initialized()` | 是否已初始化 |

### 回调（均在 NimBLE 任务上下文中执行，请保持简短）

| 方法 | 说明 |
| ---- | ---- |
| `setRecvCallback(void(*)(const uint8_t*, size_t))` | 收到手机数据时回调；数据同时写入内部缓冲 |
| `setConnectionCallback(void(*)(bool))` | 连接 / 断开时回调，参数为当前连接状态 |

### 发送（Cardputer -> 手机）

| 方法 | 说明 |
| ---- | ---- |
| `size_t send(const uint8_t* data, size_t len)` | 发送原始字节，按协商 MTU 自动分块 |
| `size_t send(const char* str)` / `send(const String&)` / `send(char c)` | 发送文本 / 单字符 |
| `size_t sendLine(const char* / const String&)` | 发送一行文本，末尾自动追加 `\r\n` |

### 接收（手机 -> Cardputer，1024 字节环形缓冲）

| 方法 | 说明 |
| ---- | ---- |
| `size_t available()` | 可读字节数 |
| `int read()` | 读 1 字节，无数据返回 -1 |
| `size_t readBytes(uint8_t* buf, size_t maxLen)` | 批量读取 |
| `String readString()` | 一次性读空当前缓冲（非阻塞） |
| `void flushRx()` | 清空缓冲 |

## 六、手机端联调

1. 手机安装 **nRF Connect**（Android/iOS）或 **BLE 调试助手**
2. 扫描到设备名 `M5Cardputer`（可自定义），点击连接
3. 找到 Nordic UART Service（UUID `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`）：
   - 向 **RX 特征**（`6E400002-...`，属性：Write / Write Without Response）写入数据 -> Cardputer 屏幕显示
   - 订阅 **TX 特征**（`6E400003-...`，属性：Notify）-> 按动 Cardputer 键盘即可实时收到字符
4. 断开连接后 Cardputer 自动重新广播

> 提示：手机需先订阅 TX 特征的 Notify 才能收到 Cardputer 发送的数据。

## 七、目录结构

```
M5CardputerBLE/
├── library.json / library.properties / keywords.txt
├── README.md
├── src/
│   ├── M5Cardputer.h         # 官方头文件 + BLE 成员（唯一改动处）
│   ├── M5Cardputer.cpp       # 与官方库一致
│   ├── CardputerBLE.h        # 新增：BLE UART 模块
│   ├── CardputerBLE.cpp      # 新增：NimBLE 实现（兼容 core 2.x / 3.x）
│   └── utility/              # 官方驱动层（键盘、TCA8418 等），与官方库一致
└── examples/
    └── Basic/
        └── bleUart/bleUart.ino   # 键盘<->手机 双向收发示例
```

## 八、许可

- 驱动层源码版权归 M5Stack Technology CO LTD 所有，MIT 许可
- BLE 模块（`CardputerBLE.h/cpp`）为派生贡献，MIT 许可
- 本库整体派生自 [M5Cardputer](https://github.com/m5stack/M5Cardputer)（MIT）
