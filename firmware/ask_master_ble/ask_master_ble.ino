#include <Arduino.h>
#include <M5Cardputer.h>
#include <Preferences.h>
#include "config.h"
#include "ui.h"
#include "pinyin_ime.h"
#include <ArduinoJson.h>

#define DEBUG_SERIAL

enum State {
    SLEEP,
    IDLE,
    RENDERING,
    WAITING_INPUT,
    SENDING
};

static constexpr const char* APP_VERSION = "ask-master BLE v1.0.0";
static constexpr const char* BLE_DEVICE_NAME = "ask-master";
// One payload must fit here whole. UTF-8 CJK costs 3 bytes per character, so a
// fully populated Chinese choose message (question + context + 6 options) needs
// several KB; 1 KB used to overflow and drop the message.
static constexpr size_t MAX_RX_BUFFER = 4096;

State currentState = SLEEP;
char inputBuffer[81] = {0};
int inputLength = 0;

// Pinyin IME state.
PinyinIME ime;

// System language: "zh" or "en". Stored in NVS so it survives reboot.
// Toggled on the SLEEP screen by pressing L. The L() macro is in ui.h.
Preferences prefs;
String sysLang = "zh";  // default to Chinese
String currentQuestion;
String currentContext;
String currentOptions[6];
int currentOptionCount = 0;
String currentType;

// BLE 接收缓冲（可能分多次 notify 到达）
// onBLEReceive() 运行在 NimBLE 任务上下文，handleBLEInput() 运行在主循环，
// 两者共享该缓冲，必须用临界区保护，否则 rxLen 会被并发破坏，
// 进而导致缓冲永久填满、设备再也收不到任何消息（只能重启恢复）。
static char rxBuffer[MAX_RX_BUFFER];
static size_t rxLen = 0;
static portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

int scrollOffset = 0;
int maxScrollOffset = 0;

// BLE 回调运行在 NimBLE 任务（core 0），主循环运行在 core 1。
// 如果回调里直接画屏，两个核会并发抢 SPI 总线，导致 ST7789 收到
// 错乱的 setAddrWindow + 像素数据，屏幕花屏且不自愈。
// 回调只设标志，绘制统一在 loop()（core 1）执行。
static volatile bool bleJustConnected = false;
static volatile bool bleJustDisconnected = false;

unsigned long lastActivityTime = 0;
unsigned long lastIdleRedraw = 0;
// IDLE 画面静止，低频重绘即可。过高频率的 pushSprite 会与
// NimBLE 蓝牙任务竞争 SPI，偶发导致屏幕显示撕裂/错位（"华容道"）。
static constexpr unsigned long IDLE_REDRAW_MS = 500;

// ESCALATE 的告警闪烁由重绘驱动，必须自行定时刷新；
// 否则只有按键触发重绘时才闪一下（等于没有闪烁效果）。
static unsigned long lastFlashRedraw = 0;
static constexpr unsigned long FLASH_REDRAW_MS = 200;

// 方向键长按连续滚动：首次按下立即滚动一行，按住超过
// SCROLL_REPEAT_DELAY_MS 后进入连发，每 SCROLL_REPEAT_RATE_MS 滚动一行。
static constexpr unsigned long SCROLL_REPEAT_DELAY_MS = 350;
static constexpr unsigned long SCROLL_REPEAT_RATE_MS = 90;
static int scrollHeldDir = 0;  // -1 上, +1 下, 0 未按住
static unsigned long scrollHoldStart = 0;
static unsigned long lastScrollStep = 0;

void onBLEReceive(const uint8_t* data, size_t len);
void renderCurrentScreen();
void handleKeyboard();
bool handleScrollKeys();
void sendReply(const String& reply);
void clearCurrentPrompt();
void drawIdle();
void updateMaxScroll();
void handleBLEInput();
void processLine(const String& line);
void transitionToSleep();

#ifdef DEBUG_SERIAL
  #define DBG(...) Serial.println(__VA_ARGS__)
#else
  #define DBG(...)
#endif

void setup() {
    #ifdef DEBUG_SERIAL
    Serial.begin(115200);
    delay(500);
    #endif

    DBG("=== ask-master BLE boot ===");

    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);

    // Load saved language preference.
    prefs.begin("ask-master", true);
    sysLang = prefs.getString("lang", "zh");
    prefs.end();

    drawSleepScreen();

    // 注册 BLE 回调
    // 回调运行在 NimBLE 任务（core 0），绝不能在这里画屏，
    // 否则与主循环（core 1）的绘制并发抢 SPI 导致花屏。
    // 只设标志位，loop() 检测后在 core 1 统一绘制。
    M5Cardputer.BLE.setConnectionCallback([](bool connected) {
        DBG(String("BLE ") + (connected ? "connected" : "disconnected"));
        if (connected) {
            bleJustConnected = true;
        } else {
            bleJustDisconnected = true;
        }
    });
    M5Cardputer.BLE.setRecvCallback(onBLEReceive);

    // 开始广播
    M5Cardputer.BLE.begin(BLE_DEVICE_NAME, true);
    DBG("BLE advertising as " + String(BLE_DEVICE_NAME));

    lastActivityTime = millis();
}

void loop() {
    M5Cardputer.update();

    // 处理 BLE 连接状态变化（从 core 0 回调转交到 core 1 执行）
    if (bleJustConnected) {
        bleJustConnected = false;
        currentState = IDLE;
        lastActivityTime = millis();
        drawIdle();
    }
    if (bleJustDisconnected) {
        bleJustDisconnected = false;
        transitionToSleep();
    }

    handleBLEInput();

    if (currentState == IDLE) {
        if (millis() - lastIdleRedraw > IDLE_REDRAW_MS) {
            lastIdleRedraw = millis();
            drawIdle();
        }
    }

    // 长按滚动先处理：它每帧检查按键状态，不依赖 isChange()
    bool scrolled = handleScrollKeys();

    if (!scrolled) {
        handleKeyboard();
    }

    // ESCALATE 告警闪烁：定时重绘驱动
    if (!scrolled && currentState == WAITING_INPUT && currentType == "escalate" &&
        millis() - lastFlashRedraw > FLASH_REDRAW_MS) {
        lastFlashRedraw = millis();
        renderCurrentScreen();
    }

    yield();  // 让出 CPU 给 NimBLE 任务，避免 SPI/BLE 竞争
}

// ---------------------------------------------------------------------------
// 方向键滚动（支持长按连发）
//
// 有输入行的界面（ask/escalate/中文输入）只认 Fn+↑/↓，
// 这样 ';' 和 '.' 仍可作为普通字符输入；
// confirm/choose 没有文本输入，裸 ';' / '.' 也能滚动。
// 返回 true 表示本帧已消费按键并重绘。
// ---------------------------------------------------------------------------
bool handleScrollKeys() {
    if (currentState != WAITING_INPUT || maxScrollOffset <= 0) {
        scrollHeldDir = 0;
        return false;
    }

    int dir = 0;
    if (M5Cardputer.Keyboard.isPressed()) {
        Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
        if (status.up) {
            dir = -1;
        } else if (status.down) {
            dir = 1;
        } else if (currentType == "confirm" || currentType == "choose") {
            for (char c : status.word) {
                if (c == ';') dir = -1;
                else if (c == '.') dir = 1;
            }
        }
    }

    if (dir == 0) {
        scrollHeldDir = 0;
        return false;
    }

    unsigned long now = millis();
    bool step = false;

    if (dir != scrollHeldDir) {
        // 新按下：立即响应一次
        scrollHeldDir = dir;
        scrollHoldStart = now;
        step = true;
    } else if (now - scrollHoldStart > SCROLL_REPEAT_DELAY_MS &&
               now - lastScrollStep > SCROLL_REPEAT_RATE_MS) {
        step = true;
    }

    if (!step) {
        return true;  // 仍按住但未到连发间隔：消费掉，避免落入字符输入
    }
    lastScrollStep = now;

    const int scrollStep = uiBodyLineHeight();
    int before = scrollOffset;
    scrollOffset += dir * scrollStep;
    if (scrollOffset < 0) scrollOffset = 0;
    if (scrollOffset > maxScrollOffset) scrollOffset = maxScrollOffset;

    if (scrollOffset != before) {
        renderCurrentScreen();
    }
    lastActivityTime = now;
    return true;
}

// ---------------------------------------------------------------------------
// BLE 数据到达：追加到接收缓冲，按行（\n）切分完整消息
// ---------------------------------------------------------------------------
void onBLEReceive(const uint8_t* data, size_t len) {
    portENTER_CRITICAL(&rxMux);
    for (size_t i = 0; i < len; i++) {
        if (rxLen >= MAX_RX_BUFFER - 1) {
            // 缓冲已满且不含完整行：丢弃这段残缺数据重新开始。
            // 绝不能静默丢弃后续字节，否则换行符永远等不到，
            // 接收会永久卡死。
            rxLen = 0;
        }
        rxBuffer[rxLen++] = (char)data[i];
    }
    portEXIT_CRITICAL(&rxMux);
}

void handleBLEInput() {
    // Static, not on the stack: the loop task only has 8 KB and this buffer is
    // as large as the receive buffer. handleBLEInput only runs on the main loop,
    // so a single shared instance is safe.
    static char line[MAX_RX_BUFFER];

    while (true) {
        bool hasLine = false;

        // 只在临界区内操作共享缓冲，取出一整行后立刻退出，
        // 后续的 JSON 解析与屏幕绘制都在临界区之外进行。
        portENTER_CRITICAL(&rxMux);
        size_t lineLen = 0;
        for (size_t i = 0; i < rxLen; i++) {
            if (rxBuffer[i] == '\n') {
                hasLine = true;
                lineLen = i;
                break;
            }
        }
        if (hasLine) {
            size_t copyLen = lineLen > sizeof(line) - 1 ? sizeof(line) - 1 : lineLen;
            memcpy(line, rxBuffer, copyLen);
            line[copyLen] = '\0';

            size_t remaining = rxLen - lineLen - 1;
            memmove(rxBuffer, rxBuffer + lineLen + 1, remaining);
            rxLen = remaining;
        }
        portEXIT_CRITICAL(&rxMux);

        if (!hasLine) {
            return;
        }

        String trimmed = String(line);
        trimmed.trim();
        if (trimmed.length() > 0) {
            DBG("RX: " + trimmed);
            processLine(trimmed);
        }
    }
}

void processLine(const String& message) {
    if (currentState == SENDING) {
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, message);
    if (error) {
        DBG("JSON parse error: " + String(error.c_str()));
        return;
    }

    clearCurrentPrompt();
    currentType = doc["type"].as<String>();
    currentQuestion = doc["question"].as<String>();
    currentContext = doc["context"].as<String>();
    scrollOffset = 0;
    maxScrollOffset = 0;

    if (currentType == "choose") {
        JsonArray opts = doc["options"].as<JsonArray>();
        currentOptionCount = 0;
        for (JsonVariant option : opts) {
            if (currentOptionCount >= 6) {
                break;
            }
            currentOptions[currentOptionCount++] = option.as<String>();
        }
    }

    currentState = RENDERING;
    updateMaxScroll();
    renderCurrentScreen();

    if (currentType == "ask") {
        M5Cardputer.Speaker.tone(BEEP_FREQ_ASK, BEEP_DURATION_MS);
    } else if (currentType == "escalate") {
        M5Cardputer.Speaker.tone(BEEP_FREQ_ESCALATE, BEEP_DURATION_MS);
    } else if (currentType == "confirm") {
        M5Cardputer.Speaker.tone(BEEP_FREQ_CONFIRM, BEEP_DURATION_MS);
    } else if (currentType == "choose") {
        M5Cardputer.Speaker.tone(BEEP_FREQ_CHOOSE, BEEP_DURATION_MS);
    }

    currentState = WAITING_INPUT;
    lastActivityTime = millis();
}

void renderCurrentScreen() {
    if (currentType == "ask") {
        if (ime.pinyinMode() && ime.composing()[0]) {
            // Build a space-separated candidate string for the UI.
            char candStr[128];
            candStr[0] = '\0';
            for (int i = 0; i < ime.candidateCount() && i < 9; i++) {
                if (i > 0) strncat(candStr, " ", sizeof(candStr) - strlen(candStr) - 1);
                strncat(candStr, ime.candidate(i), sizeof(candStr) - strlen(candStr) - 1);
            }
            drawAskScreenIME(currentQuestion.c_str(), currentContext.c_str(),
                             ime.text(), scrollOffset,
                             ime.composing(), candStr,
                             ime.pinyinMode());
        } else {
            drawAskScreenIME(currentQuestion.c_str(), currentContext.c_str(),
                             ime.text(), scrollOffset,
                             "", "", ime.pinyinMode());
        }
    } else if (currentType == "escalate") {
        if (ime.pinyinMode() && ime.composing()[0]) {
            char candStr[128];
            candStr[0] = '\0';
            for (int i = 0; i < ime.candidateCount() && i < 9; i++) {
                if (i > 0) strncat(candStr, " ", sizeof(candStr) - strlen(candStr) - 1);
                strncat(candStr, ime.candidate(i), sizeof(candStr) - strlen(candStr) - 1);
            }
            drawEscalateScreenIME(currentQuestion.c_str(), currentContext.c_str(),
                                  ime.text(), scrollOffset,
                                  ime.composing(), candStr,
                                  ime.pinyinMode());
        } else {
            drawEscalateScreenIME(currentQuestion.c_str(), currentContext.c_str(),
                                  ime.text(), scrollOffset,
                                  "", "", ime.pinyinMode());
        }
    } else if (currentType == "confirm") {
        drawConfirmScreen(currentQuestion.c_str(), currentContext.c_str(), scrollOffset);
    } else if (currentType == "choose") {
        drawChooseScreen(currentQuestion.c_str(), currentContext.c_str(), currentOptions, currentOptionCount, scrollOffset);
    }
}

void handleKeyboard() {
    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) {
        return;
    }

    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

    if (currentState == SLEEP) {
        for (char c : status.word) {
            DBG(String("SLEEP key: ") + c);
            if (c == 's' || c == 'S') {
                String info = L("蓝牙名称: ", "BLE: ") + String(BLE_DEVICE_NAME);
                drawIdleScreen(APP_VERSION, info.c_str(), false);
                delay(1500);
                transitionToSleep();
                return;
            }
            if (c == 'l' || c == 'L') {
                // Toggle system language.
                sysLang = (sysLang == "zh") ? "en" : "zh";
                prefs.begin("ask-master", false);
                prefs.putString("lang", sysLang);
                prefs.end();
                transitionToSleep();
                return;
            }
        }
        return;
    }

    if (currentState != WAITING_INPUT) {
        return;
    }

    // 滚动已由 handleScrollKeys() 在本帧之前处理

    if (currentType == "ask" || currentType == "escalate") {
        // Opt (the key between Ctrl and Alt on the Cardputer) toggles pinyin
        // mode. This lets the user fall back to raw ASCII for passwords, etc.
        if (status.opt) {
            ime.toggleMode();
            renderCurrentScreen();
            return;
        }

        // Space with a composing syllable selects the first candidate (the
        // most common pinyin-IME shortcut); space with no composing syllable
        // inserts a literal space.
        if (status.space) {
            if (ime.composing()[0]) {
                ime.select(0);
            } else {
                ime.handleKey(' ', false, false);
            }
            renderCurrentScreen();
            return;
        }

        bool needSend = false;
        bool needRedraw = false;
        for (char c : status.word) {
            PinyinIME::Action act = ime.handleKey(c, false, false);
            if (act == PinyinIME::Send) needSend = true;
            else if (act == PinyinIME::Redraw) needRedraw = true;
        }

        if (status.backspace || status.del) {
            PinyinIME::Action act = ime.handleKey(0, true, false);
            if (act == PinyinIME::Redraw) needRedraw = true;
        }

        if (status.enter) {
            PinyinIME::Action act = ime.handleKey(0, false, true);
            if (act == PinyinIME::Send) needSend = true;
        }

        if (needSend) {
            sendReply(String(ime.text()));
            return;
        }
        if (needRedraw) {
            renderCurrentScreen();
        }
        return;
    }

    if (currentType == "confirm") {
        for (char c : status.word) {
            if (c == 'y' || c == 'Y') {
                sendReply("y");
                return;
            }
            if (c == 'n' || c == 'N') {
                sendReply("n");
                return;
            }
        }
        return;
    }

    if (currentType == "choose") {
        for (char c : status.word) {
            if (c >= '1' && c <= '6') {
                int choice = c - '0';
                if (choice <= currentOptionCount) {
                    sendReply(String(c));
                    return;
                }
            }
        }
    }
}

void sendReply(const String& reply) {
    currentState = SENDING;
    String line = reply + "\n";
    M5Cardputer.BLE.send(line);
    M5Cardputer.Speaker.tone(BEEP_ANSWER_FREQ, BEEP_ANSWER_DURATION_MS);

    clearCurrentPrompt();
    delay(500);
    transitionToSleep();
}

void clearCurrentPrompt() {
    inputBuffer[0] = '\0';
    inputLength = 0;
    ime.reset();
    currentQuestion = "";
    currentContext = "";
    currentOptionCount = 0;
    currentType = "";
    scrollOffset = 0;
    maxScrollOffset = 0;

    for (int i = 0; i < 6; ++i) {
        currentOptions[i] = "";
    }
}

void drawIdle() {
    drawIdleScreen(APP_VERSION, L("蓝牙已连接", "BLE connected"), true);
}

void updateMaxScroll() {
    maxScrollOffset = computeMaxScroll(currentType.c_str(),
                                       currentQuestion.c_str(),
                                       currentContext.c_str(),
                                       currentOptions, currentOptionCount);
}

void transitionToSleep() {
    currentState = SLEEP;
    drawSleepScreen();
}
