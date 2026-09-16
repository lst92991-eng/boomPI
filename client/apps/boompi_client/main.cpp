/** @file main.cpp
 * @brief 可执行程序入口；像教学例程一样按初始化、循环处理、收尾三个步骤展开。
 * 工作线程随进程存在，所以初始化后必须持续运行，直到应用要求退出。
 */
#include "boompi/application/voice_client.h"

int main() {
  // 初始化成功后，主线程持续处理对话。
  if (App_Init()) {
    while (true) {
      if (!App_Process()) {
        break;
      }
    }
  }
  // 初始化失败、运行故障和主动退出，都在这里收尾。
  return App_Close();
}
