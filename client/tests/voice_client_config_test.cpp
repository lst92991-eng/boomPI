/**
 * @file voice_client_config_test.cpp
 * @brief 验证启动配置拒绝非法身份/端点，以及 UiView 的 UTF-8 字幕追加边界。
 *
 * 每组配置先恢复有效基线再改变一个字段，直接调用生产校验函数，不访问网络或硬件。
 * 字幕测试直接检查固定缓冲区，确认追加/截断/换轮清空不残留旧文本或拆开汉字。
 */
#include "boompi/config/voice_client_config.h"

#include <cstdlib>
#include <iostream>
#include <string>

#include "boompi/ui/ui_view.h"
namespace {
/** @brief 用平台提供的环境 API 准备测试输入，平台分支仅属于 Host 测试。 */
void Env(const char* name, const char* value) {
#ifdef _WIN32
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}
/** @brief 为每个独立反例恢复一组合法文本；pin 是格式样本，不是实际服务器身份。 */
void Valid() {
  Env("BOOMPI_DEVICE_ID", "00112233-4455-4677-8899-aabbccddeeff");
  Env("BOOMPI_SERVER_IP", "127.0.0.1");
  Env("BOOMPI_SERVER_PORT", "17806");
  Env("BOOMPI_SERVER_SPKI_SHA256", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=");
}
}  // namespace
/** @brief 依次检查固定端点、发现模式和字幕缓冲，任一契约失败均返回非零。 */
int main() {
  int failures = 0;
  auto require = [&](bool pass, const char* message) {
    if (!pass) {
      std::cerr << message << '\n';
      ++failures;
    }
  };
  boompi::config::VoiceClientConfig config;
  std::string error;
  Valid();
  require(boompi::config::LoadClientConfig(&config, &error), "valid config rejected");
  for (const auto* invalid :
       {"garbage", "256.1.2.3", "1.2.3", "1.2.3.4.5", "01.2.3.4", "1..2.3"}) {
    Valid();
    Env("BOOMPI_SERVER_IP", invalid);
    require(!boompi::config::LoadClientConfig(&config, &error) &&
                error.find("BOOMPI_SERVER_IP") != std::string::npos,
            "invalid IPv4 accepted");
  }
  for (const auto* invalid : {"0", "-1", "65536", "1abc", "99999999999"}) {
    Valid();
    Env("BOOMPI_SERVER_PORT", invalid);
    require(!boompi::config::LoadClientConfig(&config, &error), "invalid port accepted");
  }
  Valid();
  Env("BOOMPI_SERVER_IP", "");
  Env("BOOMPI_SERVER_SPKI_SHA256", "");
  require(boompi::config::LoadClientConfig(&config, &error), "discovery config rejected");
  Env("BOOMPI_DEVICE_ID", "00000000-0000-0000-0000-000000000000");
  require(!boompi::config::LoadClientConfig(&config, &error), "zero UUID accepted");
  require(!boompi::config::IsValidSpkiSha256("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB="),
          "pin padding accepted");
  boompi::ui::UiView view;
  // 三字节 UTF-8 汉字覆盖滚动保留尾部的边界；ASCII 超长片段再检查缓冲长度上限。
  view.AppendText("你好");
  view.AppendText("，世界");
  require(std::string(view.text.data()) == "你好，世界", "subtitle append failed");
  for (int i = 0; i < 100; ++i) {
    view.AppendText("甲乙丙");
  }
  const std::string tail(view.text.data());
  require(tail.size() <= 126 && tail.size() % 3 == 0, "subtitle split UTF-8");
  view.ClearText();
  view.AppendText("新问题");
  require(std::string(view.text.data()) == "新问题", "subtitle retained old turn");
  view.AppendText(std::string(400, 'a'));
  require(std::strlen(view.text.data()) == 126, "long subtitle overflow");
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
