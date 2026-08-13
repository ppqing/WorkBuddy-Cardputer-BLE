/**
 * @file bleUart.ino
 * @author M5CardputerBLE contributors
 * @brief M5Cardputer / M5Cardputer-ADV BLE UART (Nordic UART Service) 数据通道示例
 *
 * 功能：
 *   1. Cardputer 以广播名 "M5Cardputer"（可改）启动 BLE，提供标准 NUS 服务
 *   2. 手机 App（nRF Connect / BLE 调试助手 等）连接并订阅 TX 特征后：
 *        - 手机向 RX 特征写入数据 -> Cardputer 屏幕实时显示
 *        - Cardputer 键盘按键      -> 实时发送到手机
 *   3. 断开后自动重新广播，方便再次连接
 *
 * @Hardwares: M5Cardputer / M5Cardputer-ADV
 * @Dependent Library:
 *   M5Unified      : https://github.com/m5stack/M5Unified
 *   M5GFX          : https://github.com/m5stack/M5GFX
 *   NimBLE-Arduino : https://github.com/h2zero/NimBLE-Arduino
 *     (ESP32 core 2.x 请用 NimBLE-Arduino 1.4.x，ESP32 core 3.x 请用 2.x)
 */

#include "M5Cardputer.h"

static String s_logBuf;  // 手机发来的数据，屏幕显示缓冲

static void onBleConnection(bool connected)
{
    // 该回调在 NimBLE 任务上下文中执行，不要做耗时操作
    Serial.printf("[BLE] %s\n", connected ? "connected" : "disconnected");
}

static void onBleRecv(const uint8_t* data, size_t len)
{
    // 数据已自动写入内部接收缓冲，loop() 中用 read() 读取即可。
    // 这里也可以直接处理，但注意回调运行在 NimBLE 任务上下文中。
    Serial.printf("[BLE] recv %u bytes\n", (unsigned)len);
}

static void render()
{
    auto& d = M5Cardputer.Display;
    d.fillScreen(BLACK);
    d.setTextSize(1);
    d.setCursor(0, 0);

    d.setTextColor(GREEN, BLACK);
    d.printf("BLE: M5Cardputer %s\n", M5Cardputer.BLE.connected() ? "[CONNECTED]" : "[ADVERTISING]");
    d.setTextColor(WHITE, BLACK);
    d.print("Phone -> here | KB -> phone\n");
    d.print("---------------------------\n");
    d.print(s_logBuf);
}

void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);

    Serial.begin(115200);

    // 注册回调（均可选）
    M5Cardputer.BLE.setConnectionCallback(onBleConnection);
    M5Cardputer.BLE.setRecvCallback(onBleRecv);

    // 初始化并广播，设备名可自定义
    M5Cardputer.BLE.begin("M5Cardputer", true);

    render();
}

void loop()
{
    M5Cardputer.update();

    // ---------- 键盘 -> BLE ----------
    if (M5Cardputer.Keyboard.isChange()) {
        auto& state = M5Cardputer.Keyboard.keysState();
        if (M5Cardputer.BLE.connected() &&
            (!state.word.empty() || state.enter || state.backspace || state.tab || state.esc)) {
            for (char c : state.word) M5Cardputer.BLE.send(c);
            if (state.enter) M5Cardputer.BLE.send('\n');
            if (state.backspace) M5Cardputer.BLE.send('\b');
            if (state.tab) M5Cardputer.BLE.send('\t');
            if (state.esc) M5Cardputer.BLE.send(0x1b);
        }
    }

    // ---------- BLE -> 屏幕 ----------
    bool updated = false;
    while (M5Cardputer.BLE.available()) {
        char c = (char)M5Cardputer.BLE.read();
        if (c == '\r') continue;
        if (c == '\b') {
            if (s_logBuf.length() > 0) s_logBuf.remove(s_logBuf.length() - 1);
        } else {
            s_logBuf += c;
        }
        updated = true;
    }
    if (updated) {
        if (s_logBuf.length() > 200) s_logBuf.remove(0, 80);
        render();
    }

    // 每秒刷新一次状态栏（连接状态变化时文字更新）
    static uint32_t last = 0;
    if (millis() - last > 1000) {
        last = millis();
        render();
    }
}
