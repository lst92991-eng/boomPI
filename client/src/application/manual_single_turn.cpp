#include "boompi/application/manual_single_turn.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <new>
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

namespace boompi::application {
namespace {

constexpr std::uint32_t kTurnId = 1U;
constexpr std::uint32_t kUplinkStreamId = 1U;
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

  const char* const polarity = std::getenv("BOOMPI_CAPTURE_MIC_POLARITY");
  if (polarity == nullptr || polarity[0] == '\0' ||
      (polarity[0] == '1' && polarity[1] == '\0')) {
    output->capture_mic_polarity = 1;
  } else if (polarity[0] == '-' && polarity[1] == '1' &&
             polarity[2] == '\0') {
    output->capture_mic_polarity = -1;
  } else {
    return ConfigError("BOOMPI_CAPTURE_MIC_POLARITY");
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
    watchdog_.Stop();
    if (!completed_ && turn_started_ && !watchdog_.timed_out()) {
      BestEffortCancel();
    }
    client_.Close();
    io_.Abort();
    pending_pcm_.fill(0);
    SecureClear(&response_id_);
  }

  ManualSingleTurnSession(const ManualSingleTurnSession&) = delete;
  ManualSingleTurnSession& operator=(const ManualSingleTurnSession&) = delete;

  Status Run() {
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
    status = client_.Connect();
    if (!status.ok()) {
      return status;
    }
    status = Hello();
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

  void PrintSummary() const {
    std::cout << "boompi-client: manual single turn completed; "
              << "uplink_frames=" << uplink_frames_
              << " downlink_frames=" << downlink_frames_
              << " playback_chunks=" << playback_chunks_
              << " text_delta_bytes=" << text_delta_bytes_ << '\n';
  }

 private:
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
                                 session_id_, kTurnId, kUplinkStreamId, epoch_,
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
        if (config_.capture_mic_polarity > 0) {
          workspace_.input_planes[0U][sample] = captured;
        } else if (captured == std::numeric_limits<std::int16_t>::min()) {
          workspace_.input_planes[0U][sample] =
              std::numeric_limits<std::int16_t>::max();
        } else {
          workspace_.input_planes[0U][sample] =
              static_cast<std::int16_t>(-captured);
        }
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
      header.turn_id = kTurnId;
      header.stream_id = kUplinkStreamId;
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
                                 config_.device_id, session_id_, kTurnId,
                                 kUplinkStreamId, epoch_, "{}");
    return client_.SendControl(control.value);
  }

  Status ValidateResponseEnvelope(
      const protocol::ServerControlMessage& message,
      const bool allow_new_stream) const {
    const auto& envelope = message.envelope;
    if (envelope.session_id != session_id_ || envelope.turn_id != kTurnId ||
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
    const audio::PlaybackGeneration generation{epoch_, kTurnId,
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
        audio_started_ = true;
        return Status::Ok();

      case protocol::ServerControlType::kResponseDone:
        if (audio_started_ && !audio_ended_) {
          return Status::Error(StatusCode::kFailedPrecondition,
                               "response.done arrived before the PCM end marker");
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
    input.frame.metadata.turn_id = kTurnId;
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
    Status status = io_.WriteMono48k(output.samples.data(),
                                    output.valid_samples,
                                    kPlaybackOperationTimeoutMs);
    SecureZero(output.samples.data(),
               output.samples.size() * sizeof(output.samples[0]));
    if (!status.ok()) {
      renderer_.Disarm();
      return status;
    }
    ++playback_chunks_;

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
      status = io_.WriteMono48k(output.samples.data(), output.valid_samples,
                                kPlaybackOperationTimeoutMs);
      SecureZero(output.samples.data(),
                 output.samples.size() * sizeof(output.samples[0]));
      if (!status.ok()) {
        return status;
      }
      ++playback_chunks_;
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
    context.turn_id = kTurnId;
    context.stream_id = downlink_stream_id_;
    context.expected_sequence = downlink_frames_;
    Status status = protocol::ValidateAudioPacketContext(header, context);
    if (!status.ok()) {
      return status;
    }
    if (header.payload_length_bytes != inbound.pcm_payload.size() ||
        inbound.pcm_payload.empty() ||
        inbound.pcm_payload.size() > kMaximumDownlinkPacketBytes ||
        inbound.pcm_payload.size() % 2U != 0U) {
      return Status::Error(StatusCode::kInvalidArgument,
                           "downlink PCM payload violates the 20 ms bound");
    }
    const bool start = (header.flags & kPcmFlagStart) != 0U;
    const bool end = (header.flags & kPcmFlagEnd) != 0U;
    if ((header.flags & kPcmFlagDiscontinuity) != 0U ||
        start != (downlink_frames_ == 0U)) {
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

      if (inbound.type == network::WssInboundMessageType::kPcm24k) {
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
      bool response_done = false;
      status = HandleControl(&decoded, &response_done);
      SecureClear(&decoded.text);
      SecureClear(&decoded.error_message);
      if (!status.ok()) {
        return status;
      }
      if (response_done) {
        if (audio_started_) {
          status = io_.DrainPlayback(kPlaybackDrainTimeoutMs);
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
          cancel_response ? downlink_stream_id_ : kUplinkStreamId;
      SensitiveString control;
      control.value = BuildControl(
          cancel_response ? "response.cancel" : "turn.cancel", "manual-4",
          config_.device_id, session_id_, kTurnId, stream_id, epoch_, "{}");
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
  std::array<std::int16_t, audio::TtsPcmFrame24k::kFrameSamples>
      pending_pcm_{};
  std::string response_id_;
  std::uint32_t session_id_{0U};
  std::uint32_t epoch_{0U};
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

}  // namespace boompi::application
