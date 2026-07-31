#include "boompi/config/voice_client_config.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <string>

namespace boompi::config {
namespace {

constexpr std::uint32_t kMaximumSpeakerGainPercent = 400U;

Status Bad(const char* const variable_name) {
  return Status::Error(StatusCode::kInvalidArgument, std::string(variable_name) +
                                                    " is missing or outside allowed bounds");
}

Status ReadString(const char* name, bool required, std::size_t maximum,
                  std::string* output) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') { output->clear(); return required ? Bad(name) : Status::Ok(); }
  std::size_t size = 0U;
  while (size <= maximum && value[size] != '\0') ++size;
  if (size > maximum) return Bad(name);
  output->assign(value, size);
  return Status::Ok();
}

Status ReadUint(const char* name, std::uint32_t fallback,
                std::uint32_t minimum, std::uint32_t maximum,
                std::uint32_t* output) {
  const char* text = std::getenv(name);
  if (text == nullptr || *text == '\0') { *output = fallback; return Status::Ok(); }
  std::uint64_t parsed = 0U;
  for (const char* current = text; *current != '\0'; ++current) {
    if (*current < '0' || *current > '9') return Bad(name);
    const auto digit = static_cast<unsigned>(*current - '0');
    if (digit > maximum || parsed > (maximum - digit) / 10U) return Bad(name);
    parsed = parsed * 10U + digit;
  }
  if (parsed < minimum) return Bad(name);
  *output = static_cast<std::uint32_t>(parsed);
  return Status::Ok();
}

Status ReadPolarity(const char* name, std::int8_t* output) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') { *output = 1; return Status::Ok(); }
  if (value[0] == '1' && value[1] == '\0') { *output = 1; return Status::Ok(); }
  if (value[0] == '-' && value[1] == '1' && value[2] == '\0') { *output = -1; return Status::Ok(); }
  return Bad(name);
}

int Hex(const char value) noexcept {
  if (value >= '0' && value <= '9') return value - '0';
  return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

bool IsUuid(const std::string& text) noexcept {
  if (text.size() != 36U) return false;
  std::array<std::uint8_t, 16U> decoded{};
  std::size_t output_index = 0U;
  int high = -1;
  for (std::size_t index = 0U; index < text.size(); ++index) {
    const bool separator = index == 8U || index == 13U || index == 18U || index == 23U;
    if (separator) {
      if (text[index] != '-') return false;
      continue;
    }
    const int nibble = Hex(text[index]);
    if (nibble < 0) return false;
    if (high < 0) high = nibble;
    else {
      decoded[output_index++] = static_cast<std::uint8_t>(high * 16 + nibble);
      high = -1;
    }
  }
  if (output_index != decoded.size() ||
      std::all_of(decoded.begin(), decoded.end(), [](std::uint8_t value) { return value == 0U; })) return false;
  return true;
}

bool IsToken(const std::string& value) noexcept {
  return value.size() >= 32U && value.size() <= 256U &&
         std::all_of(value.begin(), value.end(), [](const char current) {
           const auto byte = static_cast<unsigned char>(current);
           return byte >= 0x21U && byte <= 0x7EU;
         });
}

}  // namespace

VoiceClientConfig::~VoiceClientConfig() noexcept {
  auto* bytes = reinterpret_cast<volatile char*>(device_token.data());
  for (std::size_t index = 0U; index < device_token.size(); ++index) bytes[index] = 0;
  device_token.clear();
}

Status LoadVoiceClientConfigFromEnvironment(VoiceClientConfig* const output) {
  if (output == nullptr) return Status::Error(StatusCode::kInvalidArgument, "voice client config output is null");
  Status status;
  if (!(status = ReadString("BOOMPI_SERVER_IP", true, 64U, &output->server_ip)).ok()) return status;
  if (!(status = ReadString("BOOMPI_SERVER_SPKI_SHA256", true, 64U, &output->server_spki_sha256)).ok()) return status;
  if (!(status = ReadString("BOOMPI_DEVICE_ID", true, 36U, &output->device_id)).ok()) return status;
  if (!IsUuid(output->device_id)) return Bad("BOOMPI_DEVICE_ID");
  if (!(status = ReadString("BOOMPI_DEVICE_TOKEN", true, 256U, &output->device_token)).ok()) return status;
  if (!IsToken(output->device_token)) return Bad("BOOMPI_DEVICE_TOKEN");
  if (!(status = ReadString("BOOMPI_CAPTURE_PCM", true, 31U, &output->capture_pcm)).ok()) return status;
  if (!(status = ReadString("BOOMPI_PLAYBACK_PCM", true, 31U, &output->playback_pcm)).ok()) return status;
  std::uint32_t parsed = 0U;
  if (!(status = ReadUint("BOOMPI_SERVER_PORT", 17806U, 1U, 65535U, &parsed)).ok()) return status;
  output->server_port = static_cast<std::uint16_t>(parsed);
  if (!(status = ReadPolarity("BOOMPI_CAPTURE_LEFT_POLARITY", &output->capture_left_polarity)).ok()) return status;
  if (!(status = ReadPolarity("BOOMPI_CAPTURE_RIGHT_POLARITY", &output->capture_right_polarity)).ok()) return status;
  if (!(status = ReadUint("BOOMPI_VOLUME_PERCENT", 60U, 0U, 100U, &parsed)).ok()) return status;
  output->volume_percent = static_cast<std::uint8_t>(parsed);
  if (!(status = ReadUint("BOOMPI_SPEAKER_GAIN_PERCENT", 100U, 1U, kMaximumSpeakerGainPercent, &parsed)).ok()) return status;
  output->speaker_gain_percent = static_cast<std::uint16_t>(parsed);
  if (!(status = ReadUint("BOOMPI_BARGE_IN_ENABLED", 1U, 0U, 1U, &parsed)).ok()) return status;
  output->barge_in_enabled = parsed != 0U;
  if (!(status = ReadString("BOOMPI_SNOWBOY_RESOURCE_FILE", false, 1024U, &output->snowboy_resource_path)).ok()) return status;
  if (!(status = ReadString("BOOMPI_SNOWBOY_MODEL_FILE", false, 1024U, &output->snowboy_model_path)).ok()) return status;
  if (!(status = ReadString("BOOMPI_SNOWBOY_SENSITIVITY", false, 32U, &output->snowboy_sensitivity)).ok()) return status;
  if (output->snowboy_sensitivity.empty()) output->snowboy_sensitivity = "0.5";
  return output->server_spki_sha256.size() == 44U ? Status::Ok()
                                                  : Bad("BOOMPI_SERVER_SPKI_SHA256");
}

}  // namespace boompi::config
