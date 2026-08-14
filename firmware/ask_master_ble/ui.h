#ifndef UI_H
#define UI_H

#include <Arduino.h>

// System language, defined in the main .ino. "zh" = Chinese, "en" = English.
extern String sysLang;
// L() returns a plain const char* (not __FlashStringHelper*) so it can be
// passed to functions expecting const char*.
#define L(zh, en) (sysLang == "zh" ? (const char*)(zh) : (const char*)(en))

void drawIdleScreen(const char* version, const char* ip, bool showSetupHint);
// 统一待机界面：未连接/已连接共用同一界面，仅状态栏圆点、正文内容不同。
void drawStandbyScreen(bool connected);
void drawSetupScreen(const char* label, const char* context, const char* inputBuffer);
void drawSetupSummaryScreen(const char* ssid, const char* serverIP, uint16_t port);
void drawNetworkListScreen(const String networks[], int networkCount, int8_t rssi[]);
int measureWordWrappedHeight(const char* text, int x, int maxWidth);
// Height of one wrapped body line, so scrolling can advance whole lines
// instead of cutting glyphs in half.
int uiBodyLineHeight();
// Returns max valid scrollY for the given screen type so callers don't
// scroll past the last visible line.
int computeMaxScroll(const char* type, const char* question, const char* context,
                      const String options[], int optionCount);
void drawSettingsMenuScreen(bool hasConfig, const char* currentSSID, const char* currentServer);
void drawWiFiSelectScreen(const String networks[], int networkCount, int8_t rssi[], bool showSaved);
void drawServerSelectScreen(const char* ips[], int ports[], int count);
void drawAskScreen(const char* question, const char* context, const char* inputBuffer, int scrollY);
void drawEscalateScreen(const char* question, const char* context, const char* inputBuffer, int scrollY);
void drawConfirmScreen(const char* statement, const char* consequence, int scrollY);
void drawChooseScreen(const char* question, const char* context, const String options[], int optionCount, int scrollY);
void drawSleepScreen();

// P1 语音输入：按住 PTT 说话时的全屏录音界面（红点 + 时长）
void drawRecordingScreen(unsigned long elapsedMs);

// Ask/escalate screens with an active pinyin IME. `composing` is the raw pinyin
// being typed (e.g. "ni"), `cands` is a NUL-terminated UTF-8 string of candidate
// hanzi shown as a numbered bar above the input row. When `composing` is empty
// the IME bar is hidden and the screen looks identical to the plain version.
void drawAskScreenIME(const char* question, const char* context,
                      const char* inputBuffer, int scrollY,
                      const char* composing, const char* cands,
                      bool pinyinMode);
void drawEscalateScreenIME(const char* question, const char* context,
                           const char* inputBuffer, int scrollY,
                           const char* composing, const char* cands,
                           bool pinyinMode);

#endif // UI_H
