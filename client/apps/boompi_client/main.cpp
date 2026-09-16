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
