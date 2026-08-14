#pragma once
#include <Arduino.h>

// P1 语音输入模块
// - 采集：M5Cardputer.Mic（ES8311 ADC，16kHz/16bit/单声道，M5Unified 内置支持 Cardputer-ADV）
// - 压缩：IMA-ADPCM（4:1）
// - 传输：BLE 文本帧（换行分隔 JSON）
//   帧格式：
//     {"evt":"audio","seq":N,"data":"<base64 ADPCM>"}
//     {"evt":"audio_end","seq":N,"len":<PCM采样数>,"rate":16000,"mode":"prompt"|"keyboard"}
// - 交互：按住 Ctrl 说话，松开发送。两种模式：
//     prompt  ：WAITING_INPUT（ask/escalate）状态，转写结果回传 agent
//     keyboard：IDLE（已连接无 prompt）状态，转写结果直接输入电脑聚焦窗口

/// 初始化麦克风（I2S + ES8311 ADC）。可重复调用，幂等。
bool audioInit();

/// 是否已初始化
bool audioReady();

/// 开始一次录音会话。
/// keyboardMode=true 表示键盘输入模式（转写结果直接输入电脑），
/// false 表示 prompt 模式（转写结果回传 agent）。
void audioBeginCapture(bool keyboardMode = false);

/// 结束录音会话并发送 audio_end 帧
void audioEndCapture();

/// 正在录音？
bool audioCapturing();

/// 本次录音已采集的毫秒数
unsigned long audioCapturedMillis();

/// 主循环每帧调用：采集一帧 PCM -> ADPCM -> BLE 发送
void audioTick();
