#include "boompi/config/voice_client_config.h"
#include "boompi/ui/ui_view.h"
#include <cstdlib>
#include <iostream>
#include <string>
namespace {
void Env(const char* name, const char* value) {
#ifdef _WIN32
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}
void Valid() {
  Env("BOOMPI_DEVICE_ID", "00112233-4455-4677-8899-aabbccddeeff");
  Env("BOOMPI_SERVER_IP", "127.0.0.1");
  Env("BOOMPI_SERVER_PORT", "17806");
  Env("BOOMPI_SERVER_SPKI_SHA256", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=");
}
}
int main() {
  int failures = 0;
  auto require = [&](bool pass, const char* message) {
    if (!pass) { std::cerr << message << '\n'; ++failures; }
  };
  boompi::config::VoiceClientConfig config;
  std::string error;
  Valid();
  require(boompi::config::LoadVoiceClientConfigFromEnvironment(&config, &error), "valid config rejected");
  for (const auto* invalid : {"garbage", "256.1.2.3", "1.2.3", "1.2.3.4.5", "01.2.3.4", "1..2.3"}) {
    Valid(); Env("BOOMPI_SERVER_IP", invalid);
    require(!boompi::config::LoadVoiceClientConfigFromEnvironment(&config, &error) &&
            error.find("BOOMPI_SERVER_IP") != std::string::npos, "invalid IPv4 accepted");
  }
  for (const auto* invalid : {"0", "-1", "65536", "1abc", "99999999999"}) {
    Valid(); Env("BOOMPI_SERVER_PORT", invalid);
    require(!boompi::config::LoadVoiceClientConfigFromEnvironment(&config, &error), "invalid port accepted");
  }
  Valid(); Env("BOOMPI_SERVER_IP", ""); Env("BOOMPI_SERVER_SPKI_SHA256", "");
  require(boompi::config::LoadVoiceClientConfigFromEnvironment(&config, &error), "discovery config rejected");
  Env("BOOMPI_DEVICE_ID", "00000000-0000-0000-0000-000000000000");
  require(!boompi::config::LoadVoiceClientConfigFromEnvironment(&config, &error), "zero UUID accepted");
  require(!boompi::config::IsValidSpkiSha256("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB="), "pin padding accepted");
  boompi::ui::UiView view;
  view.AppendText("你好"); view.AppendText("，世界");
  require(std::string(view.text.data()) == "你好，世界", "subtitle append failed");
  for (int i = 0; i < 100; ++i) view.AppendText("甲乙丙");
  const std::string tail(view.text.data());
  require(tail.size() <= 126 && tail.size() % 3 == 0, "subtitle split UTF-8");
  view.ClearText(); view.AppendText("新问题");
  require(std::string(view.text.data()) == "新问题", "subtitle retained old turn");
  view.AppendText(std::string(400, 'a'));
  require(std::strlen(view.text.data()) == 126, "long subtitle overflow");
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
