#ifndef CONFIG_H
#define CONFIG_H

#define BEEP_FREQ_ASK     1000
#define BEEP_FREQ_CONFIRM 1300
// choose 原 900Hz 频率太低，小扬声器听感弱；调到 1300（与 confirm 一致）。
#define BEEP_FREQ_CHOOSE  1300
#define BEEP_FREQ_ESCALATE 1500
#define BEEP_DURATION_MS  400
#define BEEP_ANSWER_FREQ  1400
#define BEEP_ANSWER_DURATION_MS 150

// 提示音音量调节（0~255，每次 ±VOLUME_STEP）
#define VOLUME_STEP 32
#define VOLUME_MIN 0
#define VOLUME_MAX 255

#endif
