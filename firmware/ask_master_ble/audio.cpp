#include "audio.h"
#include <M5Cardputer.h>

#ifdef DEBUG_SERIAL
  #define DBG(...) Serial.println(__VA_ARGS__)
#else
  #define DBG(...)
#endif

// ---------------------------------------------------------------------------
// IMA-ADPCM 编码器（标准 IMA，4-bit/sample，4:1 压缩）
// 打包方式：每两个 4-bit code 合成一个字节，(first << 4) | second。
// PC 端 ble_proxy.py 按同样规则解码。
// ---------------------------------------------------------------------------

static const int16_t imaStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

static const int8_t imaIndexTable[16] = {-1, -1, -1, -1, 3, 4, 5, 6,
                                         7, 8, 9, 10, 11, 12, 13, 14};

struct IMAEncoder {
    int16_t predictor = 0;
    int8_t stepIndex = 0;
};

static uint8_t imaEncode(int16_t sample, IMAEncoder& st) {
    int32_t diff = (int32_t)sample - st.predictor;
    uint8_t code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    int32_t step = imaStepTable[st.stepIndex];
    int32_t mask = 4;
    for (int i = 0; i < 3; i++) {
        if (diff >= step) {
            code |= mask;
            diff -= step;
        }
        step >>= 1;
        mask >>= 1;
    }
    int32_t delta = imaStepTable[st.stepIndex] >> 3;
    if (code & 4) delta += imaStepTable[st.stepIndex];
    if (code & 2) delta += imaStepTable[st.stepIndex] >> 1;
    if (code & 1) delta += imaStepTable[st.stepIndex] >> 2;
    int32_t pred = st.predictor + ((code & 8) ? -delta : delta);
    if (pred > 32767) pred = 32767;
    else if (pred < -32768) pred = -32768;
    st.predictor = (int16_t)pred;
    st.stepIndex += imaIndexTable[code & 0x07];
    if (st.stepIndex < 0) st.stepIndex = 0;
    else if (st.stepIndex > 88) st.stepIndex = 88;
    return code;
}

// ---------------------------------------------------------------------------
// base64（避免依赖 Arduino 内置库的版本差异）
// ---------------------------------------------------------------------------

static void b64encode(const uint8_t* in, size_t inLen, char* out, size_t outCap) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < inLen && o + 4 < outCap; i += 3) {
        uint32_t v = ((uint32_t)in[i]) << 16;
        if (i + 1 < inLen) v |= ((uint32_t)in[i + 1]) << 8;
        if (i + 2 < inLen) v |= (uint32_t)in[i + 2];
        out[o++] = tbl[(v >> 18) & 0x3F];
        out[o++] = tbl[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < inLen) ? tbl[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < inLen) ? tbl[v & 0x3F] : '=';
    }
    out[o] = '\0';
}

// ---------------------------------------------------------------------------
// 采集状态
// ---------------------------------------------------------------------------

static constexpr size_t AUDIO_FRAME_SAMPLES = 512;        // 32ms @ 16kHz
static constexpr size_t AUDIO_FRAME_ADPCM = AUDIO_FRAME_SAMPLES / 2;
static constexpr size_t B64_CAP = AUDIO_FRAME_ADPCM * 4 / 3 + 8;
static constexpr uint32_t MAX_AUDIO_MS = 30000;           // 单次录音上限 30s
static constexpr uint32_t AUDIO_SAMPLE_RATE = 16000;

static bool micInitialized = false;
static bool capturing = false;
static unsigned long captureStartMs = 0;
static unsigned long totalSamples = 0;
static uint32_t seq = 0;
static IMAEncoder ima;

static int16_t pcmBuf[AUDIO_FRAME_SAMPLES];
static uint8_t adpcmBuf[AUDIO_FRAME_ADPCM];
static char b64Buf[B64_CAP];
static char frameBuf[640];

bool audioInit() {
    if (micInitialized) return true;
    // M5Cardputer.begin() 已注册 board_M5CardputerADV 的 ES8311 ADC 回调；
    // Mic.begin() 负责 I2S_NUM_0（BCLK=41/WS=43/DIN=46）初始化 + codec 上电。
    if (!M5Cardputer.Mic.begin()) return false;
    M5Cardputer.Mic.setSampleRate(AUDIO_SAMPLE_RATE);
    micInitialized = true;
    DBG("audio: mic ready (16kHz mono)");
    return true;
}

bool audioReady() { return micInitialized; }

bool audioCapturing() { return capturing; }

unsigned long audioCapturedMillis() {
    if (!capturing) return 0;
    return (millis() - captureStartMs);
}

void audioBeginCapture() {
    if (capturing) return;
    if (!micInitialized && !audioInit()) return;
    capturing = true;
    captureStartMs = millis();
    totalSamples = 0;
    seq = 0;
    ima = IMAEncoder();
    DBG("audio: capture start");
}

void audioEndCapture() {
    if (!capturing) return;
    capturing = false;
    snprintf(frameBuf, sizeof(frameBuf),
             "{\"evt\":\"audio_end\",\"seq\":%lu,\"len\":%lu,\"rate\":%u}\n",
             (unsigned long)(seq > 0 ? seq - 1 : 0), (unsigned long)totalSamples,
             (unsigned)AUDIO_SAMPLE_RATE);
    M5Cardputer.BLE.send(frameBuf);
    DBG("audio: capture end, samples=" + String(totalSamples));
    ima = IMAEncoder();
}

void audioTick() {
    if (!capturing) return;

    // 阻塞采集一帧（约 32ms）。Mic 后台任务 double-buffer，
    // 连续调用即得到连续的 16kHz 单声道 PCM。
    if (!M5Cardputer.Mic.record(pcmBuf, AUDIO_FRAME_SAMPLES)) return;

    for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i += 2) {
        uint8_t c1 = imaEncode(pcmBuf[i], ima);
        uint8_t c2 = imaEncode(pcmBuf[i + 1], ima);
        adpcmBuf[i / 2] = (uint8_t)((c1 << 4) | c2);
    }
    totalSamples += AUDIO_FRAME_SAMPLES;

    b64encode(adpcmBuf, AUDIO_FRAME_ADPCM, b64Buf, sizeof(b64Buf));
    snprintf(frameBuf, sizeof(frameBuf),
             "{\"evt\":\"audio\",\"seq\":%lu,\"data\":\"%s\"}\n",
             (unsigned long)seq++, b64Buf);
    M5Cardputer.BLE.send(frameBuf);

    if (audioCapturedMillis() >= MAX_AUDIO_MS) {
        audioEndCapture();
    }
}
