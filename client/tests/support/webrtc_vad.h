/**
 * @file webrtc_vad.h
 * @brief Host 检测测试使用的 VAD 窄接口声明，实现在 audio_pipeline_test.cpp。
 *
 * 分类结果由测试脚本注入；SpeechDetector 中的电平准入、起止滞回和 AEC 窗口仍用
 * 生产实现。Create/Free 模拟句柄所有权，不验证 WebRTC 算法准确率或厂商 ABI。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Host测试只控制厂商分类结果；门限、滞回和AEC时序仍运行生产代码。 */
typedef struct VadInst VadInst;
VadInst* WebRtcVad_Create(void);
void WebRtcVad_Free(VadInst* vad);
int WebRtcVad_Init(VadInst* vad);
int WebRtcVad_set_mode(VadInst* vad, int mode);
int WebRtcVad_ValidRateAndFrameLength(int rate, size_t samples);
int WebRtcVad_Process(VadInst* vad, int rate, const int16_t* pcm, size_t samples);
