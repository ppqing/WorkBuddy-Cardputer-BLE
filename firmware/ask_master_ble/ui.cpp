#include "ui.h"
#include "ui_theme.h"
#include <M5Cardputer.h>
#include "config.h"

// ============================================================================
// ask-master UI — Bruce-style terminal, size-2 body for readability
//
// Layout (240 x 135):
//   y=0..13    status bar     (15 px, textSize 1, dim chrome)
//   y=14       hairline
//   y=16..96   body           (size-2 text, ~5 rows of 16 px)
//   y=97       hairline (when input row present)
//   y=99..114  input row      (size 2, ~16 px)
//   y=115      hairline
//   y=116..134 footer         (19 px, size 2 hint text, accent fill)
// ============================================================================

static M5Canvas canvas(&M5Cardputer.Display);
static bool canvasInitialized = false;

// Caret blink state
static unsigned long lastCaretToggle = 0;
static bool caretVisible = true;

// Escalate flash state
static unsigned long lastFlashTime = 0;
static bool escalateBright = true;

// ---- Fonts -----------------------------------------------------------------
// The built-in GLCD font only covers ASCII, so Chinese text sent by the daemon
// used to render as blanks. M5GFX ships efont (U8g2 bitmap fonts) covering the
// full GB2312 range and decodes UTF-8 input, so all daemon-provided content is
// drawn with it.
//
// Both the chrome and the body use a CJK font so status labels and key hints
// can be Chinese too. This costs ~500 KB of flash for the two sizes, which the
// 3 MB huge_app partition absorbs comfortably.
//
// Heights match the metrics the layout constants below were written for:
//   chrome -> efontCN_12, 12 px tall (was 8 px Font0)
//   body   -> efontCN_16, 16 px tall
static inline void useChromeFont() {
    canvas.setFont(&fonts::efontCN_12);
    canvas.setTextSize(1);
}
static inline void useBodyFont() {
    canvas.setFont(&fonts::efontCN_16);
    canvas.setTextSize(1);
}
// Large decorative ASCII text (the sleep glyph).
static inline void useDisplayFont() {
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(3);
}

// ---- UTF-8 helpers ---------------------------------------------------------
// Returns the length in bytes of the UTF-8 sequence starting at `first`.
// Invalid lead bytes report 1 so callers can always make forward progress.
static int utf8SeqLen(char first) {
    unsigned char c = (unsigned char)first;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static uint32_t utf8Decode(const char* s, int len) {
    unsigned char c0 = (unsigned char)s[0];
    switch (len) {
        case 2:
            return ((uint32_t)(c0 & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
        case 3:
            return ((uint32_t)(c0 & 0x0F) << 12) |
                   ((uint32_t)((unsigned char)s[1] & 0x3F) << 6) |
                   ((unsigned char)s[2] & 0x3F);
        case 4:
            return ((uint32_t)(c0 & 0x07) << 18) |
                   ((uint32_t)((unsigned char)s[1] & 0x3F) << 12) |
                   ((uint32_t)((unsigned char)s[2] & 0x3F) << 6) |
                   ((unsigned char)s[3] & 0x3F);
        default:
            return c0;
    }
}

// CJK / full-width codepoints may start a new line on their own. Latin words
// keep whitespace-based wrapping so they are never split mid-word.
static bool isBreakableChar(uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x11FF) ||   // Hangul Jamo
           (cp >= 0x2E80 && cp <= 0xA4CF) ||   // CJK radicals..Yi
           (cp >= 0xAC00 && cp <= 0xD7A3) ||   // Hangul syllables
           (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK compatibility
           (cp >= 0xFE30 && cp <= 0xFE4F) ||   // CJK punctuation forms
           (cp >= 0xFF00 && cp <= 0xFF60) ||   // Full-width forms
           (cp >= 0xFFE0 && cp <= 0xFFE6);
}

// Truncate `src` so it fits `maxWidth`, cutting only on UTF-8 boundaries and
// appending an ellipsis. Byte-wise truncation would split a multi-byte
// character and render mojibake.
static void truncateToWidth(char* dst, size_t dstSize, const char* src, int maxWidth) {
    if (!dst || dstSize == 0) return;
    dst[0] = '\0';
    if (!src) return;

    snprintf(dst, dstSize, "%s", src);
    if (canvas.textWidth(dst) <= maxWidth) return;

    const int ellipsisW = canvas.textWidth("...");
    size_t keep = 0;
    int width = 0;
    for (size_t i = 0; src[i];) {
        int len = utf8SeqLen(src[i]);
        for (int k = 1; k < len; k++) {
            if (!src[i + k]) { len = 1; break; }
        }
        char ch[5];
        memcpy(ch, src + i, len);
        ch[len] = '\0';
        int w = canvas.textWidth(ch);
        if (width + w + ellipsisW > maxWidth) break;
        if (keep + len + 4 >= dstSize) break;
        width += w;
        keep += len;
        i += len;
    }
    memcpy(dst, src, keep);
    dst[keep] = '\0';
    strncat(dst, "...", dstSize - keep - 1);
}

// Returns the longest suffix of `s` that fits `maxWidth`, cutting only on
// UTF-8 boundaries. The input row scrolls horizontally this way, keeping the
// caret and the most recently typed characters visible.
static const char* tailFitting(const char* s, int maxWidth) {
    if (!s) return "";
    const char* p = s;
    while (*p && canvas.textWidth(p) > maxWidth) {
        int len = utf8SeqLen(*p);
        for (int i = 1; i < len; i++) {
            if (!p[i]) { len = 1; break; }
        }
        p += len;
    }
    return p;
}

// ---- Init ------------------------------------------------------------------
void initCanvasIfNeeded() {
    // 锁定横屏画布尺寸（与 UI 布局常量 240x135 一致），
    // 避免 Display.width()/height() 在旋转状态异常时创建出错布。
    const int dw = 240;
    const int dh = 135;
    if (!canvasInitialized) {
        canvas.createSprite(dw, dh);
        useBodyFont();
        canvasInitialized = true;
    }
    // 若画布尺寸与预期不符（如历史固件残留错误），重建
    if (canvas.width() != dw || canvas.height() != dh) {
        canvas.deleteSprite();
        canvas.createSprite(dw, dh);
        useBodyFont();
    }
}

// ---- Color helpers ---------------------------------------------------------
static inline uint16_t accentBright(const UIAccent& a) {
    return canvas.color565(a.r, a.g, a.b);
}
static inline uint16_t cDim()   { return canvas.color565(0x80, 0x80, 0x80); }
static inline uint16_t cMid()   { return canvas.color565(0xB0, 0xB0, 0xB0); }
static inline uint16_t cMuted() { return canvas.color565(0x40, 0x40, 0x40); }

// Layout constants (Bruce-style, sized for a 16 px body font)
#define UI_STATUS_H        14
#define UI_HAIR1_Y         14
#define UI_BODY_Y          17
#define UI_BODY_BOTTOM_Y   96    // when input row shown
#define UI_BODY_BOTTOM_NOI 113   // no input row
#define UI_HAIR2_Y         97
#define UI_INPUT_Y         100
#define UI_HAIR3_Y         115
#define UI_FOOTER_Y        117
#define UI_FOOTER_H        18
#define UI_PAD             14
#define UI_LINE_GAP        3   // extra px between wrapped body lines
#define UI_MAX_OPTIONS     6
#define UI_OPTION_GAP      6   // vertical space between option blocks
#define UI_OPTION_INDENT   22  // x offset of option text (past index + rule)

// Body geometry shared by every screen; kept in one place so the measuring and
// drawing paths can never disagree about the available width.
static inline int bodyLeftX()  { return UI_PAD; }
static inline int bodyRightX() { return 240 - UI_PAD - 10; } // scroll gutter
static inline int optionTextWidth() {
    return bodyRightX() - (bodyLeftX() + UI_OPTION_INDENT);
}

// ---- Battery ---------------------------------------------------------------
// The Cardputer reads its battery through an ADC, and IDLE repaints twice per
// second, so the level is cached and only refreshed periodically.
static int batteryLevel = -1;
static bool batteryCharging = false;
static unsigned long lastBatteryPoll = 0;
static constexpr unsigned long BATTERY_POLL_MS = 10000;

static void pollBattery() {
    unsigned long now = millis();
    if (batteryLevel >= 0 && now - lastBatteryPoll < BATTERY_POLL_MS) return;
    lastBatteryPoll = now;

    int level = (int)M5Cardputer.Power.getBatteryLevel();
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    batteryLevel = level;
    batteryCharging = (M5Cardputer.Power.isCharging() == m5::Power_Class::is_charging);
}

static uint16_t batteryColor() {
    if (batteryCharging) return canvas.color565(0x00, 0xCC, 0x66);
    if (batteryLevel <= 15) return canvas.color565(0xCC, 0x40, 0x40);
    if (batteryLevel <= 35) return canvas.color565(0xFF, 0xB0, 0x00);
    return cMid();
}

// Draws "<pct>% [icon]" right-aligned at `rightX`.
// Returns the x of its left edge so the caller can place the link indicator
// next to it without overlapping.
static int drawBatteryIndicator(int rightX, int centerY) {
    pollBattery();

    const int bodyW = 15, bodyH = 9, tipW = 2;
    int iconX = rightX - (bodyW + tipW);
    int iconY = centerY - bodyH / 2;
    uint16_t col = batteryColor();

    canvas.drawRect(iconX, iconY, bodyW, bodyH, col);
    canvas.fillRect(iconX + bodyW, iconY + 2, tipW, bodyH - 4, col);
    int fillW = ((bodyW - 4) * batteryLevel) / 100;
    if (fillW > 0) canvas.fillRect(iconX + 2, iconY + 2, fillW, bodyH - 4, col);
    if (batteryCharging) {
        // A knocked-out dot marks "charging"; a bolt glyph is unreadable here.
        canvas.fillCircle(iconX + bodyW / 2, centerY, 1, UI_RGB_BG);
    }

    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", batteryLevel);
    canvas.setTextColor(col);
    canvas.setTextDatum(middle_right);
    canvas.drawString(pct, iconX - 3, centerY);

    return iconX - 3 - canvas.textWidth(pct);
}

// ---- Status bar (top, chrome font) -----------------------------------------
static void drawStatusBar(const char* label, const UIAccent& acc, bool online) {
    canvas.fillRect(0, 0, 240, UI_STATUS_H, UI_RGB_BG);
    useChromeFont();

    int midY = UI_STATUS_H / 2;

    canvas.setTextColor(accentBright(acc));
    canvas.setTextDatum(middle_left);
    canvas.drawString(label ? label : "", UI_PAD, midY);

    int batteryLeftX = drawBatteryIndicator(240 - 6, midY);

    int dotX = batteryLeftX - 9;
    if (online) {
        canvas.fillCircle(dotX, midY, 2, canvas.color565(0x00, 0xCC, 0x66));
    } else {
        canvas.drawCircle(dotX, midY, 2, canvas.color565(0xCC, 0x40, 0x40));
    }
    canvas.setTextColor(cDim());
    canvas.setTextDatum(middle_right);
    canvas.drawString(online ? L("在线", "ON") : L("离线", "OFF"), dotX - 6, midY);

    canvas.drawLine(0, UI_HAIR1_Y, 240, UI_HAIR1_Y, cMuted());
}

// ---- Footer (size 2, accent fill) ------------------------------------------
static void drawFooter3(const char* left, const char* center, const char* right,
                        const UIAccent& acc) {
    canvas.drawLine(0, UI_HAIR3_Y, 240, UI_HAIR3_Y, cMuted());
    canvas.fillRect(0, UI_FOOTER_Y, 240, UI_FOOTER_H, accentBright(acc));
    useChromeFont();
    canvas.setTextColor(UI_RGB_BG);
    int midY = UI_FOOTER_Y + UI_FOOTER_H / 2;

    if (left && left[0]) {
        canvas.setTextDatum(middle_left);
        canvas.drawString(left, UI_PAD + 2, midY);
    }
    if (center && center[0]) {
        canvas.setTextDatum(middle_center);
        canvas.drawString(center, 120, midY);
    }
    if (right && right[0]) {
        canvas.setTextDatum(middle_right);
        canvas.drawString(right, 240 - UI_PAD - 2, midY);
    }
}

// Big-text action footer for screens where the keys ARE the choice (CONFIRM).
// Taller band, size-2 text, accent fill, black text.
#define UI_BIG_FOOTER_H 25
#define UI_BIG_FOOTER_Y (135 - UI_BIG_FOOTER_H)
static void drawBigFooter3(const char* left, const char* center, const char* right,
                            const UIAccent& acc) {
    canvas.drawLine(0, UI_BIG_FOOTER_Y - 1, 240, UI_BIG_FOOTER_Y - 1, cMuted());
    canvas.fillRect(0, UI_BIG_FOOTER_Y, 240, UI_BIG_FOOTER_H, accentBright(acc));
    useBodyFont();
    canvas.setTextColor(UI_RGB_BG);
    int midY = UI_BIG_FOOTER_Y + UI_BIG_FOOTER_H / 2;

    if (left && left[0]) {
        canvas.setTextDatum(middle_left);
        canvas.drawString(left, UI_PAD + 2, midY);
    }
    if (center && center[0]) {
        canvas.setTextDatum(middle_center);
        canvas.drawString(center, 120, midY);
    }
    if (right && right[0]) {
        canvas.setTextDatum(middle_right);
        canvas.drawString(right, 240 - UI_PAD - 2, midY);
    }
}

static void drawFooterDim(const char* text) {
    canvas.drawLine(0, UI_HAIR3_Y, 240, UI_HAIR3_Y, cMuted());
    useChromeFont();
    canvas.setTextColor(cMid());
    canvas.setTextDatum(middle_center);
    canvas.drawString(text ? text : "", 120, UI_FOOTER_Y + UI_FOOTER_H / 2);
}

// ---- Caret (size 2 height = 16 px) -----------------------------------------
static void drawCaret(int x, int y) {
    unsigned long now = millis();
    if (now - lastCaretToggle > 500) {
        lastCaretToggle = now;
        caretVisible = !caretVisible;
    }
    if (caretVisible) {
        canvas.fillRect(x, y, 2, 14, UI_RGB_FG);
    }
}

// ---- Word-wrap (uses current font via fontHeight) --------------------------
// layoutWrapped walks `text` once and reports every token together with the
// position it should be drawn at, relative to y = 0. Measuring and drawing
// share this single implementation so they can never disagree.
//
// Wrapping is UTF-8 aware: CJK characters become individual tokens (they carry
// no spaces and may break anywhere), while Latin words stay intact.
// Returns the y offset just past the last line.
template <typename Emit>
static int layoutWrapped(const char* text, int x, int maxWidth, Emit emit) {
    const int lineHeight = canvas.fontHeight() + UI_LINE_GAP;
    if (!text || !text[0]) return lineHeight;

    const int spaceWidth = canvas.textWidth(" ");
    int cursorX = x;
    int cursorY = 0;

    char token[96];
    int tokenLen = 0;

    // Places the pending token, wrapping first if it no longer fits.
    auto flush = [&](bool trailingSpace) {
        if (tokenLen > 0) {
            token[tokenLen] = '\0';
            int w = canvas.textWidth(token);
            if (cursorX + w > x + maxWidth && cursorX > x) {
                cursorX = x;
                cursorY += lineHeight;
            }
            emit(token, cursorX, cursorY);
            cursorX += w;
            tokenLen = 0;
        }
        if (trailingSpace && cursorX > x) cursorX += spaceWidth;
    };

    const char* p = text;
    while (*p) {
        if (*p == '\n') {
            flush(false);
            cursorX = x;
            cursorY += lineHeight;
            p++;
            continue;
        }
        if (*p == ' ' || *p == '\t' || *p == '\r') {
            flush(true);
            p++;
            continue;
        }

        int len = utf8SeqLen(*p);
        // Guard against a sequence truncated by the end of the string.
        for (int i = 1; i < len; i++) {
            if (!p[i]) { len = 1; break; }
        }

        if (isBreakableChar(utf8Decode(p, len))) {
            flush(false);              // wide character stands alone
            memcpy(token, p, len);
            tokenLen = len;
            flush(false);
        } else if (tokenLen + len < (int)sizeof(token) - 1) {
            memcpy(token + tokenLen, p, len);
            tokenLen += len;
        } else {
            flush(false);              // pathologically long word: hard-break
            memcpy(token, p, len);
            tokenLen = len;
        }
        p += len;
    }
    flush(false);

    return cursorY + lineHeight;
}

int drawWordWrappedColored(const char* text, int x, int y, int maxWidth,
                            uint16_t color, int scrollY, int clipTop, int clipBottom) {
    if (!text) return y;
    canvas.setTextColor(color);
    canvas.setTextDatum(top_left);

    const int lineHeight = canvas.fontHeight() + UI_LINE_GAP;
    const int baseY = y - scrollY;

    int consumed = layoutWrapped(text, x, maxWidth,
        [&](const char* tok, int tokenX, int tokenY) {
            int drawY = baseY + tokenY;
            if (drawY + lineHeight > clipTop && drawY < clipBottom) {
                canvas.drawString(tok, tokenX, drawY);
            }
        });

    return y + consumed;
}

int measureWordWrappedHeight(const char* text, int x, int maxWidth) {
    if (!text || text[0] == '\0') return 0;
    return layoutWrapped(text, x, maxWidth, [](const char*, int, int) {});
}

int uiBodyLineHeight() {
    initCanvasIfNeeded();
    useBodyFont();
    return canvas.fontHeight() + UI_LINE_GAP;
}

// Hardware clip helpers — prevent text bleed into status bar / footer.
static inline void clipToBody(int top, int bottom) {
    canvas.setClipRect(0, top, 240, bottom - top + 1);
}
static inline void clearClip() {
    canvas.clearClipRect();
}

// Body viewport for each screen type — used by computeMaxScroll
static inline void bodyViewportForType(const char* type, int& bodyTop, int& bodyBottom) {
    bodyTop = UI_BODY_Y;
    if (!type) { bodyBottom = UI_BODY_BOTTOM_NOI; return; }
    if (!strcmp(type, "ask") || !strcmp(type, "escalate")) {
        bodyBottom = UI_BODY_BOTTOM_Y;  // input row below
    } else if (!strcmp(type, "confirm")) {
        bodyBottom = UI_BIG_FOOTER_Y - 2;  // big action footer below
    } else {
        bodyBottom = UI_BODY_BOTTOM_NOI;  // choose, settings, etc
    }
}

int computeMaxScroll(const char* type, const char* question, const char* context,
                      const String options[], int optionCount) {
    initCanvasIfNeeded();
    useBodyFont();

    int bodyTop, bodyBottom;
    bodyViewportForType(type, bodyTop, bodyBottom);
    int bodyMaxW = 240 - 2 * UI_PAD - 10;  // gutter for scroll triangles
    int viewportH = bodyBottom - bodyTop;

    int contentH = 0;
    if (question && question[0]) {
        contentH += measureWordWrappedHeight(question, 0, bodyMaxW);
    }
    if (context && context[0]) {
        contentH += 6;
        contentH += measureWordWrappedHeight(context, 0, bodyMaxW);
    }
    if (type && !strcmp(type, "choose")) {
        contentH += 6;  // gap before options
        // Options wrap over as many lines as they need, so their height must be
        // measured with the same width the renderer uses, or scrolling would
        // stop before the last line of a long option.
        int optionW = optionTextWidth();
        for (int i = 0; i < optionCount && i < UI_MAX_OPTIONS; i++) {
            contentH += measureWordWrappedHeight(options[i].c_str(), 0, optionW);
            contentH += UI_OPTION_GAP;
        }
    }

    int maxScroll = contentH - viewportH;
    return maxScroll > 0 ? maxScroll : 0;
}

// Scroll triangles in body's right gutter
static void drawScrollHints(int scrollY, int contentHeight, int viewportHeight,
                             int viewTop, int viewBottom) {
    if (contentHeight <= viewportHeight) return;
    uint16_t mark = cMid();
    if (scrollY > 0) {
        canvas.fillTriangle(228, viewTop + 5, 236, viewTop + 5,
                            232, viewTop, mark);
    }
    if (scrollY < contentHeight - viewportHeight) {
        canvas.fillTriangle(228, viewBottom - 5, 236, viewBottom - 5,
                            232, viewBottom, mark);
    }
}

// ============================================================================
// IDLE
// ============================================================================
void drawIdleScreen(const char* version, const char* ip, bool showSetupHint) {
    initCanvasIfNeeded();
    canvas.fillSprite(UI_RGB_BG);

    bool connecting = version && strstr(version, "Connecting") != nullptr;
    drawStatusBar(connecting ? L("连接中", "CONNECT") : L("待机", "IDLE"), ACC_IDLE, !connecting);

    // Small label (chrome, dim) — idle is not important
    useChromeFont();
    canvas.setTextColor(cDim());
    canvas.setTextDatum(middle_center);
    canvas.drawString("ask-master", 120, UI_BODY_Y + 12);

    // Status (body font, prominent — primary content)
    useBodyFont();
    canvas.setTextColor(UI_RGB_FG);
    canvas.drawString(connecting ? L("连接中", "connecting") : L("就绪", "ready"), 120, UI_BODY_Y + 38);

    // Link info (chrome, dim)
    if (ip && ip[0]) {
        useChromeFont();
        canvas.setTextColor(cMid());
        canvas.drawString(ip, 120, UI_BODY_Y + 64);
    }

    // Version (chrome, dim)
    if (version && !connecting) {
        useChromeFont();
        canvas.setTextColor(cDim());
        canvas.drawString(version, 120, UI_BODY_Y + 82);
    }

    drawFooterDim(showSetupHint ? L("[S] 设备信息", "[S] Info") : "");

    canvas.pushSprite(0, 0);
}

// ============================================================================
// STANDBY（统一待机界面：未连接 / 已连接共用同一界面）
// ============================================================================
// 把 bytes/s 格式化为可读字符串（如 "1.2MB/s" / "500KB/s"）。
static String formatRate(int bytesPerSec) {
    if (bytesPerSec < 0) {
        return String("--");
    }
    char buf[20];
    if (bytesPerSec >= 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1fMB/s", bytesPerSec / (1024.0f * 1024.0f));
    } else if (bytesPerSec >= 1024) {
        snprintf(buf, sizeof(buf), "%.0fKB/s", bytesPerSec / 1024.0f);
    } else {
        snprintf(buf, sizeof(buf), "%dB/s", bytesPerSec);
    }
    return String(buf);
}

void drawStandbyScreen(bool connected) {
    initCanvasIfNeeded();
    canvas.fillSprite(UI_RGB_BG);

    // 状态栏统一为「待机」，圆点/在线文字随连接状态变化。
    drawStatusBar(L("待机", "IDLE"), ACC_IDLE, connected);

    useDisplayFont();
    canvas.setTextColor(cMid());
    canvas.setTextDatum(middle_center);

    if (connected) {
        // 已连接：显示就绪 + 性能监控数据（若有）
        useChromeFont();
        canvas.setTextColor(cDim());
        canvas.drawString("ask-master", 120, UI_BODY_Y + 8);

        if (hasMetrics) {
            useBodyFont();
            canvas.setTextColor(UI_RGB_FG);

            // 第一行：CPU / GPU
            char line1[48];
            if (pcGpu >= 0) {
                snprintf(line1, sizeof(line1), "CPU %d%%  GPU %d%%", pcCpu, pcGpu);
            } else {
                snprintf(line1, sizeof(line1), "CPU %d%%", pcCpu);
            }
            canvas.drawString(line1, 120, UI_BODY_Y + 32);

            // 第二行：内存
            char line2[32];
            snprintf(line2, sizeof(line2), L("内存 %d%%", "MEM %d%%"), pcMem);
            canvas.drawString(line2, 120, UI_BODY_Y + 52);

            // 第三行：网速（下载/上传）
            String line3 = String(L("下", "D")) + " " + formatRate(pcNetDn) +
                           "  " + String(L("上", "U")) + " " + formatRate(pcNetUp);
            canvas.drawString(line3.c_str(), 120, UI_BODY_Y + 72);
        } else {
            useBodyFont();
            canvas.setTextColor(UI_RGB_FG);
            canvas.drawString(L("就绪", "ready"), 120, UI_BODY_Y + 38);
        }
    } else {
        // 未连接：等待蓝牙连接
        canvas.drawString("zZz", 120, UI_BODY_Y + 20);

        useChromeFont();
        canvas.setTextColor(cDim());
        canvas.drawString(L("等待蓝牙连接", "waiting for BLE"), 120, UI_BODY_Y + 58);
    }

    drawFooterDim(L("[S] 设备信息  [L] 语言", "[S] Info  [L] Lang"));

    canvas.pushSprite(0, 0);
}

// ============================================================================
// SLEEP（向后兼容：统一待机界面的未连接分支）
// ============================================================================
void drawSleepScreen() {
    drawStandbyScreen(false);
}

// ============================================================================
// ASK / ESCALATE — shared free-text input layout
// ============================================================================
static void drawQuestionScreen(const char* stateLabel, const UIAccent& acc,
                                const char* question, const char* context,
                                const char* inputBuffer, int scrollY,
                                bool escalateFlash,
                                const char* composing, const char* cands,
                                bool imePinyinMode) {
    initCanvasIfNeeded();
    canvas.fillSprite(UI_RGB_BG);

    UIAccent renderAcc = acc;
    if (escalateFlash) {
        unsigned long now = millis();
        if (now - lastFlashTime > 200) {
            lastFlashTime = now;
            escalateBright = !escalateBright;
        }
        if (!escalateBright) {
            renderAcc.r = (acc.r * 2) / 5;
            renderAcc.g = (acc.g * 2) / 5;
            renderAcc.b = (acc.b * 2) / 5;
        }
    }

    drawStatusBar(stateLabel, renderAcc, true);

    int bodyLeft = UI_PAD;
    int bodyRight = 240 - UI_PAD - 10; // leave gutter for scroll triangles
    int bodyMaxW = bodyRight - bodyLeft;
    int bodyTop = UI_BODY_Y;
    int bodyBottom = UI_BODY_BOTTOM_Y;

    // Body text — hardware-clip to prevent bleed into status/input
    useBodyFont();
    clipToBody(bodyTop, bodyBottom);
    int y = bodyTop;
    if (question) {
        y = drawWordWrappedColored(question, bodyLeft, y, bodyMaxW,
                                    UI_RGB_FG, scrollY, bodyTop, bodyBottom);
    }
    if (context && context[0] != '\0') {
        y += 4;
        y = drawWordWrappedColored(context, bodyLeft, y, bodyMaxW,
                                    cMid(), scrollY, bodyTop, bodyBottom);
    }
    clearClip();

    int contentH = y - bodyTop;
    int viewportH = bodyBottom - bodyTop;
    drawScrollHints(scrollY, contentH, viewportH, bodyTop, bodyBottom);

    // Input row
    canvas.drawLine(0, UI_HAIR2_Y, 240, UI_HAIR2_Y, cMuted());

    // Pinyin IME candidate bar: shown only while composing. It sits between the
    // hairline and the input row, shrinking the body by a few pixels.
    int inputY = UI_INPUT_Y;
    if (composing && composing[0]) {
        const int imeY = UI_HAIR2_Y + 2;
        const int imeH = 14;
        canvas.fillRect(0, UI_HAIR2_Y + 1, 240, imeH, canvas.color565(0x1a, 0x1a, 0x22));
        useChromeFont();
        // Show the composing pinyin in accent, then numbered candidates.
        canvas.setTextColor(accentBright(renderAcc));
        canvas.setTextDatum(top_left);
        char hdr[12];
        snprintf(hdr, sizeof(hdr), "%s|", composing);
        canvas.drawString(hdr, UI_PAD, imeY + 1);
        int x = UI_PAD + canvas.textWidth(hdr) + 2;
        // Candidates string is space-separated; each token is one candidate.
        const char* p = cands;
        int idx = 1;
        while (*p && idx <= 9 && x < 240 - UI_PAD - 8) {
            // Extract one space-separated token.
            char tok[32];
            int tlen = 0;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && tlen < (int)sizeof(tok) - 1) {
                tok[tlen++] = *p++;
            }
            if (tlen == 0) break;
            tok[tlen] = '\0';

            char num[4];
            snprintf(num, sizeof(num), "%d", idx);
            canvas.setTextColor(cMid());
            canvas.drawString(num, x, imeY + 1);
            canvas.setTextColor(UI_RGB_FG);
            canvas.drawString(tok, x + canvas.textWidth(num), imeY + 1);
            x += canvas.textWidth(num) + canvas.textWidth(tok) + 4;
            idx++;
        }
        // Shift the input row down so it doesn't overlap the IME bar.
        inputY = UI_HAIR2_Y + imeH + 3;
        useBodyFont();
    }

    useBodyFont();
    canvas.setTextColor(accentBright(renderAcc));
    canvas.setTextDatum(top_left);
    canvas.setCursor(UI_PAD, inputY);
    canvas.print("> ");

    canvas.setTextColor(UI_RGB_FG);
    int inputX = UI_PAD + canvas.textWidth("> ");
    const char* visible = tailFitting(inputBuffer ? inputBuffer : "",
                                     240 - UI_PAD - inputX - 4);
    canvas.drawString(visible, inputX, inputY);

    int caretX = inputX + canvas.textWidth(visible) + 1;
    drawCaret(caretX, inputY);

    drawFooter3("回车发送", imePinyinMode ? "中文 opt切英" : "英文 opt切中", "ESC", renderAcc);

    canvas.pushSprite(0, 0);
}

void drawAskScreen(const char* question, const char* context, const char* inputBuffer, int scrollY) {
    drawQuestionScreen(L("提问", "ASK"), ACC_ASK, question, context, inputBuffer, scrollY, false, nullptr, nullptr, true);
}

void drawEscalateScreen(const char* question, const char* context, const char* inputBuffer, int scrollY) {
    drawQuestionScreen(L("紧急", "ALERT"), ACC_ESCALATE, question, context, inputBuffer, scrollY, true, nullptr, nullptr, true);
}

void drawAskScreenIME(const char* question, const char* context,
                      const char* inputBuffer, int scrollY,
                      const char* composing, const char* cands,
                      bool pinyinMode) {
    drawQuestionScreen(L("提问", "ASK"), ACC_ASK, question, context, inputBuffer, scrollY,
                       false, composing, cands, pinyinMode);
}

void drawEscalateScreenIME(const char* question, const char* context,
                           const char* inputBuffer, int scrollY,
                           const char* composing, const char* cands,
                           bool pinyinMode) {
    drawQuestionScreen(L("紧急", "ALERT"), ACC_ESCALATE, question, context, inputBuffer, scrollY,
                       true, composing, cands, pinyinMode);
}

// ============================================================================
// CONFIRM
// ============================================================================
void drawConfirmScreen(const char* statement, const char* consequence, int scrollY) {
    initCanvasIfNeeded();
    canvas.fillSprite(UI_RGB_BG);

    drawStatusBar(L("确认", "CONFIRM"), ACC_CONFIRM, true);

    int bodyLeft = UI_PAD;
    int bodyRight = 240 - UI_PAD - 10;
    int bodyMaxW = bodyRight - bodyLeft;
    int bodyTop = UI_BODY_Y;
    int bodyBottom = UI_BIG_FOOTER_Y - 2; // make room for big action footer

    useBodyFont();
    clipToBody(bodyTop, bodyBottom);
    int y = bodyTop;
    if (statement) {
        y = drawWordWrappedColored(statement, bodyLeft, y, bodyMaxW,
                                    UI_RGB_FG, scrollY, bodyTop, bodyBottom);
    }
    if (consequence && consequence[0] != '\0') {
        y += 6;
        y = drawWordWrappedColored(consequence, bodyLeft, y, bodyMaxW,
                                    cMid(), scrollY, bodyTop, bodyBottom);
    }
    clearClip();

    int contentH = y - bodyTop;
    int viewportH = bodyBottom - bodyTop;
    drawScrollHints(scrollY, contentH, viewportH, bodyTop, bodyBottom);

    // Big action footer — Y/N are CONTENT user must read + act on
    drawBigFooter3(L("[Y] 确认", "[Y] YES"), "", L("[N] 拒绝", "[N] NO"), ACC_CONFIRM);

    canvas.pushSprite(0, 0);
}

// ============================================================================
// CHOOSE — numbered list; each option wraps over as many lines as it needs
// ============================================================================
void drawChooseScreen(const char* question, const char* context, const String options[], int optionCount, int scrollY) {
    initCanvasIfNeeded();
    canvas.fillSprite(UI_RGB_BG);

    drawStatusBar(L("选择", "CHOOSE"), ACC_CHOOSE, true);

    int bodyLeft = bodyLeftX();
    int bodyMaxW = bodyRightX() - bodyLeft;
    int bodyTop = UI_BODY_Y;
    int bodyBottom = UI_BODY_BOTTOM_NOI;

    // Question header — clip to body
    useBodyFont();
    clipToBody(bodyTop, bodyBottom);
    int qLineY = drawWordWrappedColored(question ? question : "", bodyLeft, bodyTop, bodyMaxW,
                                        UI_RGB_FG, scrollY, bodyTop, bodyBottom);
    int optionsTop = qLineY + 6;

    // Options: wrapped instead of truncated, so a long label stays readable in
    // full; the body scrolls (;/.) when the list outgrows the viewport.
    useBodyFont();
    int optionX = bodyLeft + UI_OPTION_INDENT;
    int optionW = optionTextWidth();
    int lineHeight = canvas.fontHeight() + UI_LINE_GAP;

    int y = optionsTop;
    for (int i = 0; i < optionCount && i < UI_MAX_OPTIONS; i++) {
        int blockH = measureWordWrappedHeight(options[i].c_str(), 0, optionW);
        int screenY = y - scrollY;

        // Skip blocks scrolled fully out of view, but keep advancing y.
        if (screenY + blockH > bodyTop && screenY < bodyBottom) {
            // Index in accent, aligned with the option's first line
            canvas.setTextColor(accentBright(ACC_CHOOSE));
            canvas.setTextDatum(top_left);
            canvas.setCursor(bodyLeft + 2, screenY);
            canvas.printf("%d", i + 1);

            // Vertical rule spans the whole (possibly multi-line) block
            canvas.drawLine(bodyLeft + 18, screenY - 1,
                            bodyLeft + 18, screenY + blockH - lineHeight + canvas.fontHeight(),
                            cMuted());

            drawWordWrappedColored(options[i].c_str(), optionX, y, optionW,
                                   UI_RGB_FG, scrollY, bodyTop, bodyBottom);
        }

        y += blockH + UI_OPTION_GAP;
    }
    clearClip();

    int totalH = y - bodyTop;
    int viewportH = bodyBottom - bodyTop;
    drawScrollHints(scrollY, totalH, viewportH, bodyTop, bodyBottom);

    drawFooter3(L("1-6 选择", "1-6 pick"), L("上下 滚动", "scroll"), "ESC", ACC_CHOOSE);

    canvas.pushSprite(0, 0);
}

// ============================================================================
// RECORDING（P1 语音输入）
// ============================================================================
void drawRecordingScreen(unsigned long elapsedMs) {
    initCanvasIfNeeded();
    canvas.fillSprite(UI_RGB_BG);

    // 录音只在 BLE 已连接时发生，状态栏应显示「在线」而非「离线」。
    drawStatusBar(L("语音输入", "VOICE"), ACC_ESCALATE, true);

    int cx = 120;
    int cy = 54;

    // 红色录音指示圆点
    canvas.fillCircle(cx, cy, 8, TFT_RED);
    canvas.drawCircle(cx, cy, 12, TFT_RED);

    char buf[32];
    snprintf(buf, sizeof(buf), "%lu.%02lu s", elapsedMs / 1000, (elapsedMs % 1000) / 10);
    useChromeFont();
    canvas.setTextColor(UI_RGB_FG);
    canvas.setTextDatum(middle_center);
    canvas.drawString(buf, cx, cy + 28);
    canvas.drawString(L("说话中…", "Recording…"), cx, cy + 46);

    drawFooter3(L("松开 Ctrl 发送", "release Ctrl"), "", "30s max", ACC_ESCALATE);

    canvas.pushSprite(0, 0);
}

// ============================================================================
// SETUP
// ============================================================================
void drawSetupScreen(const char* label, const char* context, const char* inputBuffer) {
    initCanvasIfNeeded();
    canvas.fillSprite(UI_RGB_BG);

    drawStatusBar("SETUP", ACC_SETUP, false);

    int bodyLeft = UI_PAD;
    int bodyRight = 240 - UI_PAD - 10;
    int bodyMaxW = bodyRight - bodyLeft;
    int bodyTop = UI_BODY_Y;

    useBodyFont();
    int y = bodyTop;
    if (label) {
        canvas.setTextColor(accentBright(ACC_SETUP));
        canvas.setTextDatum(top_left);
        y = drawWordWrappedColored(label, bodyLeft, y, bodyMaxW,
                                    accentBright(ACC_SETUP), 0, bodyTop, UI_BODY_BOTTOM_Y);
    }
    if (context && context[0] != '\0') {
        y += 4;
        useChromeFont();
        y = drawWordWrappedColored(context, bodyLeft, y, bodyMaxW,
                                    cMid(), 0, bodyTop, UI_BODY_BOTTOM_Y);
    }

    // Input row
    canvas.drawLine(0, UI_HAIR2_Y, 240, UI_HAIR2_Y, cMuted());
    useBodyFont();
    canvas.setTextColor(accentBright(ACC_SETUP));
    canvas.setTextDatum(top_left);
    canvas.setCursor(UI_PAD, UI_INPUT_Y);
    canvas.print("> ");

    canvas.setTextColor(UI_RGB_FG);
    int inputX = UI_PAD + canvas.textWidth("> ");
    const char* visible = tailFitting(inputBuffer ? inputBuffer : "",
                                     240 - UI_PAD - inputX - 4);
    canvas.drawString(visible, inputX, UI_INPUT_Y);

    int caretX = inputX + canvas.textWidth(visible) + 1;
    drawCaret(caretX, UI_INPUT_Y);

    drawFooter3("ENTER", "", "ESC back", ACC_SETUP);

    canvas.pushSprite(0, 0);
}

void drawSetupSummaryScreen(const char* ssid, const char* serverIP, uint16_t port) {
    initCanvasIfNeeded();
    canvas.fillSprite(UI_RGB_BG);

    drawStatusBar("SETUP", ACC_SETUP, false);

    useBodyFont();
    int bodyLeft = UI_PAD;
    int y = UI_BODY_Y;

    canvas.setTextColor(accentBright(ACC_SETUP));
    canvas.setTextDatum(top_left);
    canvas.drawString("Review", bodyLeft, y);
    y += canvas.fontHeight() + 6;

    useChromeFont();
    canvas.setTextColor(cMid());
    canvas.drawString("WiFi", bodyLeft, y);
    canvas.setTextColor(UI_RGB_FG);
    canvas.drawString(ssid ? ssid : "-", bodyLeft + 40, y);

    y += canvas.fontHeight() + 4;
    canvas.setTextColor(cMid());
    canvas.drawString("Server", bodyLeft, y);
    canvas.setTextColor(UI_RGB_FG);
    char serverInfo[40];
    snprintf(serverInfo, sizeof(serverInfo), "%s:%d", serverIP ? serverIP : "-", port);
    canvas.drawString(serverInfo, bodyLeft + 40, y);

    drawFooter3("[Y] Save", "", "[N] Retry", ACC_SETUP);

    canvas.pushSprite(0, 0);
}

// ============================================================================
// SETTINGS
// ============================================================================
void drawSettingsMenuScreen(bool hasConfig, const char* currentSSID, const char* currentServer) {
    initCanvasIfNeeded();
    canvas.fillSprite(UI_RGB_BG);

    drawStatusBar("SETTINGS", ACC_SETUP, true);

    int bodyLeft = UI_PAD;
    int y = UI_BODY_Y;
    useBodyFont();
    int rowH = canvas.fontHeight() + 4;

    struct { const char* key; const char* label; } items[] = {
        { "1", "WiFi" },
        { "2", "Server" },
        { "3", "Reset" }
    };

    for (int i = 0; i < 3; i++) {
        canvas.setTextColor(accentBright(ACC_SETUP));
        canvas.setTextDatum(top_left);
        canvas.setCursor(bodyLeft + 2, y);
        canvas.print(items[i].key);
        canvas.drawLine(bodyLeft + 18, y - 1, bodyLeft + 18, y + rowH - 3, cMuted());
        canvas.setTextColor(UI_RGB_FG);
        canvas.drawString(items[i].label, bodyLeft + 22, y);
        y += rowH;
    }

    if (hasConfig && currentSSID && currentServer) {
        y += 4;
        canvas.drawLine(bodyLeft, y - 3, 240 - UI_PAD, y - 3, cMuted());
        useChromeFont();
        canvas.setTextColor(cDim());
        canvas.setCursor(bodyLeft, y);
        canvas.printf("Now: %s", currentSSID);
    }

    drawFooter3("1-3 pick", "", "ESC", ACC_SETUP);

    canvas.pushSprite(0, 0);
}

// ============================================================================
// WiFi / Server lists
// ============================================================================
static void drawNetworkListBody(const String networks[], int networkCount,
                                 const int8_t* rssi) {
    useBodyFont();
    int rowH = canvas.fontHeight() + 4;
    int listTop = UI_BODY_Y;
    int listLeft = UI_PAD;
    int listRight = 240 - UI_PAD;
    int rssiW = rssi ? 50 : 0; // narrow width at size 2
    int nameMaxX = listRight - rssiW;

    for (int i = 0; i < networkCount && i < 5; i++) {
        int y = listTop + i * rowH;
        if (y + rowH > UI_BODY_BOTTOM_NOI) break;

        canvas.setTextColor(accentBright(ACC_SETUP));
        canvas.setTextDatum(top_left);
        canvas.setCursor(listLeft + 2, y);
        canvas.printf("%d", i + 1);

        canvas.drawLine(listLeft + 18, y - 1, listLeft + 18, y + rowH - 3, cMuted());

        canvas.setTextColor(UI_RGB_FG);
        char buf[96];
        truncateToWidth(buf, sizeof(buf), networks[i].c_str(),
                        nameMaxX - (listLeft + 22));
        canvas.drawString(buf, listLeft + 22, y);

        if (rssi) {
            int8_t signal = rssi[i];
            uint16_t col = canvas.color565(0x00, 0xCC, 0x66);
            if (signal < -70) col = canvas.color565(0xCC, 0x40, 0x40);
            else if (signal < -60) col = canvas.color565(0xFF, 0xB0, 0x00);
            useChromeFont();
            canvas.setTextColor(col);
            canvas.setTextDatum(top_right);
            char rssiBuf[10];
            snprintf(rssiBuf, sizeof(rssiBuf), "%d", signal);
            canvas.drawString(rssiBuf, listRight - 1, y + 4);
            useBodyFont();
        }
    }
}

void drawNetworkListScreen(const String networks[], int networkCount, int8_t rssi[]) {
    initCanvasIfNeeded();
    canvas.fillSprite(UI_RGB_BG);

    drawStatusBar("WiFi", ACC_SETUP, false);
    drawNetworkListBody(networks, networkCount, rssi);
    drawFooter3("1-5 pick", "", "[R] Rescan", ACC_SETUP);

    canvas.pushSprite(0, 0);
}

void drawWiFiSelectScreen(const String networks[], int networkCount, int8_t rssi[], bool showSaved) {
    initCanvasIfNeeded();
    canvas.fillSprite(UI_RGB_BG);

    drawStatusBar(showSaved ? "WiFi (saved)" : "WiFi", ACC_SETUP, false);
    drawNetworkListBody(networks, networkCount, showSaved ? nullptr : rssi);
    drawFooter3("1-5 pick", "", showSaved ? "[N] New" : "[R] Rescan", ACC_SETUP);

    canvas.pushSprite(0, 0);
}

void drawServerSelectScreen(const char* ips[], int ports[], int count) {
    initCanvasIfNeeded();
    canvas.fillSprite(UI_RGB_BG);

    drawStatusBar("Server", ACC_SETUP, false);

    useBodyFont();
    int rowH = canvas.fontHeight() + 4;
    int listTop = UI_BODY_Y;
    int listLeft = UI_PAD;

    for (int i = 0; i < count && i < 5; i++) {
        int y = listTop + i * rowH;
        if (y + rowH > UI_BODY_BOTTOM_NOI) break;

        canvas.setTextColor(accentBright(ACC_SETUP));
        canvas.setTextDatum(top_left);
        canvas.setCursor(listLeft + 2, y);
        canvas.printf("%d", i + 1);
        canvas.drawLine(listLeft + 18, y - 1, listLeft + 18, y + rowH - 3, cMuted());

        canvas.setTextColor(UI_RGB_FG);
        canvas.setCursor(listLeft + 22, y);
        canvas.printf("%s:%d", ips[i], ports[i]);
    }

    drawFooter3("1-5 pick", "", "[N] New", ACC_SETUP);

    canvas.pushSprite(0, 0);
}
