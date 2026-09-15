#pragma once

#include "boompi/config/voice_client_config.h"

// 本程序只有一份应用状态。由 main 按 Init → Process → Close 调用，重新 Init 前先 Close。
bool App_Init(const boompi::config::VoiceClientConfig& config);
bool App_Process();                   // 执行一轮问答处理；失败返回 false。
void App_Close() noexcept;            // 停线程、关设备；可重复调用。
const char* App_GetError() noexcept;  // 错误文本保留到下一次 Init。
