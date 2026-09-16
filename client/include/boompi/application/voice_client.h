#pragma once

// 本程序只有一份应用状态。由 main 按 Init → Process → Close 调用，重新 Init 前先 Close。
bool App_Init();           // 准备进程、读取配置，顺序初始化各模块。
bool App_Process();        // 处理一轮；true继续，false进入收尾。
int App_Close() noexcept;  // 停线程、关设备并报告故障；返回进程退出码。
