#pragma once

#include <chrono>

#include "boompi/config/voice_client_config.h"

// 默认使用系统单调时钟；可控时钟仅供主机验证使用，不是学生配置项。
using AppReadClock = std::chrono::steady_clock::time_point (*)();

// 本程序只有一份应用状态。由 main 按 Init → Process → Close 调用，重新 Init 前先 Close。
bool App_Init(const boompi::config::VoiceClientConfig& config,
              AppReadClock clock = &std::chrono::steady_clock::now);
bool App_Process();                   // 执行一轮问答处理；失败返回 false。
void App_Close() noexcept;            // 停线程、关设备；可重复调用。
const char* App_GetError() noexcept;  // 错误文本保留到下一次 Init。
