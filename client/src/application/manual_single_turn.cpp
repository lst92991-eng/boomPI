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
constexpr std::size_t kPlaybackQueueCapacity = 256U;
constexpr std::size_t kPlaybackPrebufferFrames = 15U;
constexpr std::uint32_t kPlaybackEnqueueTimeoutMs = 100U;
#if defined(BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH)
constexpr std::size_t kVoicePreRollFrames = 25U;
constexpr std::uint32_t kFirstSpeechTimeoutFrames = 6000U / kFrameDurationMs;
constexpr std::uint32_t kFollowUpTimeoutFrames = 3000U / kFrameDurationMs;
constexpr std::uint32_t kMaximumUtteranceFrames = 60000U / kFrameDurationMs;
constexpr std::uint64_t kVoiceKeepAliveIntervalUs = 5000000U;
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

class VoicePreRoll final {
 public:
  ~VoicePreRoll() { Clear(); }

  void Push(const VoiceSamples& samples) noexcept {
    frames_[write_index_] = samples;
    write_index_ = (write_index_ + 1U) % frames_.size();
    count_ = std::min(count_ + 1U, frames_.size());
  }

  const VoiceSamples& Oldest(const std::size_t offset) const noexcept {
    const std::size_t first =
        (write_index_ + frames_.size() - count_) % frames_.size();
    return frames_[(first + offset) % frames_.size()];
  }

  std::size_t size() const noexcept { return count_; }

  void Clear() noexcept {
    SecureZero(frames_.data(), sizeof(frames_));
    write_index_ = 0U;
    count_ = 0U;
  }

 private:
  std::array<VoiceSamples, kVoicePreRollFrames> frames_{};
  std::size_t write_index_{0U};
  std::size_t count_{0U};
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
        return status;
      }
      if (!captured) {
        require_wake = true;
        continue;
      }

      if (config_.barge_in_enabled) {
        status = StartBargeInCapture();
        if (!status.ok()) {
          return status;
        }
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
      }
      if (!status.ok()) {
        return status;
      }
      const bool interrupted = response_was_cancelled_;
      status = AdvanceVoiceGeneration();
      if (!status.ok()) {
        return status;
      }
      require_wake = false;
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

  void PrintSummary() const {
    std::cout << "boompi-client: manual single turn completed; "
              << "uplink_frames=" << uplink_frames_
              << " downlink_frames=" << downlink_frames_
              << " playback_chunks=" << playback_chunks_
              << " text_delta_bytes=" << text_delta_bytes_ << '\n';
  }

 private:
  using PlaybackQueue =
      std::array<audio::PlaybackPcmFrame48k, kPlaybackQueueCapacity>;

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
    if (!playback_response_active_ || playback_producer_done_) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "playback response is not accepting audio");
    }
    if (playback_queue_count_ == kPlaybackQueueCapacity &&
        !playback_condition_.wait_for(
            lock, std::chrono::milliseconds(kPlaybackEnqueueTimeoutMs),
            [this]() {
              return playback_stop_ || !playback_error_.ok() ||
                     !playback_response_active_ ||
                     playback_queue_count_ < kPlaybackQueueCapacity;
            })) {
      return Status::Error(StatusCode::kResourceExhausted,
                           "playback jitter queue is full");
    }
    if (!playback_error_.ok()) {
      return playback_error_;
    }
    if (playback_stop_ || !playback_response_active_ ||
        playback_producer_done_) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "playback response stopped while enqueueing");
    }
    (*playback_queue_)[playback_queue_tail_] = *frame;
    playback_queue_tail_ =
        (playback_queue_tail_ + 1U) % kPlaybackQueueCapacity;
    ++playback_queue_count_;
    playback_condition_.notify_one();
    return Status::Ok();
  }

  Status FinishPlaybackResponse(const bool drop) {
    std::unique_lock<std::mutex> lock(playback_mutex_);
    if (!playback_error_.ok()) {
      return playback_error_;
    }
    if (!playback_response_active_) {
      return drop ? Status::Ok()
                  : Status::Error(StatusCode::kFailedPrecondition,
                                  "playback response is not active");
    }
    playback_producer_done_ = true;
    playback_drop_requested_ = drop;
    if (drop) {
      ClearPlaybackQueueLocked();
    }
    playback_condition_.notify_all();
    playback_condition_.wait(lock, [this]() {
      return playback_response_complete_ || playback_stop_;
    });
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
          } else {
            drain = playback_producer_done_;
          }
        }
      }

      Status status = Status::Ok();
      if (write_frame) {
        status = io_.WriteMono48k(frame.samples.data(), frame.valid_samples,
                                  kPlaybackOperationTimeoutMs);
        if (status.ok()) {
#if defined(BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH)
          PublishPlaybackReference48k(frame.samples.data(),
                                      frame.valid_samples);
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
      } else if (drain) {
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
    vad_config.mode = 2;
    vad_config.speech_start_ms = 120U;
    vad_config.speech_end_ms = 700U;
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
    return Status::Ok();
  }

  void StorePlaybackReference(const VoiceSamples& samples) noexcept {
    playback_reference_generation_.fetch_add(1U, std::memory_order_acq_rel);
    for (std::size_t pair = 0U; pair < playback_reference_atomic_.size();
         ++pair) {
      const std::uint32_t low = static_cast<std::uint16_t>(samples[pair * 2U]);
      const std::uint32_t high =
          static_cast<std::uint16_t>(samples[pair * 2U + 1U]);
      playback_reference_atomic_[pair].store(
          low | (high << 16U), std::memory_order_relaxed);
    }
    playback_reference_generation_.fetch_add(1U, std::memory_order_release);
  }

  void LoadPlaybackReference() noexcept {
    voice_playback_reference_.fill(0);
    for (std::uint8_t attempt = 0U; attempt < 3U; ++attempt) {
      const std::uint32_t before =
          playback_reference_generation_.load(std::memory_order_acquire);
      if ((before & 1U) != 0U) {
        continue;
      }
      for (std::size_t pair = 0U; pair < playback_reference_atomic_.size();
           ++pair) {
        const std::uint32_t packed =
            playback_reference_atomic_[pair].load(std::memory_order_relaxed);
        const std::uint16_t low = static_cast<std::uint16_t>(packed);
        const std::uint16_t high = static_cast<std::uint16_t>(packed >> 16U);
        voice_playback_reference_[pair * 2U] =
            low <= 0x7FFFU
                ? static_cast<std::int16_t>(low)
                : static_cast<std::int16_t>(static_cast<std::int32_t>(low) -
                                            65536);
        voice_playback_reference_[pair * 2U + 1U] =
            high <= 0x7FFFU
                ? static_cast<std::int16_t>(high)
                : static_cast<std::int16_t>(static_cast<std::int32_t>(high) -
                                            65536);
      }
      const std::uint32_t after =
          playback_reference_generation_.load(std::memory_order_acquire);
      if (before == after && (after & 1U) == 0U) {
        if (after == playback_reference_consumed_generation_) {
          voice_playback_reference_.fill(0);
        } else {
          playback_reference_consumed_generation_ = after;
        }
        return;
      }
    }
    voice_playback_reference_.fill(0);
  }

  void PublishPlaybackReference48k(const std::int16_t* samples,
                                   const std::uint16_t sample_frames) noexcept {
    if (samples == nullptr || sample_frames == 0U) {
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
        StorePlaybackReference(reference);
      }
      playback_reference_input_ = {};
      playback_reference_output_ = {};
      playback_reference_samples_ = 0U;
    }
  }

  void ClearPlaybackReference() noexcept {
    playback_reference_decimator_.Reset();
    playback_reference_input_ = {};
    playback_reference_output_ = {};
    playback_reference_samples_ = 0U;
    VoiceSamples silence{};
    StorePlaybackReference(silence);
  }

  Status CaptureProcessedVoice(audio::Mono16kFrame* const output,
                               const bool apply_voice_dsp = true) {
    if (output == nullptr || !wake_engine_ || !voice_dsp_.is_open()) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "voice capture front end is unavailable");
    }
    Status status = io_.Capture20Ms(&workspace_.capture_period,
                                    kCaptureOperationTimeoutMs);
    if (!status.ok()) {
      return status;
    }
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
      LoadPlaybackReference();
      dsp_status = voice_dsp_.Process(
          voice_mic_left_, voice_mic_right_, voice_playback_reference_,
          &voice_dsp_output_);
    }
    SecureZero(workspace_.capture_period.data(),
               workspace_.capture_period.size() *
                   sizeof(workspace_.capture_period[0]));
    SecureZero(workspace_.input_planes.data(), sizeof(workspace_.input_planes));
    SecureZero(workspace_.output_planes.data(),
               sizeof(workspace_.output_planes));
    if (dsp_status != platform::rv1106::RockchipVoiceDspStatus::kOk) {
      return Status::Error(StatusCode::kInternal,
                           "Rockchip voice DSP rejected a capture frame");
    }
    output->format = {kInputSampleRateHz, kFrameDurationMs, 1U,
                      audio::SampleFormat::kPcmS16Le};
    output->metadata.monotonic_timestamp_us = MonotonicMicroseconds();
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
    Status status = client_.SendPcm(header, payload.bytes.data(),
                                    payload.bytes.size());
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

  Status CaptureVoiceAndUpload(const bool require_wake,
                               const bool resume_confirmed_speech,
                               bool* const captured) {
    if (captured == nullptr) {
      return Status::Error(StatusCode::kInvalidArgument,
                           "voice turn result is null");
    }
    *captured = false;
    VoicePreRoll local_pre_roll;
    VoicePreRoll* pre_roll = resume_confirmed_speech
                                 ? &barge_pre_roll_
                                 : &local_pre_roll;
    bool listening_for_speech = !require_wake;
    std::uint32_t wait_frames = 0U;
    if (!resume_confirmed_speech) {
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
        Status status =
            CaptureProcessedVoice(&frame, listening_for_speech);
        if (!status.ok()) {
          return status;
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
        if (vad.speech_started) {
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

    std::uint32_t utterance_frames = 0U;
    for (;;) {
      audio::Mono16kFrame frame{};
      status = CaptureProcessedVoice(&frame);
      if (!status.ok()) {
        return status;
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
      if (vad.speech_ended) {
        std::cout << "boompi-client: VAD speech ended" << std::endl;
        break;
      }
      if (utterance_frames >= kMaximumUtteranceFrames) {
        break;
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

  void RunBargeInCapture() noexcept {
    std::uint32_t consecutive_speech_ms = 0U;
    while (!barge_stop_.load(std::memory_order_acquire)) {
      audio::Mono16kFrame frame{};
      const Status status = CaptureProcessedVoice(&frame);
      if (!status.ok()) {
        barge_capture_failed_.store(true, std::memory_order_release);
        client_.RequestStop();
        return;
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
      consecutive_speech_ms = vad.speech_now
                                  ? consecutive_speech_ms + kFrameDurationMs
                                  : 0U;
      if (consecutive_speech_ms >= 80U) {
        barge_duck_.store(true, std::memory_order_release);
      }
      if (consecutive_speech_ms >= 160U) {
        barge_confirmed_.store(true, std::memory_order_release);
        return;
      }
    }
  }

  Status ApplyPendingBargeIn() {
    if (barge_capture_failed_.load(std::memory_order_acquire)) {
      return Status::Error(StatusCode::kInternal,
                           "barge-in capture failed");
    }
    if (barge_duck_.load(std::memory_order_acquire) && response_started_ &&
        !duck_applied_) {
      const Status duck_status = renderer_.SetDucked(true);
      if (!duck_status.ok()) {
        return duck_status;
      }
      duck_applied_ = true;
    }
    if (!barge_confirmed_.load(std::memory_order_acquire) ||
        cancel_requested_) {
      return Status::Ok();
    }
    std::cout << "boompi-client: barge-in confirmed" << std::endl;
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
      cancel_requested_ = true;
      std::cout << "boompi-client: barge-in cancel sent; type="
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
      status = io_.Capture20Ms(&workspace_.capture_period,
                               kCaptureOperationTimeoutMs);
      if (!status.ok()) {
        return status;
      }
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
      status = client_.SendPcm(header, payload.bytes.data(),
                               payload.bytes.size());
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
      SecureClear(&message->error_message);
      SecureClear(&message->error_code);
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

    for (std::size_t offset = 0U; offset < inbound.pcm_payload.size();
         offset += 2U) {
      if (pending_samples_ == audio::TtsPcmFrame24k::kFrameSamples) {
        status = RenderPending(false);
        if (!status.ok()) {
          return status;
        }
      }
      pending_pcm_[pending_samples_] = DecodeLittleEndianSample(
          inbound.pcm_payload[offset], inbound.pcm_payload[offset + 1U]);
      ++pending_samples_;
    }
    downlink_bytes_ += inbound.pcm_payload.size();
    ++downlink_frames_;
    if (end) {
      status = RenderPending(true);
      if (!status.ok()) {
        return status;
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
          status = FinishPlaybackResponse(false);
          if (!status.ok()) {
            return status;
          }
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
  bool duck_applied_{false};
#if defined(BOOMPI_HAS_VOICE_LOOP_VENDOR_PATH)
  platform::rv1106::RockchipVoiceDsp voice_dsp_;
  platform::rv1106::WebRtcVadGate voice_vad_;
  std::unique_ptr<platform::rv1106::SnowboyWakeWordEngine> wake_engine_;
  VoiceSamples voice_mic_left_{};
  VoiceSamples voice_mic_right_{};
  VoiceSamples voice_playback_reference_{};
  VoiceSamples voice_dsp_output_{};
  VoicePreRoll barge_pre_roll_;
  std::thread barge_thread_;
  std::atomic<bool> barge_stop_{true};
  std::atomic<bool> barge_duck_{false};
  std::atomic<bool> barge_confirmed_{false};
  std::atomic<bool> barge_capture_failed_{false};
  audio::FirDecimator48To16 playback_reference_decimator_;
  audio::CapturePlanes48k playback_reference_input_{};
  audio::CapturePlanes16k playback_reference_output_{};
  static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                "voice playback reference requires lock-free 32-bit atomics");
  std::array<std::atomic<std::uint32_t>,
             platform::rv1106::kRockchipVoiceFrameSamples16k / 2U>
      playback_reference_atomic_{};
  std::atomic<std::uint32_t> playback_reference_generation_{0U};
  std::uint32_t playback_reference_consumed_generation_{0U};
  std::size_t playback_reference_samples_{0U};
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
  try {
    ManualConfig config;
    Status status = LoadManualConfig(&config);
    if (!status.ok()) {
      return status;
    }
    ManualSingleTurnSession session(config);
    return session.RunVoiceLoop();
  } catch (const std::bad_alloc&) {
    return Status::Error(StatusCode::kResourceExhausted,
                         "voice loop memory allocation failed");
  } catch (const std::system_error&) {
    return Status::Error(StatusCode::kInternal,
                         "voice loop system operation failed");
  } catch (...) {
    return Status::Error(StatusCode::kInternal,
                         "voice loop failed unexpectedly");
  }
}

}  // namespace boompi::application
