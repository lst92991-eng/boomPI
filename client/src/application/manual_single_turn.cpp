#include "boompi/application/manual_single_turn.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <new>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "boompi/audio/audio_dsp_types.h"
#include "boompi/audio/fir_decimator_48_to_16.h"
#include "boompi/audio/playback_frame.h"
#include "boompi/audio/playback_gain_limiter.h"
#include "boompi/audio/playback_generation.h"
#include "boompi/audio/playback_renderer_24_to_48.h"
#include "boompi/network/reconnect_backoff.h"
#include "boompi/network/wss_client.h"
#include "boompi/platform/rv1106/alsa_single_turn_io.h"
#include "boompi/protocol/audio_packet.h"
#include "boompi/protocol/server_control.h"

#if defined(BOOMPI_ENABLE_SNOWBOY) && \
    defined(BOOMPI_ENABLE_ROCKCHIP_3A) && \
    defined(BOOMPI_ENABLE_WEBRTC_VAD)
#define BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH 1
#include "boompi/platform/rv1106/rockchip_voice_dsp.h"
#include "boompi/platform/rv1106/snowboy_wake_word_engine.h"
#include "boompi/platform/rv1106/webrtc_vad_gate.h"
#endif

namespace boompi::application {
namespace {

constexpr std::uint32_t kInputSampleRateHz = 16000U;
constexpr std::uint32_t kOutputSampleRateHz = 24000U;
constexpr std::uint32_t kFrameDurationMs = 20U;
constexpr std::uint32_t kCaptureOperationTimeoutMs = 1000U;
constexpr std::uint32_t kPlaybackOperationTimeoutMs = 1000U;
constexpr std::uint32_t kPlaybackDrainTimeoutMs = 5000U;
constexpr std::uint32_t kPlaybackCompletionTimeoutMs =
    kPlaybackDrainTimeoutMs + kPlaybackOperationTimeoutMs + 1000U;
constexpr std::uint32_t kResponseInactivityTimeoutMs = 30000U;
constexpr std::size_t kUplinkFrameBytes = 640U;
constexpr std::size_t kMaximumDownlinkPacketBytes = 960U;
constexpr std::size_t kMaximumDownlinkBytes =
    static_cast<std::size_t>(kOutputSampleRateHz) * 2U * 60U;
constexpr std::size_t kMaximumResponseMessages = 4096U;
constexpr std::size_t kMaximumResponseTextBytes = 256U * 1024U;
constexpr std::uint16_t kPcmFlagStart = 0x0001U;
constexpr std::uint16_t kPcmFlagEnd = 0x0002U;
constexpr std::uint16_t kPcmFlagDiscontinuity = 0x0004U;
constexpr std::size_t kPlaybackQueueCapacity = 64U;
constexpr std::size_t kPlaybackPrebufferFrames = 6U;
constexpr std::uint32_t kPlaybackEnqueueTimeoutMs = 100U;
static_assert(kPlaybackQueueCapacity * kFrameDurationMs <= 1500U,
              "playback queue must stay within the 1.5 second hard bound");
static_assert(kPlaybackPrebufferFrames * kFrameDurationMs == 120U,
              "initial playback prebuffer must remain 120 ms");
#if defined(BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH)
constexpr std::size_t kVoicePreRollFrames = 25U;
constexpr std::size_t kConfirmedBargeTailFrames = 125U;
constexpr std::size_t kVoiceCaptureQueueCapacity = 8U;
constexpr std::size_t kPlaybackReferenceQueueCapacity = 16U;
constexpr std::size_t kPlaybackReferenceLeadFrames = 3U;
constexpr std::uint32_t kBargeAecWarmupFrames = 600U / kFrameDurationMs;
constexpr std::uint32_t kEchoTailFrames = 500U / kFrameDurationMs;
constexpr double kEchoReferenceActiveMeanSquare = 64.0;
constexpr double kEchoEnergyExcessRatio = 3.0;
constexpr double kEchoStrongOutputRatio = 6.0;
constexpr double kEchoStrongMicrophoneExcessRatio = 1.5;
constexpr double kEchoReferenceEnvelopeDecay = 0.72;
constexpr std::uint32_t kEchoAdmissionHoldFrames = 160U / kFrameDurationMs;
constexpr std::uint32_t kEchoQuietBootstrapFrames = 200U / kFrameDurationMs;
constexpr std::uint32_t kFirstSpeechTimeoutFrames = 6000U / kFrameDurationMs;
constexpr std::uint32_t kFollowUpTimeoutFrames = 3000U / kFrameDurationMs;
constexpr std::uint32_t kMaximumUtteranceFrames = 30000U / kFrameDurationMs;
constexpr std::uint64_t kVoiceKeepAliveIntervalUs = 5000000U;
static_assert(kVoiceCaptureQueueCapacity * kFrameDurationMs == 160U,
              "voice capture queue must remain a bounded 160 ms bridge");
static_assert(kPlaybackReferenceLeadFrames * kFrameDurationMs == 60U,
              "playback reference lead must match the ALSA start threshold");
static_assert(kBargeAecWarmupFrames * kFrameDurationMs == 600U,
              "barge-in AEC warmup must remain 600 ms");
static_assert(kEchoTailFrames * kFrameDurationMs == 500U,
              "echo tail gate must remain 500 ms until enclosure tuning");
static_assert(kEchoAdmissionHoldFrames * kFrameDurationMs == 160U,
              "near-speech admission must bridge the barge confirmation");
static_assert(kEchoQuietBootstrapFrames * kFrameDurationMs == 200U,
              "quiet floor bootstrap must remain short and bounded");
static_assert((kVoicePreRollFrames + kConfirmedBargeTailFrames) *
                      kFrameDurationMs ==
                  3000U,
              "confirmed barge buffer must remain bounded to 3 seconds");
#endif

void SecureZero(void* const memory, const std::size_t size_bytes) noexcept {
  auto* current = static_cast<volatile std::uint8_t*>(memory);
  for (std::size_t index = 0U; index < size_bytes; ++index) {
    current[index] = 0U;
  }
}

void SecureClear(std::string* const value) noexcept {
  if (value != nullptr && !value->empty()) {
    SecureZero(&(*value)[0], value->size());
    value->clear();
  }
}

struct SensitiveString final {
  ~SensitiveString() { SecureClear(&value); }
  std::string value;
};

struct ManualConfig final {
  ManualConfig() = default;
  ~ManualConfig() { SecureClear(&device_token); }
  ManualConfig(const ManualConfig&) = delete;
  ManualConfig& operator=(const ManualConfig&) = delete;

  std::string server_ip;
  std::uint16_t server_port{17806U};
  std::string server_name;
  std::string server_spki_sha256;
  std::string device_id;
  std::string device_token;
  std::array<std::uint8_t, 16U> device_uuid{};
  std::string capture_pcm;
  std::string playback_pcm;
  std::uint8_t capture_mic_slot{0U};
  std::int8_t capture_mic_polarity{1};
  std::uint32_t record_ms{3000U};
  std::uint8_t volume_percent{60U};
  std::uint16_t speaker_gain_percent{100U};
  std::string snowboy_resource_path;
  std::string snowboy_model_path;
  std::string snowboy_sensitivity{"0.5"};
  std::int8_t capture_left_polarity{1};
  std::int8_t capture_right_polarity{1};
  bool barge_in_enabled{true};
  bool legacy_downlink_framing{false};
};

class CaptureWorkspace final {
 public:
  explicit CaptureWorkspace(const std::size_t frame_count)
      : frame_count(frame_count) {}

  ~CaptureWorkspace() {
    decimator.Reset();
    SecureZero(capture_period.data(),
               capture_period.size() * sizeof(capture_period[0]));
    SecureZero(input_planes.data(), sizeof(input_planes));
    SecureZero(output_planes.data(), sizeof(output_planes));
  }

  CaptureWorkspace(const CaptureWorkspace&) = delete;
  CaptureWorkspace& operator=(const CaptureWorkspace&) = delete;

  audio::FirDecimator48To16 decimator;
  platform::rv1106::AlsaSingleTurnIo::CapturePeriod capture_period{};
  audio::CapturePlanes48k input_planes{};
  audio::CapturePlanes16k output_planes{};
  std::size_t frame_count{0U};
};

struct SensitiveUplinkPayload final {
  ~SensitiveUplinkPayload() {
    SecureZero(bytes.data(), bytes.size() * sizeof(bytes[0]));
  }
  std::array<std::uint8_t, kUplinkFrameBytes> bytes{};
};

struct SensitiveTtsFrame final {
  ~SensitiveTtsFrame() {
    SecureZero(frame.samples.data(),
               frame.samples.size() * sizeof(frame.samples[0]));
  }
  audio::TtsPcmFrame24k frame{};
};

#if defined(BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH)
using VoiceSamples = platform::rv1106::RockchipVoiceFrame16k;

struct VoiceCaptureSlot final {
  platform::rv1106::AlsaSingleTurnIo::CapturePeriod period{};
  platform::rv1106::AlsaCaptureReadResult read_result{};
  VoiceSamples playback_reference{};
  std::uint64_t capture_timestamp_us{0U};
  std::uint64_t read_latency_us{0U};
  bool playback_reference_aligned{false};
};

class VoicePreRoll final {
 public:
  ~VoicePreRoll() { Clear(); }

  void Push(const VoiceSamples& samples) noexcept {
    if (preserve_prefix_) {
      if (tail_count_ != tail_.size()) {
        tail_[tail_count_] = samples;
        ++tail_count_;
      }
      return;
    }
    frames_[write_index_] = samples;
    write_index_ = (write_index_ + 1U) % frames_.size();
    count_ = std::min(count_ + 1U, frames_.size());
  }

  void PreservePrefix() noexcept { preserve_prefix_ = true; }

  const VoiceSamples& Oldest(const std::size_t offset) const noexcept {
    if (offset >= count_) {
      return tail_[offset - count_];
    }
    const std::size_t first =
        (write_index_ + frames_.size() - count_) % frames_.size();
    return frames_[(first + offset) % frames_.size()];
  }

  std::size_t size() const noexcept { return count_ + tail_count_; }

  void Clear() noexcept {
    SecureZero(frames_.data(), sizeof(frames_));
    SecureZero(tail_.data(), sizeof(tail_));
    write_index_ = 0U;
    count_ = 0U;
    tail_count_ = 0U;
    preserve_prefix_ = false;
  }

 private:
  std::array<VoiceSamples, kVoicePreRollFrames> frames_{};
  std::array<VoiceSamples, kConfirmedBargeTailFrames> tail_{};
  std::size_t write_index_{0U};
  std::size_t count_{0U};
  std::size_t tail_count_{0U};
  bool preserve_prefix_{false};
};
#endif

Status ConfigError(const char* const variable_name) {
  return Status::Error(StatusCode::kInvalidArgument,
                       std::string(variable_name) +
                           " is missing or outside allowed bounds");
}

std::size_t BoundedLength(const char* const value,
                          const std::size_t maximum_bytes) noexcept {
  std::size_t length = 0U;
  while (length <= maximum_bytes && value[length] != '\0') {
    ++length;
  }
  return length;
}

Status ReadEnvironmentString(const char* const variable_name,
                             const bool required,
                             const std::size_t maximum_bytes,
                             std::string* const output) {
  if (output == nullptr || maximum_bytes == 0U) {
    return Status::Error(StatusCode::kInternal,
                         "environment string destination is invalid");
  }
  const char* const value = std::getenv(variable_name);
  if (value == nullptr || value[0] == '\0') {
    output->clear();
    return required ? ConfigError(variable_name) : Status::Ok();
  }
  const std::size_t length = BoundedLength(value, maximum_bytes);
  if (length > maximum_bytes) {
    return ConfigError(variable_name);
  }
  output->assign(value, length);
  return Status::Ok();
}

Status ReadEnvironmentUint(const char* const variable_name,
                           const std::uint32_t default_value,
                           const std::uint32_t minimum_value,
                           const std::uint32_t maximum_value,
                           std::uint32_t* const output) {
  if (output == nullptr || minimum_value > maximum_value ||
      default_value < minimum_value || default_value > maximum_value) {
    return Status::Error(StatusCode::kInternal,
                         "environment integer destination is invalid");
  }
  const char* const value = std::getenv(variable_name);
  if (value == nullptr || value[0] == '\0') {
    *output = default_value;
    return Status::Ok();
  }
  std::uint64_t parsed = 0U;
  std::size_t length = 0U;
  while (value[length] != '\0') {
    if (length >= 10U || value[length] < '0' || value[length] > '9') {
      return ConfigError(variable_name);
    }
    parsed = parsed * 10U +
             static_cast<std::uint64_t>(value[length] - '0');
    if (parsed > maximum_value) {
      return ConfigError(variable_name);
    }
    ++length;
  }
  if (length == 0U || parsed < minimum_value) {
    return ConfigError(variable_name);
  }
  *output = static_cast<std::uint32_t>(parsed);
  return Status::Ok();
}

Status ReadEnvironmentPolarity(const char* const variable_name,
                               const std::int8_t default_value,
                               std::int8_t* const output) {
  if (output == nullptr || (default_value != 1 && default_value != -1)) {
    return Status::Error(StatusCode::kInternal,
                         "capture polarity destination is invalid");
  }
  const char* const value = std::getenv(variable_name);
  if (value == nullptr || value[0] == '\0') {
    *output = default_value;
    return Status::Ok();
  }
  if (value[0] == '1' && value[1] == '\0') {
    *output = 1;
    return Status::Ok();
  }
  if (value[0] == '-' && value[1] == '1' && value[2] == '\0') {
    *output = -1;
    return Status::Ok();
  }
  return ConfigError(variable_name);
}

int LowerHexNibble(const char value) noexcept {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  return -1;
}

bool ParseDeviceUuid(const std::string& device_id,
                     std::array<std::uint8_t, 16U>* const output) noexcept {
  if (output == nullptr || device_id.size() != 36U ||
      device_id[8U] != '-' || device_id[13U] != '-' ||
      device_id[18U] != '-' || device_id[23U] != '-') {
    return false;
  }
  std::array<std::uint8_t, 16U> decoded{};
  std::size_t input_index = 0U;
  for (std::size_t byte_index = 0U; byte_index < decoded.size();
       ++byte_index) {
    while (input_index < device_id.size() && device_id[input_index] == '-') {
      ++input_index;
    }
    if (input_index + 1U >= device_id.size()) {
      return false;
    }
    const int high = LowerHexNibble(device_id[input_index]);
    const int low = LowerHexNibble(device_id[input_index + 1U]);
    if (high < 0 || low < 0) {
      return false;
    }
    decoded[byte_index] = static_cast<std::uint8_t>(
        static_cast<unsigned>(high * 16 + low));
    input_index += 2U;
  }
  while (input_index < device_id.size() && device_id[input_index] == '-') {
    ++input_index;
  }
  if (input_index != device_id.size() ||
      std::all_of(decoded.begin(), decoded.end(),
                  [](const std::uint8_t value) { return value == 0U; })) {
    return false;
  }
  *output = decoded;
  return true;
}

bool IsVisibleAscii(const std::string& value,
                    const std::size_t minimum_bytes,
                    const std::size_t maximum_bytes) noexcept {
  return value.size() >= minimum_bytes && value.size() <= maximum_bytes &&
         std::all_of(value.begin(), value.end(), [](const char current) {
           const auto byte = static_cast<unsigned char>(current);
           return byte >= 0x21U && byte <= 0x7EU;
         });
}

Status LoadManualConfig(ManualConfig* const output) {
  if (output == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "manual single-turn config output is null");
  }
  Status status = ReadEnvironmentString(
      "BOOMPI_SERVER_IP", true, 64U, &output->server_ip);
  if (!status.ok()) {
    return status;
  }
  status = ReadEnvironmentString("BOOMPI_SERVER_NAME", false, 253U,
                                 &output->server_name);
  if (!status.ok()) {
    return status;
  }
  status = ReadEnvironmentString("BOOMPI_SERVER_SPKI_SHA256", true, 64U,
                                 &output->server_spki_sha256);
  if (!status.ok()) {
    return status;
  }
  status = ReadEnvironmentString("BOOMPI_DEVICE_ID", true, 36U,
                                 &output->device_id);
  if (!status.ok() ||
      !ParseDeviceUuid(output->device_id, &output->device_uuid)) {
    return ConfigError("BOOMPI_DEVICE_ID");
  }
  status = ReadEnvironmentString("BOOMPI_DEVICE_TOKEN", true, 256U,
                                 &output->device_token);
  if (!status.ok() ||
      !IsVisibleAscii(output->device_token, 32U, 256U)) {
    return ConfigError("BOOMPI_DEVICE_TOKEN");
  }
  status = ReadEnvironmentString("BOOMPI_CAPTURE_PCM", true, 31U,
                                 &output->capture_pcm);
  if (!status.ok()) {
    return status;
  }
  status = ReadEnvironmentString("BOOMPI_PLAYBACK_PCM", true, 31U,
                                 &output->playback_pcm);
  if (!status.ok()) {
    return status;
  }

  std::uint32_t parsed = 0U;
  status = ReadEnvironmentUint("BOOMPI_SERVER_PORT", 17806U, 1U, 65535U,
                               &parsed);
  if (!status.ok()) {
    return status;
  }
  output->server_port = static_cast<std::uint16_t>(parsed);
  status = ReadEnvironmentUint("BOOMPI_CAPTURE_MIC_SLOT", 0U, 0U, 1U,
                               &parsed);
  if (!status.ok()) {
    return status;
  }
  output->capture_mic_slot = static_cast<std::uint8_t>(parsed);

  status = ReadEnvironmentPolarity("BOOMPI_CAPTURE_MIC_POLARITY", 1,
                                   &output->capture_mic_polarity);
  if (!status.ok()) {
    return status;
  }
  status = ReadEnvironmentPolarity("BOOMPI_CAPTURE_LEFT_POLARITY", 1,
                                   &output->capture_left_polarity);
  if (!status.ok()) {
    return status;
  }
  status = ReadEnvironmentPolarity("BOOMPI_CAPTURE_RIGHT_POLARITY", 1,
                                   &output->capture_right_polarity);
  if (!status.ok()) {
    return status;
  }

  status = ReadEnvironmentUint("BOOMPI_MANUAL_RECORD_MS", 3000U, 20U,
                               10000U, &output->record_ms);
  if (!status.ok() || output->record_ms % kFrameDurationMs != 0U) {
    return ConfigError("BOOMPI_MANUAL_RECORD_MS");
  }
  status = ReadEnvironmentUint("BOOMPI_VOLUME_PERCENT", 60U, 0U, 100U,
                               &parsed);
  if (!status.ok()) {
    return status;
  }
  output->volume_percent = static_cast<std::uint8_t>(parsed);
  status = ReadEnvironmentUint(
      "BOOMPI_SPEAKER_GAIN_PERCENT", 100U, 1U,
      audio::PlaybackGainLimiter::kMaximumSpeakerGainPercent, &parsed);
  if (!status.ok()) {
    return status;
  }
  output->speaker_gain_percent = static_cast<std::uint16_t>(parsed);

  status = ReadEnvironmentUint("BOOMPI_BARGE_IN_ENABLED", 1U, 0U, 1U,
                               &parsed);
  if (!status.ok()) {
    return status;
  }
  output->barge_in_enabled = parsed != 0U;

  status = ReadEnvironmentUint("BOOMPI_LEGACY_DOWNLINK_FRAMING", 0U, 0U,
                               1U, &parsed);
  if (!status.ok()) {
    return status;
  }
  output->legacy_downlink_framing = parsed != 0U;

  status = ReadEnvironmentString("BOOMPI_SNOWBOY_RESOURCE_FILE", false,
                                 1024U, &output->snowboy_resource_path);
  if (!status.ok()) {
    return status;
  }
  status = ReadEnvironmentString("BOOMPI_SNOWBOY_MODEL_FILE", false,
                                 1024U, &output->snowboy_model_path);
  if (!status.ok()) {
    return status;
  }
  status = ReadEnvironmentString("BOOMPI_SNOWBOY_SENSITIVITY", false,
                                 32U, &output->snowboy_sensitivity);
  if (!status.ok()) {
    return status;
  }
  if (output->snowboy_sensitivity.empty()) {
    output->snowboy_sensitivity = "0.5";
  }

  network::WssClientConfig wss_config{};
  wss_config.server_ip = output->server_ip;
  wss_config.server_port = output->server_port;
  wss_config.server_name = output->server_name;
  wss_config.spki_sha256_base64 = output->server_spki_sha256;
  return network::ValidateWssClientConfig(wss_config);
}

std::uint64_t MonotonicMicroseconds() noexcept {
  const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
  const auto microseconds =
      std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
  return microseconds > 0 ? static_cast<std::uint64_t>(microseconds) : 0U;
}

std::uint64_t ElapsedMicroseconds(
    const std::chrono::steady_clock::time_point start) noexcept {
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto microseconds =
      std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
  return microseconds > 0 ? static_cast<std::uint64_t>(microseconds) : 0U;
}

void UpdateMaximum(const std::uint64_t value,
                   std::uint64_t* const maximum) noexcept {
  if (maximum != nullptr && value > *maximum) {
    *maximum = value;
  }
}

void UpdateAtomicMaximum(const std::size_t value,
                         std::atomic<std::size_t>* const maximum) noexcept {
  if (maximum == nullptr) {
    return;
  }
  std::size_t observed = maximum->load(std::memory_order_relaxed);
  while (value > observed &&
         !maximum->compare_exchange_weak(observed, value,
                                         std::memory_order_release,
                                         std::memory_order_relaxed)) {
  }
}

std::int16_t ApplyPolarity(const std::int16_t sample,
                           const std::int8_t polarity) noexcept {
  if (polarity > 0) {
    return sample;
  }
  return sample == std::numeric_limits<std::int16_t>::min()
             ? std::numeric_limits<std::int16_t>::max()
             : static_cast<std::int16_t>(-sample);
}

void AppendJsonString(const std::string& value, std::string* const output) {
  output->push_back('"');
  for (const char current : value) {
    if (current == '"') {
      output->append("\\\"");
    } else if (current == '\\') {
      output->append("\\\\");
    } else {
      output->push_back(current);
    }
  }
  output->push_back('"');
}

std::string BuildControl(const char* const type,
                         const char* const message_id,
                         const std::string& device_id,
                         const std::uint32_t session_id,
                         const std::uint32_t turn_id,
                         const std::uint32_t stream_id,
                         const std::uint32_t epoch,
                         const std::string& payload) {
  std::string output;
  output.reserve(256U + payload.size());
  output.append("{\"version\":1,\"type\":\"").append(type);
  output.append("\",\"message_id\":\"").append(message_id);
  output.append("\",\"device_id\":\"").append(device_id);
  output.append("\",\"session_id\":").append(std::to_string(session_id));
  output.append(",\"turn_id\":").append(std::to_string(turn_id));
  output.append(",\"stream_id\":").append(std::to_string(stream_id));
  output.append(",\"epoch\":").append(std::to_string(epoch));
  output.append(",\"payload\":").append(payload).push_back('}');
  return output;
}

class ResponseWatchdog final {
 public:
  explicit ResponseWatchdog(network::WssClient* const client)
      : client_(client) {}

  ~ResponseWatchdog() { Stop(); }
  ResponseWatchdog(const ResponseWatchdog&) = delete;
  ResponseWatchdog& operator=(const ResponseWatchdog&) = delete;

  Status Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (client_ == nullptr || thread_.joinable()) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "response watchdog cannot be started");
    }
    stopped_ = false;
    timed_out_ = false;
    last_activity_ = std::chrono::steady_clock::now();
    try {
      thread_ = std::thread(&ResponseWatchdog::Run, this);
    } catch (const std::system_error&) {
      stopped_ = true;
      return Status::Error(StatusCode::kResourceExhausted,
                           "response watchdog thread could not start");
    }
    return Status::Ok();
  }

  void Touch() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopped_ || timed_out_) {
        return;
      }
      last_activity_ = std::chrono::steady_clock::now();
    }
    condition_.notify_all();
  }

  void Stop() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
    }
    condition_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  bool timed_out() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return timed_out_;
  }

 private:
  void Run() noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopped_) {
      const auto deadline =
          last_activity_ +
          std::chrono::milliseconds(kResponseInactivityTimeoutMs);
      if (condition_.wait_until(lock, deadline) !=
          std::cv_status::timeout) {
        continue;
      }
      if (stopped_ || std::chrono::steady_clock::now() < deadline) {
        continue;
      }
      timed_out_ = true;
      lock.unlock();
      client_->RequestStop();
      return;
    }
  }

  network::WssClient* client_{nullptr};
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::thread thread_;
  std::chrono::steady_clock::time_point last_activity_{};
  bool stopped_{true};
  bool timed_out_{false};
};

class ManualSingleTurnSession final {
 public:
  explicit ManualSingleTurnSession(const ManualConfig& config)
      : config_(config),
        workspace_(config.record_ms / kFrameDurationMs),
        client_(MakeWssConfig(config)),
        watchdog_(&client_) {}

  ~ManualSingleTurnSession() {
#if defined(BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH)
    StopBargeInCapture();
    StopVoiceCapturePump();
    voice_dsp_.Close();
#endif
    watchdog_.Stop();
    if (!completed_ && turn_started_ && !watchdog_.timed_out()) {
      BestEffortCancel();
    }
    StopPlaybackConsumer();
    client_.Close();
    io_.Abort();
    pending_pcm_.fill(0);
    SecureClear(&response_id_);
  }

  ManualSingleTurnSession(const ManualSingleTurnSession&) = delete;
  ManualSingleTurnSession& operator=(const ManualSingleTurnSession&) = delete;

  Status Run() {
    Status status = Initialize();
    if (!status.ok()) {
      return status;
    }
    ResetAudioTurnMetrics();
    status = CaptureAndUpload();
    if (!status.ok()) {
      return status;
    }
    status = watchdog_.Start();
    if (!status.ok()) {
      return status;
    }
    status = ReceiveResponse();
    if (!status.ok()) {
      return status;
    }
    completed_ = true;
    return Status::Ok();
  }

  Status RunVoiceLoop() {
#if !defined(BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH)
    return Status::Error(
        StatusCode::kNotSupported,
        "voice loop requires Snowboy, Rockchip 3A, and WebRTC VAD");
#else
    if (config_.snowboy_resource_path.empty() ||
        config_.snowboy_model_path.empty()) {
      return Status::Error(
          StatusCode::kInvalidArgument,
          "voice loop requires Snowboy resource and model paths");
    }
    Status status = OpenVoiceFrontEnd();
    if (!status.ok()) {
      return status;
    }
    status = Initialize();
    if (!status.ok()) {
      return status;
    }
    status = StartVoiceCapturePump();
    if (!status.ok()) {
      return status;
    }
    std::cout << "boompi-client: voice loop ready; wake_word=snowboy"
              << std::endl;

    bool require_wake = true;
    bool resume_confirmed_speech = false;
    for (;;) {
      bool captured = false;
      status = CaptureVoiceAndUpload(require_wake, resume_confirmed_speech,
                                     &captured);
      resume_confirmed_speech = false;
      if (!status.ok()) {
        if (turn_started_) {
          PrintAudioTurnMetrics("capture_error");
        }
        return status;
      }
      if (!captured) {
        require_wake = true;
        continue;
      }

      status = watchdog_.Start();
      if (!status.ok()) {
        if (config_.barge_in_enabled) {
          StopBargeInCapture();
        }
        return status;
      }
      status = ReceiveResponse();
      watchdog_.Stop();
      if (config_.barge_in_enabled) {
        StopBargeInCapture();
        if (barge_capture_failed_.load(std::memory_order_acquire)) {
          PrintAudioTurnMetrics("barge_capture_error");
          return Status::Error(StatusCode::kInternal,
                               "barge-in capture failed");
        }
        if (barge_capture_discontinuity_.load(std::memory_order_acquire)) {
          response_cancelled_for_capture_discontinuity_ = true;
        }
      }
      if (!status.ok()) {
        PrintAudioTurnMetrics("response_error");
        return status;
      }
      if (response_failed_) {
        PrintAudioTurnMetrics("provider_error");
        status = AdvanceVoiceGeneration();
        if (!status.ok()) {
          return status;
        }
        // A provider failure only loses this turn. Keep the session alive and
        // offer a short follow-up window before requiring the wake word again.
        require_wake = false;
        resume_confirmed_speech = false;
        continue;
      }
      const bool capture_discontinuity =
          response_cancelled_for_capture_discontinuity_;
      const bool interrupted =
          !capture_discontinuity &&
          (response_was_cancelled_ ||
           barge_confirmed_.load(std::memory_order_acquire));
      if (capture_discontinuity) {
        status = ResetVoiceCaptureAfterDiscontinuity();
        if (!status.ok()) {
          PrintAudioTurnMetrics("capture_reset_error");
          return status;
        }
      }
      PrintAudioTurnMetrics(capture_discontinuity
                                ? "capture_discontinuity"
                                : (interrupted ? "interrupted" : "completed"));
      status = AdvanceVoiceGeneration();
      if (!status.ok()) {
        return status;
      }
      if (capture_discontinuity) {
        AcknowledgeVoiceCaptureDiscontinuity();
      }
      require_wake = capture_discontinuity;
      resume_confirmed_speech = interrupted;
    }
#endif
  }

  Status Initialize() {
    audio::PlaybackGainLimiterConfig gain_config{};
    gain_config.volume_percent = config_.volume_percent;
    gain_config.speaker_gain_percent = config_.speaker_gain_percent;
    gain_config.duck_target_percent = 25U;
    gain_config.duck_ramp_ms = 80U;
    gain_config.recovery_ramp_ms = 80U;
    gain_config.limiter_ceiling_percent = 95U;
    gain_config.limiter_release_ms = 80U;
    Status status =
        audio::PlaybackRenderer24To48::Create(gain_config, &renderer_);
    if (!status.ok()) {
      return status;
    }

    status = platform::rv1106::AlsaSingleTurnIo::Open(
        {config_.capture_pcm, config_.playback_pcm}, &io_);
    if (!status.ok()) {
      return status;
    }
    status = StartPlaybackConsumer();
    if (!status.ok()) {
      return status;
    }
    status = client_.Connect();
    if (!status.ok()) {
      return status;
    }
    status = Hello();
    if (!status.ok()) {
      return status;
    }
    return Status::Ok();
  }

  void PrintSummary() {
    std::cout << "boompi-client: manual single turn completed; "
              << "uplink_frames=" << uplink_frames_
              << " downlink_frames=" << downlink_frames_
              << " playback_chunks=" << playback_chunks_
              << " text_delta_bytes=" << text_delta_bytes_ << '\n';
    PrintAudioTurnMetrics("completed");
  }

 private:
  using PlaybackQueue =
      std::array<audio::PlaybackPcmFrame48k, kPlaybackQueueCapacity>;

  void ResetAudioTurnMetrics() noexcept {
    capture_xrun_count_ = 0U;
    capture_max_latency_us_ = 0U;
    dsp_max_latency_us_ = 0U;
    send_max_latency_us_ = 0U;
    capture_queue_overrun_count_.store(0U, std::memory_order_release);
    capture_queue_high_water_frames_.store(0U, std::memory_order_release);
    playback_reference_overrun_count_.store(0U, std::memory_order_release);
    playback_reference_underrun_count_.store(0U, std::memory_order_release);
    playback_reference_queue_high_water_frames_.store(
        0U, std::memory_order_release);
    playback_queue_high_water_frames_ = 0U;
    first_downlink_pcm_us_ = 0U;
    first_playback_write_us_ = 0U;
#if defined(BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH)
    barge_confirmed_timestamp_us_.store(0U, std::memory_order_release);
    barge_cancel_sent_timestamp_us_.store(0U, std::memory_order_release);
    echo_gate_rejected_vad_frames_ = 0U;
    echo_gate_admitted_vad_frames_ = 0U;
#endif
  }

  void AddCaptureRecoveries(const std::uint32_t recovery_count) noexcept {
    capture_xrun_count_ =
        recovery_count >
                std::numeric_limits<std::uint32_t>::max() -
                    capture_xrun_count_
            ? std::numeric_limits<std::uint32_t>::max()
            : capture_xrun_count_ + recovery_count;
  }

  void PrintAudioTurnMetrics(const char* const outcome) {
    std::size_t queue_high_water = 0U;
    std::uint64_t first_pcm_us = 0U;
    std::uint64_t first_write_us = 0U;
    {
      std::lock_guard<std::mutex> lock(playback_mutex_);
      queue_high_water = playback_queue_high_water_frames_;
      first_pcm_us = first_downlink_pcm_us_;
      first_write_us = first_playback_write_us_;
    }
    std::cout << "boompi-client: audio turn metrics; outcome=" << outcome
              << " capture_xruns=" << capture_xrun_count_
              << " capture_max_us=" << capture_max_latency_us_
              << " dsp_max_us=" << dsp_max_latency_us_
              << " send_max_us=" << send_max_latency_us_
              << " capture_queue_overruns="
              << capture_queue_overrun_count_.load(std::memory_order_acquire)
              << " capture_queue_hwm_frames="
              << capture_queue_high_water_frames_.load(
                     std::memory_order_acquire)
              << " playback_ref_overruns="
              << playback_reference_overrun_count_.load(
                     std::memory_order_acquire)
              << " playback_ref_underruns="
              << playback_reference_underrun_count_.load(
                     std::memory_order_acquire)
              << " playback_ref_hwm_frames="
              << playback_reference_queue_high_water_frames_.load(
                     std::memory_order_acquire)
              << " playback_queue_hwm_frames=" << queue_high_water
              << " barge_confirm_to_cancel_us=";
#if defined(BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH)
    const std::uint64_t barge_confirmed_us =
        barge_confirmed_timestamp_us_.load(std::memory_order_acquire);
    const std::uint64_t barge_cancel_sent_us =
        barge_cancel_sent_timestamp_us_.load(std::memory_order_acquire);
    if (barge_confirmed_us != 0U &&
        barge_cancel_sent_us >= barge_confirmed_us) {
      std::cout << barge_cancel_sent_us - barge_confirmed_us;
    } else {
      std::cout << "unavailable";
    }
    std::cout << " echo_gate_rejected_vad_frames="
              << echo_gate_rejected_vad_frames_
              << " echo_gate_admitted_vad_frames="
              << echo_gate_admitted_vad_frames_;
#else
    std::cout << "unavailable";
#endif
    std::cout
              << " first_pcm_to_first_write_us=";
    if (first_pcm_us != 0U && first_write_us >= first_pcm_us) {
      std::cout << first_write_us - first_pcm_us;
    } else {
      std::cout << "unavailable";
    }
    std::cout << '\n';
  }

  void ClearPlaybackQueueLocked() noexcept {
    if (playback_queue_ != nullptr) {
      while (playback_queue_count_ != 0U) {
        auto& frame = (*playback_queue_)[playback_queue_head_];
        SecureZero(frame.samples.data(),
                   frame.samples.size() * sizeof(frame.samples[0]));
        frame.ResetHeader();
        playback_queue_head_ =
            (playback_queue_head_ + 1U) % kPlaybackQueueCapacity;
        --playback_queue_count_;
      }
    }
    playback_queue_head_ = 0U;
    playback_queue_tail_ = 0U;
  }

  Status StartPlaybackConsumer() {
    playback_queue_.reset(new (std::nothrow) PlaybackQueue{});
    if (playback_queue_ == nullptr) {
      return Status::Error(StatusCode::kResourceExhausted,
                           "playback jitter queue allocation failed");
    }
    try {
      playback_thread_ = std::thread([this]() { PlaybackConsumerLoop(); });
    } catch (const std::system_error&) {
      playback_queue_.reset();
      return Status::Error(StatusCode::kResourceExhausted,
                           "playback consumer thread could not start");
    }
    return Status::Ok();
  }

  void StopPlaybackConsumer() noexcept {
    {
      std::lock_guard<std::mutex> lock(playback_mutex_);
      playback_stop_ = true;
      ClearPlaybackQueueLocked();
    }
    playback_condition_.notify_all();
    if (playback_thread_.joinable()) {
      playback_thread_.join();
    }
    playback_queue_.reset();
  }

  Status BeginPlaybackResponse() {
    {
      std::lock_guard<std::mutex> lock(playback_mutex_);
      if (!playback_thread_.joinable() || playback_stop_ ||
          playback_response_active_ || playback_queue_count_ != 0U) {
        return Status::Error(StatusCode::kFailedPrecondition,
                             "playback jitter queue is not ready");
      }
      playback_error_ = Status::Ok();
      playback_producer_done_ = false;
      playback_drop_requested_ = false;
      playback_started_ = false;
      playback_response_complete_ = false;
      playback_response_active_ = true;
    }
#if defined(BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH)
    BeginPlaybackReference();
#endif
    return Status::Ok();
  }

  Status EnqueuePlayback(audio::PlaybackPcmFrame48k* const frame) {
    if (frame == nullptr || !frame->HasValidLength()) {
      return Status::Error(StatusCode::kInvalidArgument,
                           "rendered playback frame is invalid");
    }
    std::unique_lock<std::mutex> lock(playback_mutex_);
    if (!playback_error_.ok()) {
      return playback_error_;
    }
#if defined(BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH)
    const auto discard_after_confirmed_barge = [this, frame]() noexcept {
      if (!barge_confirmed_.load(std::memory_order_acquire)) {
        return false;
      }
      SecureZero(frame->samples.data(),
                 frame->samples.size() * sizeof(frame->samples[0]));
      frame->ResetHeader();
      return true;
    };
    if (discard_after_confirmed_barge()) {
      return Status::Ok();
    }
#endif
    if (!playback_response_active_ || playback_producer_done_) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "playback response is not accepting audio");
    }
    if (playback_queue_count_ == kPlaybackQueueCapacity) {
      const bool queue_ready = playback_condition_.wait_for(
          lock, std::chrono::milliseconds(kPlaybackEnqueueTimeoutMs), [this]() {
            return playback_stop_ || !playback_error_.ok() ||
                   !playback_response_active_ ||
                   playback_queue_count_ < kPlaybackQueueCapacity;
          });
      if (!queue_ready) {
#if defined(BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH)
        if (discard_after_confirmed_barge()) {
          return Status::Ok();
        }
#endif
        return Status::Error(StatusCode::kResourceExhausted,
                             "playback jitter queue is full");
      }
    }
    if (!playback_error_.ok()) {
      return playback_error_;
    }
#if defined(BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH)
    if (discard_after_confirmed_barge()) {
      return Status::Ok();
    }
#endif
    if (playback_stop_ || !playback_response_active_ ||
        playback_producer_done_) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "playback response stopped while enqueueing");
    }
    (*playback_queue_)[playback_queue_tail_] = *frame;
    playback_queue_tail_ =
        (playback_queue_tail_ + 1U) % kPlaybackQueueCapacity;
    ++playback_queue_count_;
    playback_queue_high_water_frames_ =
        std::max(playback_queue_high_water_frames_, playback_queue_count_);
    playback_condition_.notify_one();
    return Status::Ok();
  }

  Status FinishPlaybackResponse(const bool drop) {
    std::unique_lock<std::mutex> lock(playback_mutex_);
    if (!playback_error_.ok()) {
      return playback_error_;
    }
    if (!playback_response_active_) {
#if defined(BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH)
      if (drop || barge_confirmed_.load(std::memory_order_acquire)) {
        return Status::Ok();
      }
#else
      if (drop) {
        return Status::Ok();
      }
#endif
      return Status::Error(StatusCode::kFailedPrecondition,
                           "playback response is not active");
    }
    playback_producer_done_ = true;
    playback_drop_requested_ = playback_drop_requested_ || drop;
    if (playback_drop_requested_) {
      ClearPlaybackQueueLocked();
    }
    playback_condition_.notify_all();
    const bool completed = playback_condition_.wait_for(
        lock, std::chrono::milliseconds(kPlaybackCompletionTimeoutMs),
        [this]() {
          return playback_response_complete_ || playback_stop_;
        });
    if (!completed) {
      return Status::Error(StatusCode::kResourceExhausted,
                           "playback completion timed out");
    }
    if (!playback_error_.ok()) {
      return playback_error_;
    }
    return playback_stop_
               ? Status::Error(StatusCode::kFailedPrecondition,
                               "playback consumer stopped")
               : Status::Ok();
  }

  Status PlaybackError() {
    std::lock_guard<std::mutex> lock(playback_mutex_);
    return playback_error_;
  }

  void PlaybackConsumerLoop() {
    for (;;) {
      audio::PlaybackPcmFrame48k frame{};
      bool write_frame = false;
      bool record_first_write = false;
      bool drain = false;
      bool drop = false;
      {
        std::unique_lock<std::mutex> lock(playback_mutex_);
        playback_condition_.wait(lock, [this]() {
          return playback_stop_ ||
                 (playback_response_active_ &&
                  (playback_drop_requested_ ||
                   (playback_started_ &&
                    (playback_queue_count_ != 0U || playback_producer_done_)) ||
                   (!playback_started_ &&
                    (playback_queue_count_ >= kPlaybackPrebufferFrames ||
                     playback_producer_done_))));
        });
        if (playback_stop_) {
          return;
        }
        if (playback_drop_requested_) {
          drop = true;
        } else {
          playback_started_ = true;
          if (playback_queue_count_ != 0U) {
            auto& queued = (*playback_queue_)[playback_queue_head_];
            frame = queued;
            SecureZero(queued.samples.data(),
                       queued.samples.size() * sizeof(queued.samples[0]));
            queued.ResetHeader();
            playback_queue_head_ =
                (playback_queue_head_ + 1U) % kPlaybackQueueCapacity;
            --playback_queue_count_;
            playback_condition_.notify_all();
            write_frame = true;
            record_first_write = first_playback_write_us_ == 0U;
          } else {
            drain = playback_producer_done_;
          }
        }
      }

      Status status = Status::Ok();
      if (write_frame) {
        bool recovered_playback_xrun = false;
        status = io_.WriteMono48k(frame.samples.data(), frame.valid_samples,
                                  kPlaybackOperationTimeoutMs,
                                  &recovered_playback_xrun);
        if (status.ok()) {
          if (record_first_write) {
            const std::uint64_t first_write_us = MonotonicMicroseconds();
            std::lock_guard<std::mutex> lock(playback_mutex_);
            if (first_playback_write_us_ == 0U) {
              first_playback_write_us_ = first_write_us;
            }
          }
#if defined(BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH)
          if (recovered_playback_xrun) {
            ClearPlaybackReference();
          } else {
            PublishPlaybackReference48k(frame.samples.data(),
                                        frame.valid_samples);
          }
#endif
          ++playback_chunks_;
        }
        SecureZero(frame.samples.data(),
                   frame.samples.size() * sizeof(frame.samples[0]));
        if (status.ok()) {
          continue;
        }
      } else if (drop) {
        status = io_.DropPlayback();
#if defined(BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH)
        ClearPlaybackReference();
#endif
      } else if (drain) {
#if defined(BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH)
        MarkPlaybackReferenceProducerDone();
#endif
        status = io_.DrainPlayback(kPlaybackDrainTimeoutMs);
      } else {
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(playback_mutex_);
        if (!status.ok()) {
          playback_error_ = status;
        }
        ClearPlaybackQueueLocked();
        playback_response_active_ = false;
        playback_response_complete_ = true;
      }
      playback_condition_.notify_all();
      if (!status.ok()) {
#if defined(BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH)
        ClearPlaybackReference();
#endif
        client_.RequestStop();
      }
    }
  }

#if defined(BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH)
  Status OpenVoiceFrontEnd() {
    if (voice_dsp_.Open() !=
        platform::rv1106::RockchipVoiceDspStatus::kOk) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "Rockchip voice DSP could not open");
    }
    platform::rv1106::WebRtcVadConfig vad_config{};
    vad_config.mode = 3;
    vad_config.speech_start_ms = 120U;
    vad_config.speech_end_ms = 500U;
    if (!voice_vad_.Open(vad_config)) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "WebRTC VAD could not open");
    }
    platform::rv1106::SnowboyWakeWordConfig wake_config{};
    wake_config.resource_path = config_.snowboy_resource_path;
    wake_config.model_path = config_.snowboy_model_path;
    wake_config.sensitivity = config_.snowboy_sensitivity;
    platform::rv1106::SnowboyWakeWordCreateResult create_result{};
    wake_engine_ = platform::rv1106::SnowboyWakeWordEngine::Create(
        wake_config, &create_result);
    if (!wake_engine_ || !create_result.ok()) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "Snowboy wake-word engine could not open");
    }
    ClearPlaybackReference();
    ResetEchoGate();
    return Status::Ok();
  }

  void ClearPlaybackReferenceQueueLocked() noexcept {
    for (auto& frame : playback_reference_queue_) {
      frame.fill(0);
    }
    playback_reference_queue_head_ = 0U;
    playback_reference_queue_tail_ = 0U;
    playback_reference_queue_count_ = 0U;
    playback_reference_consumption_started_ = false;
  }

  void BeginPlaybackReference() noexcept {
    std::lock_guard<std::mutex> lock(playback_reference_mutex_);
    playback_reference_decimator_.Reset();
    playback_reference_input_ = {};
    playback_reference_output_ = {};
    playback_reference_samples_ = 0U;
    ClearPlaybackReferenceQueueLocked();
    playback_reference_stream_active_ = true;
    playback_reference_producer_done_ = false;
    barge_audio_armed_.store(false, std::memory_order_release);
  }

  void StorePlaybackReferenceLocked(const VoiceSamples& samples) noexcept {
    if (!playback_reference_stream_active_) {
      return;
    }
    if (playback_reference_queue_count_ ==
        kPlaybackReferenceQueueCapacity) {
      ClearPlaybackReferenceQueueLocked();
      playback_reference_overrun_count_.fetch_add(1U,
                                                   std::memory_order_relaxed);
      barge_audio_armed_.store(false, std::memory_order_release);
    }
    playback_reference_queue_[playback_reference_queue_tail_] = samples;
    playback_reference_queue_tail_ =
        (playback_reference_queue_tail_ + 1U) %
        kPlaybackReferenceQueueCapacity;
    ++playback_reference_queue_count_;
    UpdateAtomicMaximum(playback_reference_queue_count_,
                        &playback_reference_queue_high_water_frames_);
  }

  bool TakePlaybackReference(VoiceSamples* const output) noexcept {
    if (output == nullptr) {
      return false;
    }
    output->fill(0);
    std::lock_guard<std::mutex> lock(playback_reference_mutex_);
    if (!playback_reference_stream_active_) {
      return false;
    }
    if (!playback_reference_consumption_started_) {
      const bool reference_lead_ready =
          playback_reference_queue_count_ >= kPlaybackReferenceLeadFrames;
      const bool short_response_ready =
          playback_reference_producer_done_ &&
          playback_reference_queue_count_ != 0U;
      if (!reference_lead_ready && !short_response_ready) {
        return false;
      }
      playback_reference_consumption_started_ = true;
    }
    if (playback_reference_queue_count_ == 0U) {
      playback_reference_consumption_started_ = false;
      barge_audio_armed_.store(false, std::memory_order_release);
      if (!playback_reference_producer_done_) {
        playback_reference_underrun_count_.fetch_add(
            1U, std::memory_order_relaxed);
      }
      return false;
    }
    auto& frame = playback_reference_queue_[playback_reference_queue_head_];
    *output = frame;
    frame.fill(0);
    playback_reference_queue_head_ =
        (playback_reference_queue_head_ + 1U) %
        kPlaybackReferenceQueueCapacity;
    --playback_reference_queue_count_;
    if (playback_reference_producer_done_ &&
        playback_reference_queue_count_ == 0U) {
      playback_reference_consumption_started_ = false;
      barge_audio_armed_.store(false, std::memory_order_release);
    }
    return true;
  }

  void PublishPlaybackReference48k(const std::int16_t* samples,
                                   const std::uint16_t sample_frames) noexcept {
    if (samples == nullptr || sample_frames == 0U) {
      return;
    }
    std::lock_guard<std::mutex> lock(playback_reference_mutex_);
    if (!playback_reference_stream_active_ ||
        playback_reference_producer_done_) {
      return;
    }
    std::size_t source = 0U;
    while (source < sample_frames) {
      const std::size_t available =
          audio::kSamplesPer48k20ms - playback_reference_samples_;
      const std::size_t copied =
          std::min<std::size_t>(available, sample_frames - source);
      std::copy_n(samples + source, copied,
                  playback_reference_input_[0U].begin() +
                      playback_reference_samples_);
      playback_reference_samples_ += copied;
      source += copied;
      if (playback_reference_samples_ != audio::kSamplesPer48k20ms) {
        continue;
      }
      const auto transformed = playback_reference_decimator_.Process(
          playback_reference_input_, &playback_reference_output_);
      if (transformed.ok()) {
        VoiceSamples reference{};
        std::copy(playback_reference_output_[0U].begin(),
                  playback_reference_output_[0U].end(), reference.begin());
        StorePlaybackReferenceLocked(reference);
      }
      playback_reference_input_ = {};
      playback_reference_output_ = {};
      playback_reference_samples_ = 0U;
    }
  }

  void MarkPlaybackReferenceProducerDone() noexcept {
    std::lock_guard<std::mutex> lock(playback_reference_mutex_);
    playback_reference_producer_done_ = true;
    if (playback_reference_queue_count_ == 0U) {
      playback_reference_consumption_started_ = false;
      barge_audio_armed_.store(false, std::memory_order_release);
    }
  }

  void ArmBargeAfterAlignedPlaybackReference() noexcept {
    std::lock_guard<std::mutex> lock(playback_reference_mutex_);
    if (playback_reference_stream_active_ &&
        playback_reference_consumption_started_) {
      barge_audio_armed_.store(true, std::memory_order_release);
    }
  }

  void ClearPlaybackReference() noexcept {
    std::lock_guard<std::mutex> lock(playback_reference_mutex_);
    playback_reference_decimator_.Reset();
    playback_reference_input_ = {};
    playback_reference_output_ = {};
    playback_reference_samples_ = 0U;
    ClearPlaybackReferenceQueueLocked();
    playback_reference_stream_active_ = false;
    playback_reference_producer_done_ = false;
    barge_audio_armed_.store(false, std::memory_order_release);
  }

  static double FrameMeanSquare(const VoiceSamples& samples) noexcept {
    double sum = 0.0;
    for (const std::int16_t sample : samples) {
      const double value = static_cast<double>(sample);
      sum += value * value;
    }
    return sum / static_cast<double>(samples.size());
  }

  void ResetEchoAdmission() noexcept {
    echo_admission_evidence_ = 0U;
    echo_admission_hold_frames_ = 0U;
  }

  void ResetEchoGateRuntime() noexcept {
    echo_previous_microphone_mean_square_ = 0.0;
    echo_previous_reference_mean_square_ = 0.0;
    voice_frame_output_mean_square_ = 0.0;
    echo_previous_input_valid_ = false;
    echo_previous_reference_aligned_ = false;
    echo_reference_was_active_ = false;
    echo_tail_frames_remaining_ = 0U;
    echo_reference_envelope_mean_square_ = 0.0;
    voice_frame_echo_context_ = false;
    voice_frame_near_speech_allowed_ = true;
    voice_frame_near_speech_energy_evidence_ = false;
    ResetEchoAdmission();
  }

  void ResetEchoGate() noexcept {
    ResetEchoGateRuntime();
    echo_calibration_frames_ = 0U;
    echo_mic_to_reference_baseline_ = 0.0;
    echo_output_to_reference_baseline_ = 0.0;
    echo_output_noise_floor_mean_square_ = 1.0;
    echo_output_noise_floor_initialized_ = false;
    echo_output_noise_floor_sum_ = 0.0;
    echo_quiet_observation_frames_ = 0U;
  }

  void BeginEchoCalibration(const double reference_mean_square) noexcept {
    echo_reference_envelope_mean_square_ = reference_mean_square;
    ResetEchoAdmission();
  }

  void UpdateEchoOutputNoiseFloor(const double output_mean_square) noexcept {
    if (output_mean_square <= 0.0) {
      return;
    }
    if (!echo_output_noise_floor_initialized_) {
      echo_output_noise_floor_sum_ += output_mean_square;
      ++echo_quiet_observation_frames_;
      if (echo_quiet_observation_frames_ >= kEchoQuietBootstrapFrames) {
        echo_output_noise_floor_mean_square_ = std::max(
            1.0, echo_output_noise_floor_sum_ /
                     static_cast<double>(echo_quiet_observation_frames_));
        echo_output_noise_floor_initialized_ = true;
        echo_output_noise_floor_sum_ = 0.0;
        echo_quiet_observation_frames_ = 0U;
      }
      return;
    }
    const double bounded_mean_square =
        std::clamp(output_mean_square,
                   echo_output_noise_floor_mean_square_ * 0.5,
                   echo_output_noise_floor_mean_square_ * 2.0);
    const double weight =
        bounded_mean_square < echo_output_noise_floor_mean_square_ ? 0.20
                                                                   : 0.005;
    echo_output_noise_floor_mean_square_ +=
        weight *
        (bounded_mean_square - echo_output_noise_floor_mean_square_);
    echo_output_noise_floor_mean_square_ =
        std::max(1.0, echo_output_noise_floor_mean_square_);
  }

  void UpdateEchoGateForProcessedFrame(
      const VoiceSamples& processed) noexcept {
    voice_frame_echo_context_ = false;
    voice_frame_near_speech_allowed_ = true;
    voice_frame_near_speech_energy_evidence_ = false;
    const double output_mean_square = FrameMeanSquare(processed);
    voice_frame_output_mean_square_ = output_mean_square;
    if (!echo_previous_input_valid_) {
      return;
    }

    const double reference_mean_square =
        echo_previous_reference_mean_square_;
    const bool reference_active =
        echo_previous_reference_aligned_ &&
        reference_mean_square >= kEchoReferenceActiveMeanSquare;
    if (reference_active && !echo_reference_was_active_) {
      BeginEchoCalibration(reference_mean_square);
    }
    echo_reference_was_active_ = reference_active;
    if (reference_active) {
      echo_tail_frames_remaining_ = kEchoTailFrames;
      echo_reference_envelope_mean_square_ =
          std::max(reference_mean_square,
                   echo_reference_envelope_mean_square_ *
                       kEchoReferenceEnvelopeDecay);
      voice_frame_echo_context_ = true;
    } else if (echo_tail_frames_remaining_ != 0U) {
      --echo_tail_frames_remaining_;
      echo_reference_envelope_mean_square_ *=
          kEchoReferenceEnvelopeDecay;
      voice_frame_echo_context_ = true;
    }

    if (!voice_frame_echo_context_) {
      ResetEchoAdmission();
      return;
    }

    const double microphone_mean_square =
        echo_previous_microphone_mean_square_;
    if (reference_active &&
        echo_calibration_frames_ < kBargeAecWarmupFrames) {
      const double microphone_ratio =
          microphone_mean_square / reference_mean_square;
      const double output_ratio = output_mean_square / reference_mean_square;
      const double weight = echo_calibration_frames_ == 0U ? 1.0 : 0.10;
      echo_mic_to_reference_baseline_ +=
          weight * (microphone_ratio - echo_mic_to_reference_baseline_);
      echo_output_to_reference_baseline_ +=
          weight * (output_ratio - echo_output_to_reference_baseline_);
      ++echo_calibration_frames_;
      voice_frame_near_speech_allowed_ = false;
      ResetEchoAdmission();
      return;
    }
    if (echo_calibration_frames_ < kBargeAecWarmupFrames) {
      voice_frame_near_speech_allowed_ = false;
      ResetEchoAdmission();
      return;
    }

    const double predicted_microphone =
        std::max(1.0, echo_mic_to_reference_baseline_ *
                          echo_reference_envelope_mean_square_);
    const double predicted_output =
        std::max(echo_output_noise_floor_mean_square_,
                 echo_output_to_reference_baseline_ *
                     echo_reference_envelope_mean_square_);
    const bool microphone_excess =
        microphone_mean_square > predicted_microphone *
                                     kEchoEnergyExcessRatio;
    const bool output_excess =
        output_mean_square > predicted_output * kEchoEnergyExcessRatio;
    const bool strongly_excess_output =
        output_mean_square > predicted_output * kEchoStrongOutputRatio &&
        microphone_mean_square >
            predicted_microphone * kEchoStrongMicrophoneExcessRatio;
    voice_frame_near_speech_energy_evidence_ =
        (output_excess && microphone_excess) || strongly_excess_output;
  }

  void UpdateEchoAdmission(const bool vad_speech_now) noexcept {
    if (!voice_frame_echo_context_) {
      voice_frame_near_speech_allowed_ = true;
      ResetEchoAdmission();
      ObserveEchoGateVad(vad_speech_now);
      return;
    }
    const bool near_speech_evidence =
        vad_speech_now && voice_frame_near_speech_energy_evidence_;
    echo_admission_evidence_ = static_cast<std::uint8_t>(
        ((echo_admission_evidence_ << 1U) |
         static_cast<std::uint8_t>(near_speech_evidence)) &
        0x07U);
    const unsigned evidence_count =
        static_cast<unsigned>(echo_admission_evidence_ & 0x01U) +
        static_cast<unsigned>((echo_admission_evidence_ >> 1U) & 0x01U) +
        static_cast<unsigned>((echo_admission_evidence_ >> 2U) & 0x01U);
    if (evidence_count >= 2U) {
      echo_admission_hold_frames_ = kEchoAdmissionHoldFrames;
    }
    voice_frame_near_speech_allowed_ =
        echo_admission_hold_frames_ != 0U;
    if (echo_admission_hold_frames_ != 0U) {
      --echo_admission_hold_frames_;
    }
  }

  void ObserveEchoGateVad(const bool speech_now) noexcept {
    if (!voice_frame_echo_context_ && !speech_now &&
        echo_previous_input_valid_) {
      UpdateEchoOutputNoiseFloor(voice_frame_output_mean_square_);
    } else if (speech_now && !echo_output_noise_floor_initialized_) {
      echo_output_noise_floor_sum_ = 0.0;
      echo_quiet_observation_frames_ = 0U;
    }
  }

  void RememberEchoGateInput(
      const VoiceSamples& microphone_left,
      const VoiceSamples& microphone_right,
      const VoiceSamples& playback_reference,
      const bool playback_reference_aligned) noexcept {
    echo_previous_microphone_mean_square_ =
        std::max(FrameMeanSquare(microphone_left),
                 FrameMeanSquare(microphone_right));
    echo_previous_reference_mean_square_ =
        FrameMeanSquare(playback_reference);
    echo_previous_reference_aligned_ = playback_reference_aligned;
    echo_previous_input_valid_ = true;
  }

  void ClearVoiceCaptureQueueLocked() noexcept {
    while (voice_capture_queue_count_ != 0U) {
      auto& slot = voice_capture_queue_[voice_capture_queue_head_];
      voice_capture_pending_max_latency_us_ =
          std::max(voice_capture_pending_max_latency_us_,
                   slot.read_latency_us);
      SecureZero(slot.period.data(),
                 slot.period.size() * sizeof(slot.period[0]));
      slot.read_result = {};
      slot.playback_reference.fill(0);
      slot.capture_timestamp_us = 0U;
      slot.read_latency_us = 0U;
      slot.playback_reference_aligned = false;
      voice_capture_queue_head_ =
          (voice_capture_queue_head_ + 1U) % kVoiceCaptureQueueCapacity;
      --voice_capture_queue_count_;
    }
    voice_capture_queue_head_ = 0U;
    voice_capture_queue_tail_ = 0U;
  }

  Status StartVoiceCapturePump() {
    {
      std::lock_guard<std::mutex> lock(voice_capture_mutex_);
      if (voice_capture_thread_.joinable() || !voice_capture_stop_) {
        return Status::Error(StatusCode::kFailedPrecondition,
                             "voice capture pump is already running");
      }
      ClearVoiceCaptureQueueLocked();
      voice_capture_pending_recoveries_ = 0U;
      voice_capture_pending_overruns_ = 0U;
      voice_capture_pending_max_latency_us_ = 0U;
      voice_capture_discontinuity_ = false;
      voice_capture_error_ = Status::Ok();
      voice_capture_stop_ = false;
    }
    try {
      voice_capture_thread_ =
          std::thread([this]() { VoiceCapturePumpLoop(); });
    } catch (const std::system_error&) {
      std::lock_guard<std::mutex> lock(voice_capture_mutex_);
      voice_capture_stop_ = true;
      return Status::Error(StatusCode::kResourceExhausted,
                           "voice capture pump thread could not start");
    }
    return Status::Ok();
  }

  void StopVoiceCapturePump() noexcept {
    {
      std::lock_guard<std::mutex> lock(voice_capture_mutex_);
      voice_capture_stop_ = true;
    }
    voice_capture_condition_.notify_all();
    if (voice_capture_thread_.joinable()) {
      voice_capture_thread_.join();
    }
    std::lock_guard<std::mutex> lock(voice_capture_mutex_);
    ClearVoiceCaptureQueueLocked();
    voice_capture_pending_recoveries_ = 0U;
    voice_capture_pending_overruns_ = 0U;
    voice_capture_pending_max_latency_us_ = 0U;
    voice_capture_discontinuity_ = false;
    voice_capture_error_ = Status::Ok();
  }

  void VoiceCapturePumpLoop() noexcept {
    for (;;) {
      platform::rv1106::AlsaSingleTurnIo::CapturePeriod period{};
      platform::rv1106::AlsaCaptureReadResult read_result{};
      VoiceSamples playback_reference{};
      const auto read_started = std::chrono::steady_clock::now();
      Status status = io_.Capture20Ms(&period, kCaptureOperationTimeoutMs,
                                      &read_result);
      const std::uint64_t capture_timestamp_us = MonotonicMicroseconds();
      const std::uint64_t read_latency_us =
          ElapsedMicroseconds(read_started);
      bool playback_reference_aligned = false;
      if (status.ok() && capture_timestamp_us != 0U) {
        if (read_result.discontinuity) {
          ClearPlaybackReference();
        } else {
          playback_reference_aligned =
              TakePlaybackReference(&playback_reference);
        }
      }

      {
        std::lock_guard<std::mutex> lock(voice_capture_mutex_);
        if (voice_capture_stop_) {
          SecureZero(period.data(), period.size() * sizeof(period[0]));
          playback_reference.fill(0);
          return;
        }
        if (!status.ok()) {
          voice_capture_error_ = std::move(status);
          SecureZero(period.data(), period.size() * sizeof(period[0]));
          playback_reference.fill(0);
          voice_capture_condition_.notify_all();
          return;
        }
        if (capture_timestamp_us == 0U) {
          voice_capture_error_ = Status::Error(
              StatusCode::kInternal,
              "voice capture pump monotonic clock is unavailable");
          SecureZero(period.data(), period.size() * sizeof(period[0]));
          playback_reference.fill(0);
          voice_capture_condition_.notify_all();
          return;
        }
        if (voice_capture_discontinuity_) {
          const std::uint64_t recovery_total =
              static_cast<std::uint64_t>(voice_capture_pending_recoveries_) +
              read_result.recovery_count;
          voice_capture_pending_recoveries_ = static_cast<std::uint32_t>(
              std::min<std::uint64_t>(
                  recovery_total,
                  std::numeric_limits<std::uint32_t>::max()));
          voice_capture_pending_max_latency_us_ =
              std::max(voice_capture_pending_max_latency_us_,
                       read_latency_us);
          SecureZero(period.data(), period.size() * sizeof(period[0]));
          playback_reference.fill(0);
          voice_capture_condition_.notify_all();
          continue;
        }
        if (read_result.discontinuity) {
          ClearVoiceCaptureQueueLocked();
          const std::uint64_t recovery_total =
              static_cast<std::uint64_t>(voice_capture_pending_recoveries_) +
              read_result.recovery_count;
          voice_capture_pending_recoveries_ = static_cast<std::uint32_t>(
              std::min<std::uint64_t>(
                  recovery_total,
                  std::numeric_limits<std::uint32_t>::max()));
          voice_capture_pending_max_latency_us_ =
              std::max(voice_capture_pending_max_latency_us_,
                       read_latency_us);
          voice_capture_discontinuity_ = true;
          SecureZero(period.data(), period.size() * sizeof(period[0]));
          playback_reference.fill(0);
          voice_capture_condition_.notify_all();
          continue;
        }
        if (voice_capture_queue_count_ == kVoiceCaptureQueueCapacity) {
          ClearVoiceCaptureQueueLocked();
          ClearPlaybackReference();
          voice_capture_pending_max_latency_us_ =
              std::max(voice_capture_pending_max_latency_us_,
                       read_latency_us);
          voice_capture_discontinuity_ = true;
          if (voice_capture_pending_overruns_ !=
              std::numeric_limits<std::uint64_t>::max()) {
            ++voice_capture_pending_overruns_;
          }
          SecureZero(period.data(), period.size() * sizeof(period[0]));
          playback_reference.fill(0);
          voice_capture_condition_.notify_all();
          continue;
        }

        auto& slot = voice_capture_queue_[voice_capture_queue_tail_];
        slot.period = period;
        slot.read_result = read_result;
        slot.playback_reference = playback_reference;
        slot.capture_timestamp_us = capture_timestamp_us;
        slot.read_latency_us = read_latency_us;
        slot.playback_reference_aligned = playback_reference_aligned;
        voice_capture_queue_tail_ =
            (voice_capture_queue_tail_ + 1U) % kVoiceCaptureQueueCapacity;
        ++voice_capture_queue_count_;
        UpdateAtomicMaximum(voice_capture_queue_count_,
                            &capture_queue_high_water_frames_);
      }
      SecureZero(period.data(), period.size() * sizeof(period[0]));
      playback_reference.fill(0);
      voice_capture_condition_.notify_one();
    }
  }

  Status TakeVoiceCapture(
      platform::rv1106::AlsaSingleTurnIo::CapturePeriod* const output,
      platform::rv1106::AlsaCaptureReadResult* const read_result,
      VoiceSamples* const playback_reference,
      bool* const playback_reference_aligned,
      std::uint64_t* const capture_timestamp_us,
      std::uint64_t* const read_latency_us,
      bool* const capture_discontinuity) {
    if (output == nullptr || read_result == nullptr ||
        playback_reference == nullptr || playback_reference_aligned == nullptr ||
        capture_timestamp_us == nullptr || read_latency_us == nullptr ||
        capture_discontinuity == nullptr) {
      return Status::Error(StatusCode::kInvalidArgument,
                           "voice capture pump output is invalid");
    }
    *read_result = {};
    playback_reference->fill(0);
    *playback_reference_aligned = false;
    *capture_timestamp_us = 0U;
    *read_latency_us = 0U;
    *capture_discontinuity = false;

    std::unique_lock<std::mutex> lock(voice_capture_mutex_);
    voice_capture_condition_.wait(lock, [this]() {
      return voice_capture_stop_ || !voice_capture_error_.ok() ||
             voice_capture_discontinuity_ ||
             voice_capture_queue_count_ != 0U;
    });
    if (!voice_capture_error_.ok()) {
      return voice_capture_error_;
    }
    if (voice_capture_stop_) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "voice capture pump stopped");
    }
    if (voice_capture_discontinuity_) {
      UpdateAtomicMaximum(voice_capture_queue_count_,
                          &capture_queue_high_water_frames_);
      ClearVoiceCaptureQueueLocked();
      output->fill(0);
      read_result->recovery_count = voice_capture_pending_recoveries_;
      read_result->discontinuity = true;
      *read_latency_us = voice_capture_pending_max_latency_us_;
      *capture_discontinuity = true;
      capture_queue_overrun_count_.fetch_add(
          voice_capture_pending_overruns_, std::memory_order_relaxed);
      return Status::Ok();
    }

    auto& slot = voice_capture_queue_[voice_capture_queue_head_];
    *output = slot.period;
    *read_result = slot.read_result;
    *playback_reference = slot.playback_reference;
    *playback_reference_aligned = slot.playback_reference_aligned;
    *capture_timestamp_us = slot.capture_timestamp_us;
    *read_latency_us = slot.read_latency_us;
    SecureZero(slot.period.data(),
               slot.period.size() * sizeof(slot.period[0]));
    slot.read_result = {};
    slot.playback_reference.fill(0);
    slot.capture_timestamp_us = 0U;
    slot.read_latency_us = 0U;
    slot.playback_reference_aligned = false;
    voice_capture_queue_head_ =
        (voice_capture_queue_head_ + 1U) % kVoiceCaptureQueueCapacity;
    --voice_capture_queue_count_;
    return Status::Ok();
  }

  Status CaptureProcessedVoice(audio::Mono16kFrame* const output,
                               const bool apply_voice_dsp,
                               bool* const capture_discontinuity) {
    if (output == nullptr || capture_discontinuity == nullptr ||
        !wake_engine_ || !voice_dsp_.is_open()) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "voice capture front end is unavailable");
    }
    *capture_discontinuity = false;
    platform::rv1106::AlsaCaptureReadResult capture_result{};
    bool aligned_playback_reference = false;
    std::uint64_t capture_timestamp_us = 0U;
    std::uint64_t capture_latency_us = 0U;
    Status status = TakeVoiceCapture(
        &workspace_.capture_period, &capture_result,
        &voice_playback_reference_, &aligned_playback_reference,
        &capture_timestamp_us, &capture_latency_us, capture_discontinuity);
    UpdateMaximum(capture_latency_us, &capture_max_latency_us_);
    AddCaptureRecoveries(capture_result.recovery_count);
    if (!status.ok()) {
      return status;
    }
    if (*capture_discontinuity || capture_result.discontinuity) {
      SecureZero(workspace_.capture_period.data(),
                 workspace_.capture_period.size() *
                     sizeof(workspace_.capture_period[0]));
      SecureZero(workspace_.input_planes.data(),
                 sizeof(workspace_.input_planes));
      SecureZero(workspace_.output_planes.data(),
                 sizeof(workspace_.output_planes));
      output->samples.fill(0);
      output->ResetHeader();
      *capture_discontinuity = true;
      return Status::Ok();
    }
    const auto dsp_started = std::chrono::steady_clock::now();
    for (std::size_t sample = 0U; sample < audio::kSamplesPer48k20ms;
         ++sample) {
      const std::size_t offset =
          sample * platform::rv1106::AlsaSingleTurnIo::kChannels;
      workspace_.input_planes[0U][sample] = ApplyPolarity(
          workspace_.capture_period[offset], config_.capture_left_polarity);
      workspace_.input_planes[1U][sample] = ApplyPolarity(
          workspace_.capture_period[offset + 1U],
          config_.capture_right_polarity);
      workspace_.input_planes[2U][sample] = 0;
      workspace_.input_planes[3U][sample] = 0;
    }
    const audio::SampleTransformResult transform = workspace_.decimator.Process(
        workspace_.input_planes, &workspace_.output_planes);
    if (!transform.ok()) {
      voice_playback_reference_.fill(0);
      UpdateMaximum(ElapsedMicroseconds(dsp_started), &dsp_max_latency_us_);
      return Status::Error(StatusCode::kInternal,
                           "dual-microphone decimation failed");
    }
    std::copy(workspace_.output_planes[0U].begin(),
              workspace_.output_planes[0U].end(), voice_mic_left_.begin());
    std::copy(workspace_.output_planes[1U].begin(),
              workspace_.output_planes[1U].end(), voice_mic_right_.begin());
    platform::rv1106::RockchipVoiceDspStatus dsp_status =
        platform::rv1106::RockchipVoiceDspStatus::kOk;
    if (apply_voice_dsp) {
      dsp_status = voice_dsp_.Process(
          voice_mic_left_, voice_mic_right_, voice_playback_reference_,
          &voice_dsp_output_);
      if (dsp_status == platform::rv1106::RockchipVoiceDspStatus::kOk) {
        UpdateEchoGateForProcessedFrame(voice_dsp_output_);
        RememberEchoGateInput(voice_mic_left_, voice_mic_right_,
                              voice_playback_reference_,
                              aligned_playback_reference);
      }
    } else {
      if (echo_previous_input_valid_ || voice_frame_echo_context_ ||
          echo_tail_frames_remaining_ != 0U ||
          echo_reference_was_active_ || echo_admission_evidence_ != 0U ||
          echo_admission_hold_frames_ != 0U) {
        ResetEchoGateRuntime();
      }
    }
    voice_playback_reference_.fill(0);
    SecureZero(workspace_.capture_period.data(),
               workspace_.capture_period.size() *
                   sizeof(workspace_.capture_period[0]));
    SecureZero(workspace_.input_planes.data(), sizeof(workspace_.input_planes));
    SecureZero(workspace_.output_planes.data(),
               sizeof(workspace_.output_planes));
    UpdateMaximum(ElapsedMicroseconds(dsp_started), &dsp_max_latency_us_);
    if (dsp_status != platform::rv1106::RockchipVoiceDspStatus::kOk) {
      return Status::Error(StatusCode::kInternal,
                           "Rockchip voice DSP rejected a capture frame");
    }
    if (apply_voice_dsp && aligned_playback_reference &&
        config_.barge_in_enabled) {
      ArmBargeAfterAlignedPlaybackReference();
    }
    output->format = {kInputSampleRateHz, kFrameDurationMs, 1U,
                      audio::SampleFormat::kPcmS16Le};
    output->metadata.monotonic_timestamp_us = capture_timestamp_us;
    output->metadata.sequence = voice_capture_sequence_;
    output->metadata.stream_id = uplink_stream_id_;
    output->metadata.turn_id = turn_started_ ? turn_id_ : 0U;
    output->metadata.epoch = epoch_;
    output->metadata.discontinuity = false;
    output->samples_per_channel = audio::kSamplesPer16k20ms;
    const VoiceSamples& selected =
        apply_voice_dsp ? voice_dsp_output_ : voice_mic_left_;
    std::copy(selected.begin(), selected.end(), output->samples.begin());
    if (!output->HasValidLength() ||
        output->metadata.monotonic_timestamp_us == 0U ||
        voice_capture_sequence_ == std::numeric_limits<std::uint32_t>::max()) {
      return Status::Error(StatusCode::kResourceExhausted,
                           "voice capture metadata exhausted");
    }
    ++voice_capture_sequence_;
    return Status::Ok();
  }

  std::string NextVoiceMessageId() {
    return "voice-" + std::to_string(voice_message_sequence_++);
  }

  Status StartVoiceTurn(const std::uint64_t first_timestamp_us) {
    SensitiveString control;
    const std::string message_id = NextVoiceMessageId();
    control.value = BuildControl(
        "turn.start", message_id.c_str(), config_.device_id, session_id_,
        turn_id_, uplink_stream_id_, epoch_,
        "{\"sample_rate_hz\":16000}");
    Status status = client_.SendControl(control.value);
    if (!status.ok()) {
      return status;
    }
    turn_started_ = true;
    completed_ = false;
    voice_uplink_sequence_ = 0U;
    voice_first_timestamp_us_ = first_timestamp_us;
    return Status::Ok();
  }

  Status SendVoicePcm(const VoiceSamples& samples,
                      const bool start,
                      const bool end) {
    SensitiveUplinkPayload payload;
    for (std::size_t sample = 0U; sample < samples.size(); ++sample) {
      const auto encoded = static_cast<std::uint16_t>(samples[sample]);
      payload.bytes[sample * 2U] =
          static_cast<std::uint8_t>(encoded & 0xFFU);
      payload.bytes[sample * 2U + 1U] =
          static_cast<std::uint8_t>((encoded >> 8U) & 0xFFU);
    }
    protocol::AudioPacketHeader header{};
    header.kind = protocol::AudioPacketKind::kUplink;
    header.flags = static_cast<std::uint16_t>(
        (start ? kPcmFlagStart : 0U) | (end ? kPcmFlagEnd : 0U));
    header.sample_rate_hz = kInputSampleRateHz;
    header.payload_length_bytes = static_cast<std::uint32_t>(payload.bytes.size());
    header.sequence = voice_uplink_sequence_;
    header.monotonic_timestamp_us =
        voice_first_timestamp_us_ +
        static_cast<std::uint64_t>(voice_uplink_sequence_) * 20000U;
    header.epoch = epoch_;
    header.device_uuid = config_.device_uuid;
    header.session_id = session_id_;
    header.turn_id = turn_id_;
    header.stream_id = uplink_stream_id_;
    const auto send_started = std::chrono::steady_clock::now();
    Status status = client_.SendPcm(header, payload.bytes.data(),
                                    payload.bytes.size());
    UpdateMaximum(ElapsedMicroseconds(send_started), &send_max_latency_us_);
    if (!status.ok()) {
      return status;
    }
    if (voice_uplink_sequence_ == std::numeric_limits<std::uint32_t>::max()) {
      return Status::Error(StatusCode::kResourceExhausted,
                           "voice uplink sequence exhausted");
    }
    ++voice_uplink_sequence_;
    ++uplink_frames_;
    return Status::Ok();
  }

  Status CommitVoiceTurn() {
    SensitiveString control;
    const std::string message_id = NextVoiceMessageId();
    control.value = BuildControl(
        "turn.commit", message_id.c_str(), config_.device_id, session_id_,
        turn_id_, uplink_stream_id_, epoch_, "{}");
    return client_.SendControl(control.value);
  }

  Status MaybeSendVoiceKeepAlive() {
    const std::uint64_t now_us = MonotonicMicroseconds();
    if (now_us == 0U) {
      return Status::Error(StatusCode::kInternal,
                           "voice keepalive timestamp is invalid");
    }
    if (voice_keepalive_timestamp_us_ == 0U) {
      voice_keepalive_timestamp_us_ = now_us;
      return Status::Ok();
    }
    if (now_us - voice_keepalive_timestamp_us_ < kVoiceKeepAliveIntervalUs) {
      return Status::Ok();
    }
    Status status = client_.SendKeepAlivePong();
    if (status.ok()) {
      voice_keepalive_timestamp_us_ = now_us;
    }
    return status;
  }

  Status ResetVoiceCaptureAfterDiscontinuity() {
    workspace_.decimator.Reset();
    voice_dsp_.Close();
    if (voice_dsp_.Open() !=
        platform::rv1106::RockchipVoiceDspStatus::kOk) {
      return Status::Error(
          StatusCode::kInternal,
          "Rockchip voice DSP could not restart after capture discontinuity");
    }
    voice_vad_.Reset();
    const auto wake_reset =
        wake_engine_->Reset(audio::WakeWordResetReason::kDiscontinuity);
    if (!wake_reset.valid() || !wake_reset.reset) {
      return Status::Error(
          StatusCode::kInternal,
          "Snowboy could not reset after capture discontinuity");
    }
    voice_mic_left_.fill(0);
    voice_mic_right_.fill(0);
    voice_playback_reference_.fill(0);
    voice_dsp_output_.fill(0);
    ClearPlaybackReference();
    ResetEchoGate();
    return Status::Ok();
  }

  void AcknowledgeVoiceCaptureDiscontinuity() noexcept {
    {
      std::lock_guard<std::mutex> lock(voice_capture_mutex_);
      ClearVoiceCaptureQueueLocked();
      voice_capture_pending_recoveries_ = 0U;
      voice_capture_pending_overruns_ = 0U;
      voice_capture_pending_max_latency_us_ = 0U;
      voice_capture_discontinuity_ = false;
    }
    voice_capture_condition_.notify_all();
  }

  Status CancelVoiceTurnAfterCaptureDiscontinuity() {
    if (!turn_started_ || response_started_ || cancel_requested_) {
      return Status::Error(
          StatusCode::kFailedPrecondition,
          "capture discontinuity cannot cancel the active voice turn");
    }
    SensitiveString control;
    const std::string message_id = NextVoiceMessageId();
    control.value = BuildControl(
        "turn.cancel", message_id.c_str(), config_.device_id, session_id_,
        turn_id_, uplink_stream_id_, epoch_, "{}");
    Status status = client_.SendControl(control.value);
    if (!status.ok()) {
      return status;
    }
    cancel_requested_ = true;
    status = watchdog_.Start();
    if (!status.ok()) {
      return status;
    }
    status = ReceiveResponse();
    watchdog_.Stop();
    if (!status.ok()) {
      return status;
    }
    PrintAudioTurnMetrics("capture_discontinuity");
    status = AdvanceVoiceGeneration();
    if (status.ok()) {
      AcknowledgeVoiceCaptureDiscontinuity();
    }
    return status;
  }

  Status CaptureVoiceAndUpload(const bool require_wake,
                               const bool resume_confirmed_speech,
                               bool* const captured) {
    if (captured == nullptr) {
      return Status::Error(StatusCode::kInvalidArgument,
                           "voice turn result is null");
    }
    *captured = false;
    ResetAudioTurnMetrics();
    const bool resume_with_pre_roll =
        resume_confirmed_speech && barge_pre_roll_.size() != 0U;
    if (resume_confirmed_speech && !resume_with_pre_roll) {
      std::cout << "boompi-client: barge-in pre-roll unavailable; "
                   "falling back to live follow-up capture"
                << std::endl;
      barge_speech_active_.store(false, std::memory_order_release);
    }
    VoicePreRoll local_pre_roll;
    VoicePreRoll* pre_roll = resume_with_pre_roll
                                 ? &barge_pre_roll_
                                 : &local_pre_roll;
    const bool resumed_speech_active =
        !resume_with_pre_roll ||
        barge_speech_active_.load(std::memory_order_acquire);
    bool listening_for_speech = !require_wake;
    std::uint32_t wait_frames = 0U;
    if (!resume_with_pre_roll) {
      voice_vad_.Reset();
      if (require_wake) {
        const auto reset = wake_engine_->Reset(
            audio::WakeWordResetReason::kGenerationActivated);
        if (!reset.valid() || !reset.reset) {
          return Status::Error(StatusCode::kInternal,
                               "Snowboy reset failed");
        }
      }
      for (;;) {
        audio::Mono16kFrame frame{};
        bool capture_discontinuity = false;
        Status status = CaptureProcessedVoice(
            &frame, listening_for_speech, &capture_discontinuity);
        if (!status.ok()) {
          return status;
        }
        if (capture_discontinuity) {
          status = ResetVoiceCaptureAfterDiscontinuity();
          if (!status.ok()) {
            return status;
          }
          AcknowledgeVoiceCaptureDiscontinuity();
          pre_roll->Clear();
          listening_for_speech = !require_wake;
          wait_frames = 0U;
          continue;
        }
        status = MaybeSendVoiceKeepAlive();
        if (!status.ok()) {
          return status;
        }
        VoiceSamples samples{};
        std::copy_n(frame.samples.begin(), samples.size(), samples.begin());
        pre_roll->Push(samples);
        if (!listening_for_speech) {
          audio::Mono16kFrame wake_frame = frame;
          std::copy(voice_mic_left_.begin(), voice_mic_left_.end(),
                    wake_frame.samples.begin());
          const auto wake = wake_engine_->Process(wake_frame);
          SecureZero(wake_frame.samples.data(),
                     wake_frame.samples.size() *
                         sizeof(wake_frame.samples[0]));
          if (!wake.valid()) {
            return Status::Error(StatusCode::kInternal,
                                 "Snowboy processing failed");
          }
          if (!wake.detected()) {
            continue;
          }
          std::cout << "boompi-client: Snowboy wake detected" << std::endl;
          listening_for_speech = true;
          wait_frames = 0U;
          voice_vad_.Reset();
        }
        const auto vad = voice_vad_.Process(frame);
        if (!vad.valid) {
          return Status::Error(StatusCode::kInternal,
                               "WebRTC VAD processing failed");
        }
        UpdateEchoAdmission(vad.speech_now);
        const bool echo_rejected =
            listening_for_speech && voice_frame_echo_context_ &&
            !voice_frame_near_speech_allowed_;
        if (echo_rejected) {
          if (vad.speech_now) {
            ++echo_gate_rejected_vad_frames_;
          }
          pre_roll->Clear();
          voice_vad_.Reset();
        } else if (voice_frame_echo_context_ && vad.speech_now) {
          ++echo_gate_admitted_vad_frames_;
        }
        if (!echo_rejected && vad.speech_started) {
          std::cout << "boompi-client: VAD speech started" << std::endl;
          break;
        }
        ++wait_frames;
        const std::uint32_t timeout_frames =
            require_wake ? kFirstSpeechTimeoutFrames
                         : kFollowUpTimeoutFrames;
        if (wait_frames >= timeout_frames) {
          pre_roll->Clear();
          return Status::Ok();
        }
      }
    }

    const std::uint64_t now_us = MonotonicMicroseconds();
    const std::uint64_t pre_roll_us =
        pre_roll->size() > 0U
            ? static_cast<std::uint64_t>(pre_roll->size() - 1U) * 20000U
            : 0U;
    if (now_us == 0U || now_us < pre_roll_us) {
      return Status::Error(StatusCode::kInternal,
                           "voice pre-roll timestamp is invalid");
    }
    Status status = StartVoiceTurn(now_us - pre_roll_us);
    if (!status.ok()) {
      return status;
    }
    VoiceSamples held{};
    bool have_held = false;
    for (std::size_t index = 0U; index < pre_roll->size(); ++index) {
      if (have_held) {
        status = SendVoicePcm(held, voice_uplink_sequence_ == 0U, false);
        if (!status.ok()) {
          return status;
        }
      }
      held = pre_roll->Oldest(index);
      have_held = true;
    }
    pre_roll->Clear();
    if (!have_held) {
      return Status::Error(StatusCode::kInternal,
                           "voice utterance has no pre-roll frame");
    }

    if (resumed_speech_active) {
      std::uint32_t utterance_frames = 0U;
      for (;;) {
        audio::Mono16kFrame frame{};
        bool capture_discontinuity = false;
        status = CaptureProcessedVoice(&frame, true, &capture_discontinuity);
        if (!status.ok()) {
          return status;
        }
        if (capture_discontinuity) {
          status = ResetVoiceCaptureAfterDiscontinuity();
          if (!status.ok()) {
            return status;
          }
          return CancelVoiceTurnAfterCaptureDiscontinuity();
        }
        status = MaybeSendVoiceKeepAlive();
        if (!status.ok()) {
          return status;
        }
        status = SendVoicePcm(held, voice_uplink_sequence_ == 0U, false);
        if (!status.ok()) {
          return status;
        }
        std::copy_n(frame.samples.begin(), held.size(), held.begin());
        ++utterance_frames;
        const auto vad = voice_vad_.Process(frame);
        if (!vad.valid) {
          return Status::Error(StatusCode::kInternal,
                               "WebRTC VAD processing failed");
        }
        UpdateEchoAdmission(vad.speech_now);
        if (vad.speech_ended) {
          std::cout << "boompi-client: VAD speech ended" << std::endl;
          break;
        }
        if (utterance_frames >= kMaximumUtteranceFrames) {
          std::cout
              << "boompi-client: maximum 30 second utterance reached; committing"
              << std::endl;
          break;
        }
      }
    }
    // Complete the main-to-barge capture handoff before the synchronous END
    // frame and turn.commit writes. Otherwise the 160 ms capture bridge has
    // no consumer during those writes and reports a false discontinuity.
    if (config_.barge_in_enabled) {
      status = StartBargeInCapture();
      if (!status.ok()) {
        return status;
      }
    }
    status = SendVoicePcm(held, voice_uplink_sequence_ == 0U, true);
    if (!status.ok()) {
      return status;
    }
    status = CommitVoiceTurn();
    if (!status.ok()) {
      return status;
    }
    std::cout << "boompi-client: voice turn committed" << std::endl;
    *captured = true;
    return Status::Ok();
  }

  Status StartBargeInCapture() {
    StopBargeInCapture();
    barge_stop_.store(false, std::memory_order_release);
    barge_duck_.store(false, std::memory_order_release);
    barge_confirmed_.store(false, std::memory_order_release);
    barge_audio_armed_.store(false, std::memory_order_release);
    barge_capture_discontinuity_.store(false, std::memory_order_release);
    barge_speech_active_.store(false, std::memory_order_release);
    barge_capture_failed_.store(false, std::memory_order_release);
    barge_pre_roll_.Clear();
    voice_vad_.Reset();
    try {
      barge_thread_ = std::thread([this]() { RunBargeInCapture(); });
    } catch (const std::system_error&) {
      return Status::Error(StatusCode::kResourceExhausted,
                           "barge-in capture thread could not start");
    }
    return Status::Ok();
  }

  void StopBargeInCapture() noexcept {
    barge_stop_.store(true, std::memory_order_release);
    if (barge_thread_.joinable()) {
      barge_thread_.join();
    }
  }

  void RequestPlaybackDropAfterBargeConfirmation() noexcept {
    std::lock_guard<std::mutex> lock(playback_mutex_);
    if (!playback_response_active_) {
      return;
    }
    playback_producer_done_ = true;
    playback_drop_requested_ = true;
    ClearPlaybackQueueLocked();
    playback_condition_.notify_all();
  }

  void RunBargeInCapture() noexcept {
    std::uint32_t consecutive_speech_ms = 0U;
    std::uint32_t warmup_frames_remaining = 0U;
    bool vad_armed = false;
    bool confirmed = false;
    bool vad_in_speech = false;
    while (!barge_stop_.load(std::memory_order_acquire)) {
      audio::Mono16kFrame frame{};
      bool capture_discontinuity = false;
      const Status status =
          CaptureProcessedVoice(&frame, true, &capture_discontinuity);
      if (!status.ok()) {
        barge_capture_failed_.store(true, std::memory_order_release);
        client_.RequestStop();
        return;
      }
      if (capture_discontinuity) {
        barge_capture_discontinuity_.store(true, std::memory_order_release);
        client_.RequestReceiveInterrupt();
        return;
      }
      const bool already_confirmed =
          barge_confirmed_.load(std::memory_order_acquire);
      if (!already_confirmed &&
          !barge_audio_armed_.load(std::memory_order_acquire)) {
        const auto quiet_vad = voice_vad_.Process(frame);
        if (!quiet_vad.valid) {
          barge_capture_failed_.store(true, std::memory_order_release);
          client_.RequestStop();
          return;
        }
        ObserveEchoGateVad(quiet_vad.speech_now);
        if (vad_armed) {
          barge_duck_.store(false, std::memory_order_release);
          barge_speech_active_.store(false, std::memory_order_release);
          vad_armed = false;
          warmup_frames_remaining = 0U;
          consecutive_speech_ms = 0U;
          confirmed = false;
          vad_in_speech = false;
          barge_pre_roll_.Clear();
          voice_vad_.Reset();
        }
        continue;
      }
      if (!vad_armed) {
        vad_armed = true;
        warmup_frames_remaining = kBargeAecWarmupFrames;
        consecutive_speech_ms = 0U;
        confirmed = false;
        vad_in_speech = false;
        barge_pre_roll_.Clear();
        voice_vad_.Reset();
        continue;
      }
      if (warmup_frames_remaining != 0U) {
        --warmup_frames_remaining;
        if (warmup_frames_remaining == 0U) {
          consecutive_speech_ms = 0U;
          confirmed = false;
          vad_in_speech = false;
          barge_pre_roll_.Clear();
          voice_vad_.Reset();
        }
        continue;
      }
      VoiceSamples samples{};
      std::copy_n(frame.samples.begin(), samples.size(), samples.begin());
      barge_pre_roll_.Push(samples);
      const auto vad = voice_vad_.Process(frame);
      if (!vad.valid) {
        barge_capture_failed_.store(true, std::memory_order_release);
        client_.RequestStop();
        return;
      }
      UpdateEchoAdmission(vad.speech_now);
      if (!confirmed && voice_frame_echo_context_ &&
          !voice_frame_near_speech_allowed_) {
        if (vad.speech_now) {
          ++echo_gate_rejected_vad_frames_;
        }
        consecutive_speech_ms = 0U;
        vad_in_speech = false;
        barge_duck_.store(false, std::memory_order_release);
        barge_speech_active_.store(false, std::memory_order_release);
        barge_pre_roll_.Clear();
        voice_vad_.Reset();
        continue;
      }
      if (!confirmed && voice_frame_echo_context_ && vad.speech_now) {
        ++echo_gate_admitted_vad_frames_;
      }
      if (vad.speech_started) {
        vad_in_speech = true;
      }
      if (vad.speech_ended) {
        vad_in_speech = false;
      }
      if (!confirmed) {
        consecutive_speech_ms =
            vad.speech_now ? consecutive_speech_ms + kFrameDurationMs : 0U;
        if (consecutive_speech_ms >= 80U) {
          barge_duck_.store(true, std::memory_order_release);
        }
      }
      if (!confirmed && consecutive_speech_ms >= 160U) {
        confirmed = true;
        barge_pre_roll_.PreservePrefix();
        barge_confirmed_timestamp_us_.store(MonotonicMicroseconds(),
                                            std::memory_order_release);
        barge_confirmed_.store(true, std::memory_order_release);
        RequestPlaybackDropAfterBargeConfirmation();
        client_.RequestReceiveInterrupt();
      }
      if (confirmed) {
        barge_speech_active_.store(vad_in_speech || vad.speech_now,
                                   std::memory_order_release);
      }
    }
  }

  Status ApplyPendingBargeIn() {
    if (barge_capture_failed_.load(std::memory_order_acquire)) {
      return Status::Error(StatusCode::kInternal,
                           "barge-in capture failed");
    }
    const bool capture_discontinuity =
        barge_capture_discontinuity_.load(std::memory_order_acquire);
    const bool duck_requested =
        !capture_discontinuity &&
        barge_duck_.load(std::memory_order_acquire);
    if (!duck_requested && duck_applied_ &&
        !barge_confirmed_.load(std::memory_order_acquire)) {
      const Status duck_status = renderer_.SetDucked(false);
      if (!duck_status.ok()) {
        return duck_status;
      }
      duck_applied_ = false;
    }
    if (duck_requested && response_started_ && !duck_applied_) {
      const Status duck_status = renderer_.SetDucked(true);
      if (!duck_status.ok()) {
        return duck_status;
      }
      duck_applied_ = true;
    }
    const bool barge_confirmed =
        barge_confirmed_.load(std::memory_order_acquire);
    if (capture_discontinuity && cancel_requested_) {
      response_cancelled_for_capture_discontinuity_ = true;
      return Status::Ok();
    }
    if ((!capture_discontinuity && !barge_confirmed) || cancel_requested_) {
      return Status::Ok();
    }
    std::cout << (capture_discontinuity
                      ? "boompi-client: capture discontinuity detected"
                      : "boompi-client: barge-in confirmed")
              << std::endl;
    Status status = FinishPlaybackResponse(true);
    if (!status.ok()) {
      return status;
    }
    renderer_.Disarm();
    pending_pcm_.fill(0);
    pending_samples_ = 0U;
    SensitiveString control;
    const std::string message_id = NextVoiceMessageId();
    const bool cancel_response = response_started_;
    const std::uint32_t cancel_stream =
        cancel_response ? downlink_stream_id_ : uplink_stream_id_;
    control.value = BuildControl(
        cancel_response ? "response.cancel" : "turn.cancel",
        message_id.c_str(), config_.device_id,
        session_id_, turn_id_, cancel_stream, epoch_, "{}");
    status = client_.SendControl(control.value);
    if (status.ok()) {
      barge_cancel_sent_timestamp_us_.store(MonotonicMicroseconds(),
                                            std::memory_order_release);
      cancel_requested_ = true;
      response_cancelled_for_capture_discontinuity_ = capture_discontinuity;
      std::cout << "boompi-client: response cancel sent; type="
                << (cancel_response ? "response.cancel" : "turn.cancel")
                << std::endl;
    }
    return status;
  }

  Status AdvanceVoiceGeneration() {
    if (epoch_ == std::numeric_limits<std::uint32_t>::max() ||
        turn_id_ == std::numeric_limits<std::uint32_t>::max() ||
        uplink_stream_id_ == std::numeric_limits<std::uint32_t>::max()) {
      return Status::Error(StatusCode::kResourceExhausted,
                           "voice generation identifiers exhausted");
    }
    ++epoch_;
    ++turn_id_;
    ++uplink_stream_id_;
    downlink_stream_id_ = 0U;
    downlink_frames_ = 0U;
    playback_sequence_ = 0U;
    playback_timestamp_us_ = 0U;
    pending_pcm_.fill(0);
    pending_samples_ = 0U;
    downlink_bytes_ = 0U;
    text_delta_bytes_ = 0U;
    response_id_.clear();
    turn_started_ = false;
    response_started_ = false;
    audio_started_ = false;
    audio_ended_ = false;
    completed_ = true;
    cancel_requested_ = false;
    response_was_cancelled_ = false;
    response_failed_ = false;
    response_cancelled_for_capture_discontinuity_ = false;
    duck_applied_ = false;
    ClearPlaybackReference();
    return Status::Ok();
  }
#endif

  static network::WssClientConfig MakeWssConfig(
      const ManualConfig& config) {
    network::WssClientConfig output{};
    output.server_ip = config.server_ip;
    output.server_port = config.server_port;
    output.server_name = config.server_name;
    output.spki_sha256_base64 = config.server_spki_sha256;
    output.connect_timeout_ms = 5000U;
    output.write_timeout_ms = 10000U;
    output.receive_idle_timeout_ms = kResponseInactivityTimeoutMs;
    return output;
  }

  Status Hello() {
    SensitiveString payload;
    payload.value = "{\"device_token\":";
    AppendJsonString(config_.device_token, &payload.value);
    payload.value.push_back('}');
    SensitiveString control;
    control.value = BuildControl("hello", "manual-1", config_.device_id,
                                 0U, 0U, 0U, 0U, payload.value);
    Status status = client_.SendControl(control.value);
    if (!status.ok()) {
      return status;
    }

    status = watchdog_.Start();
    if (!status.ok()) {
      return status;
    }

    network::WssInboundMessage inbound{};
    status = client_.Receive(&inbound);
    watchdog_.Stop();
    if (!status.ok()) {
      if (watchdog_.timed_out()) {
        return Status::Error(
            StatusCode::kResourceExhausted,
            "server hello acknowledgement was inactive for 30 seconds");
      }
      return status;
    }
    if (watchdog_.timed_out()) {
      SecureClear(&inbound.control_json);
      SecureZero(inbound.pcm_payload.data(), inbound.pcm_payload.size());
      return Status::Error(
          StatusCode::kResourceExhausted,
          "server hello acknowledgement was inactive for 30 seconds");
    }
    if (inbound.type != network::WssInboundMessageType::kControl) {
      SecureZero(inbound.pcm_payload.data(), inbound.pcm_payload.size());
      return Status::Error(StatusCode::kFailedPrecondition,
                           "expected hello acknowledgement control");
    }
    protocol::ServerControlMessage decoded{};
    status = protocol::DecodeServerControl(inbound.control_json,
                                           config_.device_id, &decoded);
    SecureClear(&inbound.control_json);
    if (!status.ok()) {
      return status;
    }
    if (decoded.type == protocol::ServerControlType::kError) {
      SecureClear(&decoded.error_message);
      SecureClear(&decoded.error_code);
      return Status::Error(StatusCode::kFailedPrecondition,
                           "server rejected the device hello");
    }
    if (decoded.type != protocol::ServerControlType::kHelloAck ||
        decoded.envelope.session_id == 0U ||
        decoded.envelope.turn_id != 0U ||
        decoded.envelope.stream_id != 0U || decoded.envelope.epoch == 0U ||
        decoded.input_sample_rate_hz != kInputSampleRateHz ||
        decoded.output_sample_rate_hz != kOutputSampleRateHz ||
        decoded.input_frame_ms != kFrameDurationMs) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "hello acknowledgement is outside the exact audio contract");
    }
    session_id_ = decoded.envelope.session_id;
    epoch_ = decoded.envelope.epoch;
    return Status::Ok();
  }

  Status CaptureAndUpload() {
    SensitiveString control;
    control.value = BuildControl("turn.start", "manual-2", config_.device_id,
                                 session_id_, turn_id_, uplink_stream_id_, epoch_,
                                 "{\"sample_rate_hz\":16000}");
    Status status = client_.SendControl(control.value);
    if (!status.ok()) {
      return status;
    }
    turn_started_ = true;
    SecureClear(&control.value);

    const std::uint64_t first_timestamp_us = MonotonicMicroseconds();
    if (first_timestamp_us == 0U) {
      return Status::Error(StatusCode::kInternal,
                           "monotonic capture clock is unavailable");
    }
    constexpr std::uint64_t kFrameDurationUs =
        static_cast<std::uint64_t>(kFrameDurationMs) * 1000U;
    if (workspace_.frame_count >
        (std::numeric_limits<std::uint64_t>::max() - first_timestamp_us) /
            kFrameDurationUs) {
      return Status::Error(StatusCode::kInternal,
                           "capture timestamp range overflowed");
    }
    const auto pacing_start = std::chrono::steady_clock::now();

    for (std::size_t frame_index = 0U;
         frame_index < workspace_.frame_count; ++frame_index) {
      if (frame_index != 0U) {
        const auto send_not_before =
            pacing_start + std::chrono::milliseconds(
                               frame_index * kFrameDurationMs);
        std::this_thread::sleep_until(send_not_before);
      }
      platform::rv1106::AlsaCaptureReadResult capture_result{};
      const auto capture_started = std::chrono::steady_clock::now();
      status = io_.Capture20Ms(&workspace_.capture_period,
                               kCaptureOperationTimeoutMs, &capture_result);
      UpdateMaximum(ElapsedMicroseconds(capture_started),
                    &capture_max_latency_us_);
      AddCaptureRecoveries(capture_result.recovery_count);
      if (!status.ok()) {
        return status;
      }
      if (capture_result.discontinuity) {
        return Status::Error(
            StatusCode::kFailedPrecondition,
            "ALSA capture discontinuity cancelled the manual turn");
      }
      const auto dsp_started = std::chrono::steady_clock::now();
      for (std::size_t sample = 0U; sample < audio::kSamplesPer48k20ms;
           ++sample) {
        const std::size_t interleaved_index =
            sample * platform::rv1106::AlsaSingleTurnIo::kChannels +
            config_.capture_mic_slot;
        const std::int16_t captured =
            workspace_.capture_period[interleaved_index];
        workspace_.input_planes[0U][sample] =
            ApplyPolarity(captured, config_.capture_mic_polarity);
      }
      const audio::SampleTransformResult transform = workspace_.decimator.Process(
          workspace_.input_planes, &workspace_.output_planes);
      UpdateMaximum(ElapsedMicroseconds(dsp_started), &dsp_max_latency_us_);
      if (!transform.ok()) {
        return Status::Error(StatusCode::kInternal,
                             "48 kHz to 16 kHz capture decimation failed");
      }
      SensitiveUplinkPayload payload;
      const auto& samples = workspace_.output_planes[0U];
      for (std::size_t sample = 0U; sample < samples.size(); ++sample) {
        const auto encoded = static_cast<std::uint16_t>(samples[sample]);
        const auto widened = static_cast<std::uint32_t>(encoded);
        payload.bytes[sample * 2U] =
            static_cast<std::uint8_t>(widened & 0xFFU);
        payload.bytes[sample * 2U + 1U] =
            static_cast<std::uint8_t>((widened >> 8U) & 0xFFU);
      }

      protocol::AudioPacketHeader header{};
      header.kind = protocol::AudioPacketKind::kUplink;
      header.flags = 0U;
      if (frame_index == 0U) {
        header.flags = static_cast<std::uint16_t>(header.flags |
                                                 kPcmFlagStart);
      }
      if (frame_index + 1U == workspace_.frame_count) {
        header.flags =
            static_cast<std::uint16_t>(header.flags | kPcmFlagEnd);
      }
      header.sample_rate_hz = kInputSampleRateHz;
      header.payload_length_bytes =
          static_cast<std::uint32_t>(payload.bytes.size());
      header.sequence = static_cast<std::uint32_t>(frame_index);
      header.monotonic_timestamp_us =
          first_timestamp_us + frame_index * kFrameDurationUs;
      header.epoch = epoch_;
      header.device_uuid = config_.device_uuid;
      header.session_id = session_id_;
      header.turn_id = turn_id_;
      header.stream_id = uplink_stream_id_;
      const auto send_started = std::chrono::steady_clock::now();
      status = client_.SendPcm(header, payload.bytes.data(),
                               payload.bytes.size());
      UpdateMaximum(ElapsedMicroseconds(send_started), &send_max_latency_us_);
      SecureZero(workspace_.capture_period.data(),
                 workspace_.capture_period.size() *
                     sizeof(workspace_.capture_period[0]));
      SecureZero(workspace_.input_planes.data(),
                 sizeof(workspace_.input_planes));
      SecureZero(workspace_.output_planes.data(),
                 sizeof(workspace_.output_planes));
      if (!status.ok()) {
        return status;
      }
      ++uplink_frames_;
    }

    status = io_.FinishCapture();
    if (!status.ok()) {
      return status;
    }
    workspace_.decimator.Reset();

    control.value = BuildControl("turn.commit", "manual-3",
                                 config_.device_id, session_id_, turn_id_,
                                 uplink_stream_id_, epoch_, "{}");
    return client_.SendControl(control.value);
  }

  Status ValidateResponseEnvelope(
      const protocol::ServerControlMessage& message,
      const bool allow_new_stream) const {
    const auto& envelope = message.envelope;
    if (envelope.session_id != session_id_ || envelope.turn_id != turn_id_ ||
        envelope.epoch != epoch_ || envelope.stream_id == 0U) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "response control identity does not match the active turn");
    }
    if (!allow_new_stream && envelope.stream_id != downlink_stream_id_) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "response control stream changed during the active turn");
    }
    return Status::Ok();
  }

  Status StartResponse(const protocol::ServerControlMessage& message) {
    if (response_started_) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "response.start was received more than once");
    }
    Status status = ValidateResponseEnvelope(message, true);
    if (!status.ok()) {
      return status;
    }
    downlink_stream_id_ = message.envelope.stream_id;
    response_id_ = message.response_id;
    const audio::PlaybackGeneration generation{epoch_, turn_id_,
                                               downlink_stream_id_};
    status = renderer_.Arm(generation);
    if (!status.ok()) {
      return status;
    }
    playback_timestamp_us_ = MonotonicMicroseconds();
    if (playback_timestamp_us_ == 0U) {
      renderer_.Disarm();
      return Status::Error(StatusCode::kInternal,
                           "monotonic playback clock is unavailable");
    }
    response_started_ = true;
    return Status::Ok();
  }

  Status HandleControl(protocol::ServerControlMessage* const message,
                       bool* const response_done) {
    if (message == nullptr || response_done == nullptr) {
      return Status::Error(StatusCode::kInternal,
                           "response control destination is invalid");
    }
    *response_done = false;
    if (message->type == protocol::ServerControlType::kError) {
      if (session_id_ != 0U && turn_started_) {
        const Status identity = ValidateResponseEnvelope(
            *message, !response_started_);
        if (!identity.ok()) {
          return identity;
        }
      }
      const bool recoverable_provider_error =
          message->error_code == "provider_error";
      SecureClear(&message->error_message);
      SecureClear(&message->error_code);
      if (recoverable_provider_error) {
        response_failed_ = true;
        *response_done = true;
        return Status::Ok();
      }
      return Status::Error(StatusCode::kFailedPrecondition,
                           "server returned an error response");
    }
    if (message->type == protocol::ServerControlType::kResponseStart) {
      return StartResponse(*message);
    }
    if (message->type == protocol::ServerControlType::kResponseCancelled) {
      if (!cancel_requested_ || message->envelope.session_id != session_id_ ||
          message->envelope.turn_id != turn_id_ ||
          message->envelope.epoch != epoch_ ||
          message->envelope.stream_id == 0U) {
        SecureClear(&message->cancellation_reason);
        return Status::Error(StatusCode::kFailedPrecondition,
                             "unexpected response cancellation acknowledgement");
      }
      SecureClear(&message->cancellation_reason);
      response_was_cancelled_ = true;
      *response_done = true;
      return Status::Ok();
    }
    if (!response_started_) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "response control arrived before response.start");
    }
    Status status = ValidateResponseEnvelope(*message, false);
    if (!status.ok()) {
      return status;
    }
    if (message->response_id != response_id_) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "response identifier changed during the active turn");
    }

    switch (message->type) {
      case protocol::ServerControlType::kResponseTextDelta:
        if (message->text.size() >
            kMaximumResponseTextBytes - text_delta_bytes_) {
          SecureClear(&message->text);
          return Status::Error(StatusCode::kResourceExhausted,
                               "response text exceeded the single-turn bound");
        }
        text_delta_bytes_ += message->text.size();
        SecureClear(&message->text);
        return Status::Ok();

      case protocol::ServerControlType::kResponseAudioStart:
        if (audio_started_ || message->sample_rate_hz != kOutputSampleRateHz) {
          return Status::Error(StatusCode::kFailedPrecondition,
                               "response audio contract is invalid");
        }
        status = BeginPlaybackResponse();
        if (!status.ok()) {
          renderer_.Disarm();
          return status;
        }
        audio_started_ = true;
        return Status::Ok();

      case protocol::ServerControlType::kResponseDone:
        if (audio_started_ && !audio_ended_) {
          if (!config_.legacy_downlink_framing || pending_samples_ == 0U) {
            return Status::Error(
                StatusCode::kFailedPrecondition,
                "response.done arrived before the PCM end marker");
          }
          status = RenderPending(true);
          if (!status.ok()) {
            return status;
          }
          audio_ended_ = true;
        }
        if (audio_started_ && downlink_bytes_ == 0U) {
          return Status::Error(StatusCode::kFailedPrecondition,
                               "response audio ended without PCM");
        }
        if (!audio_started_) {
          renderer_.Disarm();
        }
        *response_done = true;
        return Status::Ok();

      case protocol::ServerControlType::kHelloAck:
      case protocol::ServerControlType::kResponseStart:
      case protocol::ServerControlType::kResponseCancelled:
      case protocol::ServerControlType::kError:
        return Status::Error(StatusCode::kFailedPrecondition,
                             "unexpected response control sequence");
    }
    return Status::Error(StatusCode::kInternal,
                         "unhandled server control type");
  }

  static std::int16_t DecodeLittleEndianSample(
      const std::uint8_t low, const std::uint8_t high) noexcept {
    const std::uint32_t combined =
        static_cast<std::uint32_t>(low) |
        (static_cast<std::uint32_t>(high) << 8U);
    const auto unsigned_sample = static_cast<std::uint16_t>(combined);
    if (unsigned_sample <=
        static_cast<std::uint16_t>(std::numeric_limits<std::int16_t>::max())) {
      return static_cast<std::int16_t>(unsigned_sample);
    }
    return static_cast<std::int16_t>(
        static_cast<std::int32_t>(unsigned_sample) - 65536);
  }

  Status RenderPending(const bool end_of_stream) {
    if (pending_samples_ == 0U ||
        pending_samples_ > audio::TtsPcmFrame24k::kFrameSamples) {
      return Status::Error(StatusCode::kInternal,
                           "pending response PCM has an invalid length");
    }
    SensitiveTtsFrame input;
    input.frame.format = {audio::TtsPcmFrame24k::kSampleRateHz,
                          audio::TtsPcmFrame24k::kFrameDurationMs,
                          audio::TtsPcmFrame24k::kChannelCount,
                          audio::TtsPcmFrame24k::kSampleFormat};
    input.frame.metadata.monotonic_timestamp_us =
        playback_timestamp_us_ +
        static_cast<std::uint64_t>(playback_sequence_) * 20000U;
    input.frame.metadata.sequence = playback_sequence_;
    input.frame.metadata.stream_id = downlink_stream_id_;
    input.frame.metadata.turn_id = turn_id_;
    input.frame.metadata.epoch = epoch_;
    input.frame.valid_source_samples =
        static_cast<std::uint16_t>(pending_samples_);
    input.frame.end_of_stream = end_of_stream;
    std::copy_n(pending_pcm_.begin(), pending_samples_,
                input.frame.samples.begin());
    std::fill(input.frame.samples.begin() + pending_samples_,
              input.frame.samples.end(), 0);

    audio::PlaybackPcmFrame48k output{};
    const audio::PlaybackRenderResult render =
        renderer_.Process(input.frame, &output);
    if (!render.produced() || render.drain_required != end_of_stream ||
        !output.HasValidLength()) {
      renderer_.Disarm();
      SecureZero(output.samples.data(),
                 output.samples.size() * sizeof(output.samples[0]));
      return Status::Error(StatusCode::kInternal,
                           "24 kHz response rendering failed");
    }
    Status status = EnqueuePlayback(&output);
    SecureZero(output.samples.data(),
               output.samples.size() * sizeof(output.samples[0]));
    if (!status.ok()) {
      renderer_.Disarm();
      return status;
    }

    if (end_of_stream) {
      output = {};
      const audio::PlaybackRenderResult drain = renderer_.Drain(&output);
      if (!drain.produced() || drain.drain_required ||
          !output.HasValidLength() || !output.end_of_stream) {
        renderer_.Disarm();
        SecureZero(output.samples.data(),
                   output.samples.size() * sizeof(output.samples[0]));
        return Status::Error(StatusCode::kInternal,
                             "24 kHz response renderer drain failed");
      }
      status = EnqueuePlayback(&output);
      SecureZero(output.samples.data(),
                 output.samples.size() * sizeof(output.samples[0]));
      if (!status.ok()) {
        return status;
      }
    }

    pending_pcm_.fill(0);
    pending_samples_ = 0U;
    if (playback_sequence_ == std::numeric_limits<std::uint32_t>::max()) {
      return Status::Error(StatusCode::kResourceExhausted,
                           "response playback sequence exhausted");
    }
    ++playback_sequence_;
    return Status::Ok();
  }

  Status HandlePcm(const network::WssInboundMessage& inbound) {
    if (!response_started_ || !audio_started_ || audio_ended_) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "downlink PCM arrived outside the active audio response");
    }
    const auto& header = inbound.pcm_header;
    protocol::AudioStreamContext context{};
    context.kind = protocol::AudioPacketKind::kDownlink;
    context.sample_rate_hz = kOutputSampleRateHz;
    context.device_uuid = config_.device_uuid;
    context.epoch = epoch_;
    context.session_id = session_id_;
    context.turn_id = turn_id_;
    context.stream_id = downlink_stream_id_;
    context.expected_sequence = downlink_frames_;
    Status status = protocol::ValidateAudioPacketContext(header, context);
    if (!status.ok()) {
      return status;
    }
    if (header.payload_length_bytes != inbound.pcm_payload.size() ||
        inbound.pcm_payload.empty() ||
        inbound.pcm_payload.size() >
            (config_.legacy_downlink_framing
                 ? static_cast<std::size_t>(
                       protocol::kMaximumAudioPayloadBytes)
                 : kMaximumDownlinkPacketBytes) ||
        inbound.pcm_payload.size() % 2U != 0U) {
      return Status::Error(StatusCode::kInvalidArgument,
                           "downlink PCM payload violates the 20 ms bound");
    }
    const bool start = (header.flags & kPcmFlagStart) != 0U;
    const bool end = (header.flags & kPcmFlagEnd) != 0U;
    if ((header.flags & kPcmFlagDiscontinuity) != 0U ||
        (!config_.legacy_downlink_framing &&
         start != (downlink_frames_ == 0U)) ||
        (config_.legacy_downlink_framing && start &&
         downlink_frames_ != 0U)) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "downlink PCM flags break stream continuity");
    }
    if (inbound.pcm_payload.size() >
        kMaximumDownlinkBytes - downlink_bytes_) {
      return Status::Error(StatusCode::kResourceExhausted,
                           "downlink PCM exceeded the 60 second bound");
    }
    if (downlink_frames_ == 0U) {
      first_downlink_pcm_us_ = MonotonicMicroseconds();
    }

    bool rendered_end = false;
    for (std::size_t offset = 0U; offset < inbound.pcm_payload.size();
         offset += 2U) {
      if (pending_samples_ >= audio::TtsPcmFrame24k::kFrameSamples) {
        return Status::Error(StatusCode::kInternal,
                             "pending response PCM overflowed");
      }
      pending_pcm_[pending_samples_] = DecodeLittleEndianSample(
          inbound.pcm_payload[offset], inbound.pcm_payload[offset + 1U]);
      ++pending_samples_;
      if (pending_samples_ == audio::TtsPcmFrame24k::kFrameSamples) {
        const bool frame_ends_stream =
            end && offset + 2U == inbound.pcm_payload.size();
        status = RenderPending(frame_ends_stream);
        if (!status.ok()) {
          return status;
        }
        rendered_end = frame_ends_stream;
      }
    }
    downlink_bytes_ += inbound.pcm_payload.size();
    ++downlink_frames_;
    if (end) {
      if (!rendered_end) {
        status = RenderPending(true);
        if (!status.ok()) {
          return status;
        }
      }
      audio_ended_ = true;
    }
    return Status::Ok();
  }

  Status ReceiveResponse() {
    for (std::size_t message_count = 0U;
         message_count < kMaximumResponseMessages; ++message_count) {
      network::WssInboundMessage inbound{};
      Status status = client_.Receive(&inbound);
      if (!status.ok()) {
#if defined(BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH)
        if (status.code() == StatusCode::kInterrupted) {
          status = ApplyPendingBargeIn();
          if (!status.ok()) {
            return status;
          }
          continue;
        }
#endif
        const Status playback_error = PlaybackError();
        if (!playback_error.ok()) {
          return playback_error;
        }
        if (watchdog_.timed_out()) {
          return Status::Error(StatusCode::kResourceExhausted,
                               "server response was inactive for 30 seconds");
        }
        return status;
      }
      watchdog_.Touch();
      if (watchdog_.timed_out()) {
        SecureClear(&inbound.control_json);
        SecureZero(inbound.pcm_payload.data(), inbound.pcm_payload.size());
        return Status::Error(StatusCode::kResourceExhausted,
                             "server response was inactive for 30 seconds");
      }

#if defined(BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH)
      status = ApplyPendingBargeIn();
      if (!status.ok()) {
        SecureClear(&inbound.control_json);
        SecureZero(inbound.pcm_payload.data(), inbound.pcm_payload.size());
        return status;
      }
#endif

      if (inbound.type == network::WssInboundMessageType::kPcm24k) {
        if (cancel_requested_) {
          SecureZero(inbound.pcm_payload.data(), inbound.pcm_payload.size());
          continue;
        }
        status = HandlePcm(inbound);
        SecureZero(inbound.pcm_payload.data(), inbound.pcm_payload.size());
        if (!status.ok()) {
          return status;
        }
        continue;
      }
      if (inbound.type != network::WssInboundMessageType::kControl) {
        return Status::Error(StatusCode::kInvalidArgument,
                             "server response message type is invalid");
      }
      protocol::ServerControlMessage decoded{};
      status = protocol::DecodeServerControl(inbound.control_json,
                                             config_.device_id, &decoded);
      SecureClear(&inbound.control_json);
      if (!status.ok()) {
        return status;
      }
      if (cancel_requested_ &&
          decoded.type != protocol::ServerControlType::kResponseCancelled &&
          decoded.type != protocol::ServerControlType::kError) {
        SecureClear(&decoded.text);
        SecureClear(&decoded.error_message);
        continue;
      }
      bool response_done = false;
      status = HandleControl(&decoded, &response_done);
      SecureClear(&decoded.text);
      SecureClear(&decoded.error_message);
      if (!status.ok()) {
        return status;
      }
      if (response_done) {
        if (audio_started_ && !response_was_cancelled_) {
          status = FinishPlaybackResponse(response_failed_);
          if (!status.ok()) {
            return status;
          }
          if (response_failed_) {
            renderer_.Disarm();
            pending_pcm_.fill(0);
            pending_samples_ = 0U;
          }
        } else if (response_failed_) {
          renderer_.Disarm();
          pending_pcm_.fill(0);
          pending_samples_ = 0U;
        }
        return Status::Ok();
      }
    }
    return Status::Error(StatusCode::kResourceExhausted,
                         "server response exceeded the message bound");
  }

  void BestEffortCancel() noexcept {
    if (!client_.connected()) {
      return;
    }
    try {
      const bool cancel_response = response_started_;
      const std::uint32_t stream_id =
          cancel_response ? downlink_stream_id_ : uplink_stream_id_;
      SensitiveString control;
      control.value = BuildControl(
          cancel_response ? "response.cancel" : "turn.cancel", "manual-4",
          config_.device_id, session_id_, turn_id_, stream_id, epoch_, "{}");
      static_cast<void>(client_.SendControl(control.value));
    } catch (...) {
      // Cleanup remains best effort; Close below is the final cancellation.
    }
  }

  const ManualConfig& config_;
  CaptureWorkspace workspace_;
  platform::rv1106::AlsaSingleTurnIo io_;
  network::WssClient client_;
  ResponseWatchdog watchdog_;
  audio::PlaybackRenderer24To48 renderer_;
  std::unique_ptr<PlaybackQueue> playback_queue_;
  std::mutex playback_mutex_;
  std::condition_variable playback_condition_;
  std::thread playback_thread_;
  Status playback_error_{};
  std::size_t playback_queue_head_{0U};
  std::size_t playback_queue_tail_{0U};
  std::size_t playback_queue_count_{0U};
  bool playback_stop_{false};
  bool playback_response_active_{false};
  bool playback_producer_done_{false};
  bool playback_drop_requested_{false};
  bool playback_started_{false};
  bool playback_response_complete_{false};
  std::array<std::int16_t, audio::TtsPcmFrame24k::kFrameSamples>
      pending_pcm_{};
  std::string response_id_;
  std::uint32_t session_id_{0U};
  std::uint32_t epoch_{0U};
  std::uint32_t turn_id_{1U};
  std::uint32_t uplink_stream_id_{1U};
  std::uint32_t downlink_stream_id_{0U};
  std::uint32_t uplink_frames_{0U};
  std::uint32_t downlink_frames_{0U};
  std::uint32_t playback_sequence_{0U};
  std::uint32_t playback_chunks_{0U};
  std::uint64_t playback_timestamp_us_{0U};
  std::uint32_t capture_xrun_count_{0U};
  std::uint64_t capture_max_latency_us_{0U};
  std::uint64_t dsp_max_latency_us_{0U};
  std::uint64_t send_max_latency_us_{0U};
  std::atomic<std::uint64_t> capture_queue_overrun_count_{0U};
  std::atomic<std::size_t> capture_queue_high_water_frames_{0U};
  std::atomic<std::uint64_t> playback_reference_overrun_count_{0U};
  std::atomic<std::uint64_t> playback_reference_underrun_count_{0U};
  std::atomic<std::size_t> playback_reference_queue_high_water_frames_{0U};
  std::size_t playback_queue_high_water_frames_{0U};
  std::uint64_t first_downlink_pcm_us_{0U};
  std::uint64_t first_playback_write_us_{0U};
  std::size_t pending_samples_{0U};
  std::size_t downlink_bytes_{0U};
  std::size_t text_delta_bytes_{0U};
  bool turn_started_{false};
  bool response_started_{false};
  bool audio_started_{false};
  bool audio_ended_{false};
  bool completed_{false};
  bool cancel_requested_{false};
  bool response_was_cancelled_{false};
  bool response_failed_{false};
  bool response_cancelled_for_capture_discontinuity_{false};
  bool duck_applied_{false};
#if defined(BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH)
  platform::rv1106::RockchipVoiceDsp voice_dsp_;
  platform::rv1106::WebRtcVadGate voice_vad_;
  std::unique_ptr<platform::rv1106::SnowboyWakeWordEngine> wake_engine_;
  std::array<VoiceCaptureSlot, kVoiceCaptureQueueCapacity>
      voice_capture_queue_{};
  std::mutex voice_capture_mutex_;
  std::condition_variable voice_capture_condition_;
  std::thread voice_capture_thread_;
  Status voice_capture_error_{};
  std::size_t voice_capture_queue_head_{0U};
  std::size_t voice_capture_queue_tail_{0U};
  std::size_t voice_capture_queue_count_{0U};
  std::uint32_t voice_capture_pending_recoveries_{0U};
  std::uint64_t voice_capture_pending_overruns_{0U};
  std::uint64_t voice_capture_pending_max_latency_us_{0U};
  bool voice_capture_stop_{true};
  bool voice_capture_discontinuity_{false};
  VoiceSamples voice_mic_left_{};
  VoiceSamples voice_mic_right_{};
  VoiceSamples voice_playback_reference_{};
  VoiceSamples voice_dsp_output_{};
  double echo_previous_microphone_mean_square_{0.0};
  double echo_previous_reference_mean_square_{0.0};
  double voice_frame_output_mean_square_{0.0};
  std::uint32_t echo_tail_frames_remaining_{0U};
  std::uint32_t echo_calibration_frames_{0U};
  std::uint32_t echo_quiet_observation_frames_{0U};
  std::uint32_t echo_admission_hold_frames_{0U};
  double echo_mic_to_reference_baseline_{0.0};
  double echo_output_to_reference_baseline_{0.0};
  double echo_reference_envelope_mean_square_{0.0};
  double echo_output_noise_floor_mean_square_{1.0};
  double echo_output_noise_floor_sum_{0.0};
  std::uint8_t echo_admission_evidence_{0U};
  bool echo_output_noise_floor_initialized_{false};
  bool echo_previous_input_valid_{false};
  bool echo_previous_reference_aligned_{false};
  bool echo_reference_was_active_{false};
  bool voice_frame_echo_context_{false};
  bool voice_frame_near_speech_allowed_{true};
  bool voice_frame_near_speech_energy_evidence_{false};
  std::uint64_t echo_gate_rejected_vad_frames_{0U};
  std::uint64_t echo_gate_admitted_vad_frames_{0U};
  VoicePreRoll barge_pre_roll_;
  std::thread barge_thread_;
  std::atomic<bool> barge_stop_{true};
  std::atomic<bool> barge_duck_{false};
  std::atomic<bool> barge_confirmed_{false};
  std::atomic<bool> barge_audio_armed_{false};
  std::atomic<bool> barge_capture_discontinuity_{false};
  std::atomic<bool> barge_speech_active_{false};
  std::atomic<bool> barge_capture_failed_{false};
  std::atomic<std::uint64_t> barge_confirmed_timestamp_us_{0U};
  std::atomic<std::uint64_t> barge_cancel_sent_timestamp_us_{0U};
  audio::FirDecimator48To16 playback_reference_decimator_;
  audio::CapturePlanes48k playback_reference_input_{};
  audio::CapturePlanes16k playback_reference_output_{};
  std::array<VoiceSamples, kPlaybackReferenceQueueCapacity>
      playback_reference_queue_{};
  std::mutex playback_reference_mutex_;
  std::size_t playback_reference_queue_head_{0U};
  std::size_t playback_reference_queue_tail_{0U};
  std::size_t playback_reference_queue_count_{0U};
  std::size_t playback_reference_samples_{0U};
  bool playback_reference_stream_active_{false};
  bool playback_reference_consumption_started_{false};
  bool playback_reference_producer_done_{false};
  std::uint32_t voice_capture_sequence_{0U};
  std::uint32_t voice_uplink_sequence_{0U};
  std::uint64_t voice_first_timestamp_us_{0U};
  std::uint64_t voice_keepalive_timestamp_us_{0U};
  std::uint64_t voice_message_sequence_{1U};
#endif
};

}  // namespace

Status RunManualSingleTurnFromEnvironment() {
  try {
    ManualConfig config;
    Status status = LoadManualConfig(&config);
    if (!status.ok()) {
      return status;
    }
    ManualSingleTurnSession session(config);
    status = session.Run();
    if (!status.ok()) {
      return status;
    }
    session.PrintSummary();
    return Status::Ok();
  } catch (const std::bad_alloc&) {
    return Status::Error(StatusCode::kResourceExhausted,
                         "manual single-turn memory allocation failed");
  } catch (const std::system_error&) {
    return Status::Error(StatusCode::kInternal,
                         "manual single-turn system operation failed");
  } catch (...) {
    return Status::Error(StatusCode::kInternal,
                         "manual single-turn failed unexpectedly");
  }
}

Status RunVoiceLoopFromEnvironment() {
  ManualConfig config;
  Status status = LoadManualConfig(&config);
  if (!status.ok()) {
    return status;
  }
  std::uint32_t reconnect_attempt = 0U;
  for (;;) {
    const auto session_started = std::chrono::steady_clock::now();
    try {
      ManualSingleTurnSession session(config);
      status = session.RunVoiceLoop();
    } catch (const std::bad_alloc&) {
      status = Status::Error(StatusCode::kResourceExhausted,
                             "voice loop memory allocation failed");
    } catch (const std::system_error&) {
      status = Status::Error(StatusCode::kInternal,
                             "voice loop system operation failed");
    } catch (...) {
      status = Status::Error(StatusCode::kInternal,
                             "voice loop failed unexpectedly");
    }
    if (status.code() == StatusCode::kInvalidArgument ||
        status.code() == StatusCode::kNotSupported) {
      return status;
    }
    if (std::chrono::steady_clock::now() - session_started >=
        std::chrono::minutes(1)) {
      reconnect_attempt = 0U;
    }
    const std::uint64_t now_us = MonotonicMicroseconds();
    const std::int32_t jitter =
        static_cast<std::int32_t>(now_us % 201U) - 100;
    const std::uint32_t delay_ms =
        network::ReconnectDelayMs(reconnect_attempt, jitter);
    if (reconnect_attempt != std::numeric_limits<std::uint32_t>::max()) {
      ++reconnect_attempt;
    }
    std::cerr << "boompi-client: voice loop session failed; retry_in_ms="
              << delay_ms << " reason=" << status.message() << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
  }
}

}  // namespace boompi::application
