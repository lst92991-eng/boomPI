/** @file voice_client.h
 * @brief 程序入口的三个步骤；本应用只有一份对话状态，由 main 串行调用。
 */
#pragma once

/** @brief 准备进程、读取配置，再启动界面、音频和网络。
 * @return 成功才可进入 Process；失败也必须调用 Close，回收已完成的初始化部分。
 * 重新 Init 前先 Close；启动网络线程成功不代表已经连接服务端。
 */
bool App_Init();
/** @brief 处理回复、输入帧和触摸动作；每次输入等待最多 20ms。
 * @return true 继续循环；false 表示主动退出或运行故障，统一进入 Close。
 * 单轮取消、断线重连属于可恢复情况，仍返回 true。
 */
bool App_Process();
/** @brief 停线程后关设备，释放进程锁；返回进程退出码 0 正常、1 故障。
 * 初始化未完成时也可调用；故障原因由 debug 回调在此报告。
 */
int App_Close() noexcept;
