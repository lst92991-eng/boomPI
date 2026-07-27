#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <type_traits>
#include <utility>

#include "boompi/platform/audio_pcm_playback.h"
#include "boompi/test/test_context.h"

#define BOOMPI_EXPECT(context, expression)                                     \
  (context).Expect((expression), #expression, __FILE__, __LINE__)

namespace {

using boompi::StatusCode;
using boompi::audio::AcceptedRenderChunk48k;
using boompi::audio::PcmPlaybackControlCode;
using boompi::audio::PcmPlaybackWriteCode;
using boompi::audio::PcmPlaybackWriteResult;
using boompi::audio::PlaybackTimingSource;
using boompi::platform::PcmPlaybackDevice;
using boompi::platform::PcmPlaybackDeviceCode;
using boompi::platform::PcmPlaybackDeviceConfig;
using boompi::platform::PcmPlaybackDeviceControlResult;
using boompi::platform::PcmPlaybackDeviceStatusResult;
using boompi::platform::PcmPlaybackDeviceWriteResult;
using boompi::platform::PcmPlaybackSink48k;
using boompi::platform::PcmPlaybackTimelineState;

static_assert(std::is_trivially_copyable<PcmPlaybackDeviceConfig>::value,
              "device config must remain value-copyable");
static_assert(
    noexcept(std::declval<PcmPlaybackSink48k &>().TryWriteInterleavedS16(
        std::declval<const std::int16_t *>(), std::uint16_t{})),
    "platform sink write must remain noexcept");
static_assert(noexcept(std::declval<PcmPlaybackSink48k &>().Drop()),
              "platform sink drop must remain noexcept");
static_assert(noexcept(std::declval<PcmPlaybackSink48k &>().Prepare()),
              "platform sink prepare must remain noexcept");

constexpr PcmPlaybackDeviceConfig
DeviceConfig(const std::uint8_t channels = 1U) noexcept {
  return {
      AcceptedRenderChunk48k::kSampleRateHz, 240U, 960U, 480U, 240U, channels};
}

constexpr PcmPlaybackDeviceStatusResult
SuccessfulStatus(const std::uint32_t queued_sample_frames = 0U,
                 const std::uint64_t timestamp_us = 1000000U,
                 const PcmPlaybackTimelineState timeline_state =
                     PcmPlaybackTimelineState::kRunning) noexcept {
  return {PcmPlaybackDeviceCode::kSucceeded, queued_sample_frames, timestamp_us,
          0, timeline_state};
}

constexpr PcmPlaybackDeviceWriteResult
SuccessfulWrite(const std::uint32_t accepted_sample_frames) noexcept {
  return {PcmPlaybackDeviceCode::kSucceeded, accepted_sample_frames, 0};
}

class FakePlaybackDevice final : public PcmPlaybackDevice {
public:
  PcmPlaybackDeviceConfig config() const noexcept override {
    ++config_call_count_;
    return config_result;
  }

  std::uint64_t MonotonicNowUs() const noexcept override {
    ++clock_call_count;
    const std::size_t index = monotonic_read_index < monotonic_results.size()
                                  ? monotonic_read_index
                                  : monotonic_results.size() - 1U;
    ++monotonic_read_index;
    return monotonic_results[index];
  }

  PcmPlaybackDeviceStatusResult QueryStatus() noexcept override {
    ++status_call_count;
    const std::size_t index = status_read_index < status_results.size()
                                  ? status_read_index
                                  : status_results.size() - 1U;
    ++status_read_index;
    return status_results[index];
  }

  PcmPlaybackDeviceWriteResult
  TryWriteInterleavedS16(const std::int16_t *const samples,
                         const std::uint32_t sample_frames) noexcept override {
    ++write_call_count;
    last_samples_address = reinterpret_cast<std::uintptr_t>(samples);
    last_requested_sample_frames = sample_frames;
    captured_sample_count = 0U;
    if (samples != nullptr &&
        sample_frames <= AcceptedRenderChunk48k::kSampleCapacity &&
        (config_result.channels == 1U || config_result.channels == 2U)) {
      const std::size_t sample_count =
          static_cast<std::size_t>(sample_frames) * config_result.channels;
      for (std::size_t index = 0U; index < sample_count; ++index) {
        captured_samples[index] = samples[index];
      }
      captured_sample_count = sample_count;
    }
    return write_result;
  }

  PcmPlaybackDeviceControlResult Drop() noexcept override {
    ++drop_call_count;
    return drop_result;
  }

  PcmPlaybackDeviceControlResult Prepare() noexcept override {
    ++prepare_call_count;
    return prepare_result;
  }

  std::uint32_t config_call_count() const noexcept {
    return config_call_count_;
  }

  void SetStatuses(const PcmPlaybackDeviceStatusResult &before_write,
                   const PcmPlaybackDeviceStatusResult &after_write) noexcept {
    status_results = {before_write, after_write};
    status_read_index = 0U;
  }

  void SetMonotonicReadings(
      const std::uint64_t write_completed_us,
      const std::uint64_t observed_after_status_us) const noexcept {
    monotonic_results = {write_completed_us, observed_after_status_us};
    monotonic_read_index = 0U;
  }

  PcmPlaybackDeviceConfig config_result{DeviceConfig()};
  std::array<PcmPlaybackDeviceStatusResult, 2U> status_results{
      SuccessfulStatus(0U, 1000000U, PcmPlaybackTimelineState::kPrepared),
      SuccessfulStatus()};
  PcmPlaybackDeviceWriteResult write_result{SuccessfulWrite(1U)};
  PcmPlaybackDeviceControlResult drop_result{PcmPlaybackDeviceCode::kSucceeded,
                                             0};
  PcmPlaybackDeviceControlResult prepare_result{
      PcmPlaybackDeviceCode::kSucceeded, 0};
  std::array<std::int16_t, AcceptedRenderChunk48k::kSampleCapacity * 2U>
      captured_samples{};
  std::uintptr_t last_samples_address{0U};
  std::uint32_t last_requested_sample_frames{0U};
  std::size_t captured_sample_count{0U};
  std::uint32_t status_call_count{0U};
  std::uint32_t write_call_count{0U};
  std::uint32_t drop_call_count{0U};
  std::uint32_t prepare_call_count{0U};
  mutable std::uint32_t clock_call_count{0U};

private:
  mutable std::uint32_t config_call_count_{0U};
  std::size_t status_read_index{0U};
  mutable std::array<std::uint64_t, 2U> monotonic_results{1000000U, 1000000U};
  mutable std::size_t monotonic_read_index{0U};
};

bool ConfigureSink(boompi::test::TestContext &context,
                   FakePlaybackDevice *const device,
                   PcmPlaybackSink48k *const sink) {
  const auto status = PcmPlaybackSink48k::Create(device, sink);
  BOOMPI_EXPECT(context, status.ok());
  BOOMPI_EXPECT(context, sink->configured());
  return status.ok();
}

void ExpectEmptyWrite(boompi::test::TestContext &context,
                      const PcmPlaybackWriteResult &result,
                      const PcmPlaybackWriteCode expected_code,
                      const std::int32_t expected_native_error) {
  BOOMPI_EXPECT(context, result.code == expected_code);
  BOOMPI_EXPECT(context, result.accepted_sample_frames == 0U);
  BOOMPI_EXPECT(context, result.write_completed_timestamp_us == 0U);
  BOOMPI_EXPECT(context, result.estimated_presentation_timestamp_us == 0U);
  BOOMPI_EXPECT(context, result.queued_sample_frames_before_write == 0U);
  BOOMPI_EXPECT(context, result.timing_source == PlaybackTimingSource::kUnset);
  BOOMPI_EXPECT(context, result.native_error == expected_native_error);
  BOOMPI_EXPECT(context, result.valid());
  BOOMPI_EXPECT(context, !result.accepted());
}

void ExpectMalformedPositive(boompi::test::TestContext &context,
                             const PcmPlaybackWriteResult &result,
                             const PcmPlaybackWriteCode expected_code,
                             const std::uint16_t expected_accepted,
                             const std::int32_t expected_native_error) {
  BOOMPI_EXPECT(context, result.code == expected_code);
  BOOMPI_EXPECT(context, result.accepted_sample_frames == expected_accepted);
  BOOMPI_EXPECT(context, result.write_completed_timestamp_us == 0U);
  BOOMPI_EXPECT(context, result.estimated_presentation_timestamp_us == 0U);
  BOOMPI_EXPECT(context, result.queued_sample_frames_before_write == 0U);
  BOOMPI_EXPECT(context, result.timing_source == PlaybackTimingSource::kUnset);
  BOOMPI_EXPECT(context, result.native_error == expected_native_error);
  BOOMPI_EXPECT(context, !result.valid());
  BOOMPI_EXPECT(context, !result.accepted());
}

void TestDeviceConfigContract(boompi::test::TestContext &context) {
  constexpr auto mono = DeviceConfig(1U);
  constexpr auto stereo = DeviceConfig(2U);
  static_assert(mono.valid(), "mono device config must be valid");
  static_assert(stereo.valid(), "stereo device config must be valid");
  BOOMPI_EXPECT(context, mono.valid());
  BOOMPI_EXPECT(context, stereo.valid());

  constexpr PcmPlaybackDeviceConfig maximum{
      AcceptedRenderChunk48k::kSampleRateHz,
      PcmPlaybackWriteResult::kMaximumQueuedSampleFrames / 2U,
      PcmPlaybackWriteResult::kMaximumQueuedSampleFrames,
      AcceptedRenderChunk48k::kSampleCapacity,
      PcmPlaybackWriteResult::kMaximumQueuedSampleFrames,
      2U};
  static_assert(maximum.valid(), "maximum bounded device config must be valid");
  BOOMPI_EXPECT(context, maximum.valid());

  auto invalid = mono;
  invalid.sample_rate_hz = 0U;
  BOOMPI_EXPECT(context, !invalid.valid());
  invalid = mono;
  invalid.channels = 0U;
  BOOMPI_EXPECT(context, !invalid.valid());
  invalid.channels = 3U;
  BOOMPI_EXPECT(context, !invalid.valid());
  invalid = mono;
  invalid.period_sample_frames = 0U;
  BOOMPI_EXPECT(context, !invalid.valid());
  invalid = mono;
  invalid.period_sample_frames = invalid.buffer_sample_frames / 2U + 1U;
  BOOMPI_EXPECT(context, !invalid.valid());
  invalid = mono;
  invalid.buffer_sample_frames =
      PcmPlaybackWriteResult::kMaximumQueuedSampleFrames + 1U;
  BOOMPI_EXPECT(context, !invalid.valid());
  invalid = mono;
  invalid.start_threshold_sample_frames = 0U;
  BOOMPI_EXPECT(context, !invalid.valid());
  invalid = maximum;
  invalid.start_threshold_sample_frames =
      AcceptedRenderChunk48k::kSampleCapacity + 1U;
  BOOMPI_EXPECT(context, !invalid.valid());
  invalid = mono;
  invalid.buffer_sample_frames = 480U;
  invalid.start_threshold_sample_frames = 481U;
  BOOMPI_EXPECT(context, !invalid.valid());
  invalid = mono;
  invalid.avail_min_sample_frames = 0U;
  BOOMPI_EXPECT(context, !invalid.valid());
  invalid.avail_min_sample_frames = invalid.buffer_sample_frames + 1U;
  BOOMPI_EXPECT(context, !invalid.valid());
}

void TestCreateValidation(boompi::test::TestContext &context) {
  FakePlaybackDevice device;
  PcmPlaybackSink48k sink;

  BOOMPI_EXPECT(context, PcmPlaybackSink48k::Create(&device, nullptr).code() ==
                             StatusCode::kInvalidArgument);
  BOOMPI_EXPECT(context, PcmPlaybackSink48k::Create(nullptr, &sink).code() ==
                             StatusCode::kInvalidArgument);
  BOOMPI_EXPECT(context, !sink.configured());

  constexpr std::array<PcmPlaybackDeviceConfig, 9U> kInvalidConfigs{{
      {0U, 240U, 960U, 480U, 240U, 1U},
      {48000U, 240U, 960U, 480U, 240U, 0U},
      {48000U, 240U, 960U, 480U, 240U, 3U},
      {48000U, 0U, 960U, 480U, 240U, 1U},
      {48000U, 481U, 960U, 480U, 240U, 1U},
      {48000U, 240U, 960U, 0U, 240U, 1U},
      {48000U, 240U, 960U, 961U, 240U, 1U},
      {48000U, 240U, 1920U, 961U, 240U, 1U},
      {48000U, 240U, 960U, 480U, 0U, 1U},
  }};
  for (const auto &invalid_config : kInvalidConfigs) {
    FakePlaybackDevice invalid_device;
    invalid_device.config_result = invalid_config;
    PcmPlaybackSink48k invalid_sink;
    const auto status =
        PcmPlaybackSink48k::Create(&invalid_device, &invalid_sink);
    BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);
    BOOMPI_EXPECT(context, !invalid_sink.configured());
    BOOMPI_EXPECT(context, invalid_device.config_call_count() == 1U);
  }

  FakePlaybackDevice wrong_rate;
  wrong_rate.config_result.sample_rate_hz = 44100U;
  BOOMPI_EXPECT(context, wrong_rate.config_result.valid());
  PcmPlaybackSink48k wrong_rate_sink;
  BOOMPI_EXPECT(
      context,
      PcmPlaybackSink48k::Create(&wrong_rate, &wrong_rate_sink).code() ==
          StatusCode::kInvalidArgument);
  BOOMPI_EXPECT(context, !wrong_rate_sink.configured());

  BOOMPI_EXPECT(context, ConfigureSink(context, &device, &sink));
  BOOMPI_EXPECT(context, device.config_call_count() == 1U);
  BOOMPI_EXPECT(context, device.status_call_count == 0U);
  BOOMPI_EXPECT(context, device.write_call_count == 0U);
  BOOMPI_EXPECT(context, PcmPlaybackSink48k::Create(nullptr, &sink).code() ==
                             StatusCode::kFailedPrecondition);
  BOOMPI_EXPECT(context, device.config_call_count() == 1U);
}

void TestUnconfiguredAndInvalidWriteArguments(
    boompi::test::TestContext &context) {
  PcmPlaybackSink48k unconfigured;
  std::int16_t sample = 7;
  ExpectEmptyWrite(context, unconfigured.TryWriteInterleavedS16(&sample, 1U),
                   PcmPlaybackWriteCode::kUnavailable, 0);
  const auto unavailable_drop = unconfigured.Drop();
  const auto unavailable_prepare = unconfigured.Prepare();
  BOOMPI_EXPECT(context,
                unavailable_drop.code == PcmPlaybackControlCode::kUnavailable);
  BOOMPI_EXPECT(context, unavailable_drop.valid());
  BOOMPI_EXPECT(context, unavailable_drop.native_error == 0);
  BOOMPI_EXPECT(context, unavailable_prepare.code ==
                             PcmPlaybackControlCode::kUnavailable);
  BOOMPI_EXPECT(context, unavailable_prepare.valid());
  BOOMPI_EXPECT(context, unavailable_prepare.native_error == 0);

  FakePlaybackDevice device;
  PcmPlaybackSink48k sink;
  if (!ConfigureSink(context, &device, &sink)) {
    return;
  }

  ExpectEmptyWrite(context, sink.TryWriteInterleavedS16(nullptr, 1U),
                   PcmPlaybackWriteCode::kFatal, 0);
  ExpectEmptyWrite(context, sink.TryWriteInterleavedS16(&sample, 0U),
                   PcmPlaybackWriteCode::kFatal, 0);
  std::array<std::int16_t, AcceptedRenderChunk48k::kSampleCapacity + 1U>
      oversized{};
  ExpectEmptyWrite(
      context,
      sink.TryWriteInterleavedS16(oversized.data(),
                                  static_cast<std::uint16_t>(oversized.size())),
      PcmPlaybackWriteCode::kFatal, 0);
  BOOMPI_EXPECT(context, device.status_call_count == 0U);
  BOOMPI_EXPECT(context, device.write_call_count == 0U);
  BOOMPI_EXPECT(context, device.clock_call_count == 0U);
}

void TestMonoFullAndPartialWrites(boompi::test::TestContext &context) {
  FakePlaybackDevice device;
  device.config_result = DeviceConfig(1U);
  PcmPlaybackSink48k sink;
  if (!ConfigureSink(context, &device, &sink)) {
    return;
  }

  std::array<std::int16_t, 6U> samples{{-300, -20, 0, 40, 900, 1200}};
  device.SetStatuses(
      SuccessfulStatus(480U, 1000000U, PcmPlaybackTimelineState::kPrepared),
      SuccessfulStatus(486U, 1005050U, PcmPlaybackTimelineState::kRunning));
  device.SetMonotonicReadings(1005000U, 1005100U);
  device.write_result = SuccessfulWrite(6U);
  auto result = sink.TryWriteInterleavedS16(samples.data(), 6U);
  BOOMPI_EXPECT(context, result.valid());
  BOOMPI_EXPECT(context, result.accepted());
  BOOMPI_EXPECT(context, result.code == PcmPlaybackWriteCode::kAccepted);
  BOOMPI_EXPECT(context, result.accepted_sample_frames == 6U);
  BOOMPI_EXPECT(context, result.write_completed_timestamp_us == 1005000U);
  BOOMPI_EXPECT(context,
                result.estimated_presentation_timestamp_us == 1015050U);
  BOOMPI_EXPECT(context, result.queued_sample_frames_before_write == 480U);
  BOOMPI_EXPECT(context,
                result.timing_source == PlaybackTimingSource::kAlsaStatus);
  BOOMPI_EXPECT(context, result.native_error == 0);
  BOOMPI_EXPECT(context, device.last_samples_address ==
                             reinterpret_cast<std::uintptr_t>(samples.data()));
  BOOMPI_EXPECT(context, device.last_requested_sample_frames == 6U);
  BOOMPI_EXPECT(context, device.captured_sample_count == samples.size());
  for (std::size_t index = 0U; index < samples.size(); ++index) {
    BOOMPI_EXPECT(context, device.captured_samples[index] == samples[index]);
  }

  device.SetStatuses(
      SuccessfulStatus(0U, 2000000U, PcmPlaybackTimelineState::kRunning),
      SuccessfulStatus(3U, 2001050U, PcmPlaybackTimelineState::kRunning));
  device.SetMonotonicReadings(2001000U, 2001100U);
  device.write_result = SuccessfulWrite(3U);
  result = sink.TryWriteInterleavedS16(samples.data(), 6U);
  BOOMPI_EXPECT(context, result.valid());
  BOOMPI_EXPECT(context, result.accepted());
  BOOMPI_EXPECT(context, result.accepted_sample_frames == 3U);
  BOOMPI_EXPECT(context, result.write_completed_timestamp_us == 2001000U);
  BOOMPI_EXPECT(context,
                result.estimated_presentation_timestamp_us == 2001050U);
  BOOMPI_EXPECT(context, result.queued_sample_frames_before_write == 0U);
  BOOMPI_EXPECT(context, device.last_requested_sample_frames == 6U);
  BOOMPI_EXPECT(context, device.status_call_count == 4U);
  BOOMPI_EXPECT(context, device.write_call_count == 2U);
  BOOMPI_EXPECT(context, device.clock_call_count == 4U);
}

void TestStereoDuplication(boompi::test::TestContext &context) {
  FakePlaybackDevice device;
  device.config_result = DeviceConfig(2U);
  device.SetStatuses(
      SuccessfulStatus(0U, 3000000U, PcmPlaybackTimelineState::kPrepared),
      SuccessfulStatus(4U, 3000150U, PcmPlaybackTimelineState::kRunning));
  device.SetMonotonicReadings(3000100U, 3000200U);
  device.write_result = SuccessfulWrite(4U);
  PcmPlaybackSink48k sink;
  if (!ConfigureSink(context, &device, &sink)) {
    return;
  }

  const std::array<std::int16_t, 4U> mono{{-32768, -1, 1, 32767}};
  const auto result = sink.TryWriteInterleavedS16(mono.data(), 4U);
  BOOMPI_EXPECT(context, result.valid());
  BOOMPI_EXPECT(context, result.accepted());
  BOOMPI_EXPECT(context, device.last_requested_sample_frames == 4U);
  BOOMPI_EXPECT(context, device.captured_sample_count == 8U);
  BOOMPI_EXPECT(context, device.last_samples_address !=
                             reinterpret_cast<std::uintptr_t>(mono.data()));
  for (std::size_t frame = 0U; frame < mono.size(); ++frame) {
    BOOMPI_EXPECT(context, device.captured_samples[frame * 2U] == mono[frame]);
    BOOMPI_EXPECT(context,
                  device.captured_samples[frame * 2U + 1U] == mono[frame]);
  }

  std::array<std::int16_t, AcceptedRenderChunk48k::kSampleCapacity>
      full_capacity_mono{};
  for (std::size_t frame = 0U; frame < full_capacity_mono.size(); ++frame) {
    full_capacity_mono[frame] =
        static_cast<std::int16_t>(static_cast<std::int32_t>(frame) - 480);
  }
  full_capacity_mono.back() = 32767;
  device.SetStatuses(
      SuccessfulStatus(0U, 3100000U, PcmPlaybackTimelineState::kPrepared),
      SuccessfulStatus(AcceptedRenderChunk48k::kSampleCapacity, 3100150U,
                       PcmPlaybackTimelineState::kRunning));
  device.SetMonotonicReadings(3100100U, 3100200U);
  device.write_result =
      SuccessfulWrite(AcceptedRenderChunk48k::kSampleCapacity);
  const auto full_capacity_result = sink.TryWriteInterleavedS16(
      full_capacity_mono.data(),
      static_cast<std::uint16_t>(full_capacity_mono.size()));
  BOOMPI_EXPECT(context, full_capacity_result.valid());
  BOOMPI_EXPECT(context, full_capacity_result.accepted());
  BOOMPI_EXPECT(context, full_capacity_result.accepted_sample_frames ==
                             AcceptedRenderChunk48k::kSampleCapacity);
  BOOMPI_EXPECT(context, device.last_requested_sample_frames ==
                             AcceptedRenderChunk48k::kSampleCapacity);
  BOOMPI_EXPECT(context, device.captured_sample_count ==
                             AcceptedRenderChunk48k::kSampleCapacity * 2U);
  BOOMPI_EXPECT(
      context, device.last_samples_address !=
                   reinterpret_cast<std::uintptr_t>(full_capacity_mono.data()));
  for (std::size_t frame = 0U; frame < full_capacity_mono.size(); ++frame) {
    BOOMPI_EXPECT(context, device.captured_samples[frame * 2U] ==
                               full_capacity_mono[frame]);
    BOOMPI_EXPECT(context, device.captured_samples[frame * 2U + 1U] ==
                               full_capacity_mono[frame]);
  }
  const std::size_t final_scratch_index =
      AcceptedRenderChunk48k::kSampleCapacity * 2U - 1U;
  BOOMPI_EXPECT(context, device.captured_samples[final_scratch_index] ==
                             full_capacity_mono.back());
  BOOMPI_EXPECT(context, device.status_call_count == 4U);
  BOOMPI_EXPECT(context, device.write_call_count == 2U);
  BOOMPI_EXPECT(context, device.clock_call_count == 4U);
}

struct WriteCodeCase final {
  PcmPlaybackDeviceCode device_code;
  PcmPlaybackWriteCode sink_code;
};

constexpr std::array<WriteCodeCase, 9U> kNonSuccessfulWriteCases{{
    {PcmPlaybackDeviceCode::kUnset, PcmPlaybackWriteCode::kFatal},
    {PcmPlaybackDeviceCode::kWouldBlock, PcmPlaybackWriteCode::kWouldBlock},
    {PcmPlaybackDeviceCode::kInterrupted, PcmPlaybackWriteCode::kInterrupted},
    {PcmPlaybackDeviceCode::kXrun, PcmPlaybackWriteCode::kXrun},
    {PcmPlaybackDeviceCode::kSuspended, PcmPlaybackWriteCode::kSuspended},
    {PcmPlaybackDeviceCode::kDeviceLost, PcmPlaybackWriteCode::kDeviceLost},
    {PcmPlaybackDeviceCode::kZeroProgress, PcmPlaybackWriteCode::kZeroProgress},
    {PcmPlaybackDeviceCode::kFatal, PcmPlaybackWriteCode::kFatal},
    {PcmPlaybackDeviceCode::kUnavailable, PcmPlaybackWriteCode::kUnavailable},
}};

void TestStatusFailuresCauseZeroWrites(boompi::test::TestContext &context) {
  FakePlaybackDevice device;
  PcmPlaybackSink48k sink;
  if (!ConfigureSink(context, &device, &sink)) {
    return;
  }
  const std::array<std::int16_t, 2U> samples{{10, 20}};

  for (std::size_t index = 0U; index < kNonSuccessfulWriteCases.size();
       ++index) {
    const auto &test_case = kNonSuccessfulWriteCases[index];
    const auto native_error = -static_cast<std::int32_t>(100U + index);
    device.SetStatuses({test_case.device_code, 123U, 4000000U, native_error,
                        PcmPlaybackTimelineState::kPrepared},
                       SuccessfulStatus());
    const auto writes_before = device.write_call_count;
    const auto clock_calls_before = device.clock_call_count;
    const auto result = sink.TryWriteInterleavedS16(samples.data(), 2U);
    ExpectEmptyWrite(context, result, test_case.sink_code, native_error);
    BOOMPI_EXPECT(context, device.write_call_count == writes_before);
    BOOMPI_EXPECT(context, device.clock_call_count == clock_calls_before);
  }

  device.SetStatuses({PcmPlaybackDeviceCode::kSucceeded, 0U, 4000000U, -5,
                      PcmPlaybackTimelineState::kPrepared},
                     SuccessfulStatus());
  auto writes_before = device.write_call_count;
  auto result = sink.TryWriteInterleavedS16(samples.data(), 2U);
  ExpectEmptyWrite(context, result, PcmPlaybackWriteCode::kFatal, -5);
  BOOMPI_EXPECT(context, device.write_call_count == writes_before);

  device.SetStatuses(
      SuccessfulStatus(0U, 0U, PcmPlaybackTimelineState::kPrepared),
      SuccessfulStatus());
  writes_before = device.write_call_count;
  result = sink.TryWriteInterleavedS16(samples.data(), 2U);
  ExpectEmptyWrite(context, result, PcmPlaybackWriteCode::kFatal, 0);
  BOOMPI_EXPECT(context, device.write_call_count == writes_before);

  device.SetStatuses(
      SuccessfulStatus(PcmPlaybackWriteResult::kMaximumQueuedSampleFrames + 1U,
                       4000000U, PcmPlaybackTimelineState::kPrepared),
      SuccessfulStatus());
  writes_before = device.write_call_count;
  result = sink.TryWriteInterleavedS16(samples.data(), 2U);
  ExpectEmptyWrite(context, result, PcmPlaybackWriteCode::kFatal, 0);
  BOOMPI_EXPECT(context, device.write_call_count == writes_before);

  constexpr std::array<PcmPlaybackTimelineState, 2U> kInvalidTimelineStates{
      PcmPlaybackTimelineState::kUnset,
      static_cast<PcmPlaybackTimelineState>(0xFFU)};
  for (const auto timeline_state : kInvalidTimelineStates) {
    device.SetStatuses(SuccessfulStatus(0U, 4000000U, timeline_state),
                       SuccessfulStatus());
    writes_before = device.write_call_count;
    result = sink.TryWriteInterleavedS16(samples.data(), 2U);
    ExpectEmptyWrite(context, result, PcmPlaybackWriteCode::kFatal, 0);
    BOOMPI_EXPECT(context, device.write_call_count == writes_before);
  }

  device.SetStatuses({static_cast<PcmPlaybackDeviceCode>(0xFFU), 0U, 0U, -999,
                      PcmPlaybackTimelineState::kUnset},
                     SuccessfulStatus());
  writes_before = device.write_call_count;
  result = sink.TryWriteInterleavedS16(samples.data(), 2U);
  ExpectEmptyWrite(context, result, PcmPlaybackWriteCode::kFatal, -999);
  BOOMPI_EXPECT(context, device.write_call_count == writes_before);
  BOOMPI_EXPECT(context, device.clock_call_count == 0U);
}

void TestZeroProgressAndAllWriteCodes(boompi::test::TestContext &context) {
  FakePlaybackDevice device;
  PcmPlaybackSink48k sink;
  if (!ConfigureSink(context, &device, &sink)) {
    return;
  }
  const std::array<std::int16_t, 2U> samples{{30, 40}};
  const auto before_write =
      SuccessfulStatus(0U, 5000000U, PcmPlaybackTimelineState::kPrepared);
  const auto after_write =
      SuccessfulStatus(0U, 5000000U, PcmPlaybackTimelineState::kRunning);

  device.SetStatuses(before_write, after_write);
  device.write_result = {PcmPlaybackDeviceCode::kSucceeded, 0U, -70};
  auto result = sink.TryWriteInterleavedS16(samples.data(), 2U);
  ExpectEmptyWrite(context, result, PcmPlaybackWriteCode::kZeroProgress, -70);

  for (std::size_t index = 0U; index < kNonSuccessfulWriteCases.size();
       ++index) {
    const auto &test_case = kNonSuccessfulWriteCases[index];
    const auto native_error = -static_cast<std::int32_t>(200U + index);
    device.SetStatuses(before_write, after_write);
    device.write_result = {test_case.device_code, 0U, native_error};
    result = sink.TryWriteInterleavedS16(samples.data(), 2U);
    ExpectEmptyWrite(context, result, test_case.sink_code, native_error);
  }

  device.SetStatuses(before_write, after_write);
  device.write_result = {static_cast<PcmPlaybackDeviceCode>(0xFFU), 0U, -777};
  result = sink.TryWriteInterleavedS16(samples.data(), 2U);
  ExpectEmptyWrite(context, result, PcmPlaybackWriteCode::kFatal, -777);
  BOOMPI_EXPECT(context, device.status_call_count ==
                             kNonSuccessfulWriteCases.size() + 2U);
  BOOMPI_EXPECT(context, device.write_call_count ==
                             kNonSuccessfulWriteCases.size() + 2U);
  BOOMPI_EXPECT(context, device.clock_call_count == 0U);
}

void TestOverReportAndMalformedPositiveWrites(
    boompi::test::TestContext &context) {
  FakePlaybackDevice device;
  PcmPlaybackSink48k sink;
  if (!ConfigureSink(context, &device, &sink)) {
    return;
  }
  const std::array<std::int16_t, 4U> samples{{1, 2, 3, 4}};
  const auto before_write =
      SuccessfulStatus(0U, 6000000U, PcmPlaybackTimelineState::kPrepared);
  const auto after_write =
      SuccessfulStatus(0U, 6000000U, PcmPlaybackTimelineState::kRunning);

  device.SetStatuses(before_write, after_write);
  device.write_result = SuccessfulWrite(5U);
  auto result = sink.TryWriteInterleavedS16(samples.data(), 4U);
  ExpectMalformedPositive(context, result, PcmPlaybackWriteCode::kFatal, 5U, 0);

  device.SetStatuses(before_write, after_write);
  device.write_result =
      SuccessfulWrite(std::numeric_limits<std::uint32_t>::max());
  result = sink.TryWriteInterleavedS16(samples.data(), 4U);
  ExpectMalformedPositive(context, result, PcmPlaybackWriteCode::kFatal,
                          std::numeric_limits<std::uint16_t>::max(), 0);

  device.SetStatuses(before_write, after_write);
  device.write_result = {PcmPlaybackDeviceCode::kSucceeded, 2U, -5};
  result = sink.TryWriteInterleavedS16(samples.data(), 4U);
  ExpectMalformedPositive(context, result, PcmPlaybackWriteCode::kFatal, 2U,
                          -5);

  for (std::size_t index = 0U; index < kNonSuccessfulWriteCases.size();
       ++index) {
    const auto &test_case = kNonSuccessfulWriteCases[index];
    const auto native_error = -static_cast<std::int32_t>(300U + index);
    device.SetStatuses(before_write, after_write);
    device.write_result = {test_case.device_code, 2U, native_error};
    result = sink.TryWriteInterleavedS16(samples.data(), 4U);
    ExpectMalformedPositive(context, result, test_case.sink_code, 2U,
                            native_error);
  }

  device.SetStatuses(before_write, after_write);
  device.write_result = {static_cast<PcmPlaybackDeviceCode>(0xFFU), 2U, -888};
  result = sink.TryWriteInterleavedS16(samples.data(), 4U);
  ExpectMalformedPositive(context, result, PcmPlaybackWriteCode::kFatal, 2U,
                          -888);
  BOOMPI_EXPECT(context, device.clock_call_count == 0U);
}

void TestPostWriteStatusFailures(boompi::test::TestContext &context) {
  FakePlaybackDevice device;
  PcmPlaybackSink48k sink;
  if (!ConfigureSink(context, &device, &sink)) {
    return;
  }
  const std::array<std::int16_t, 2U> samples{{50, 60}};
  const auto before_write =
      SuccessfulStatus(12U, 7000000U, PcmPlaybackTimelineState::kPrepared);
  device.write_result = SuccessfulWrite(2U);

  for (std::size_t index = 0U; index < kNonSuccessfulWriteCases.size();
       ++index) {
    const auto &test_case = kNonSuccessfulWriteCases[index];
    const auto native_error = -static_cast<std::int32_t>(500U + index);
    device.SetStatuses(before_write,
                       {test_case.device_code, 14U, 7000150U, native_error,
                        PcmPlaybackTimelineState::kRunning});
    device.SetMonotonicReadings(7000100U, 7000200U);
    const auto statuses_before = device.status_call_count;
    const auto writes_before = device.write_call_count;
    const auto clocks_before = device.clock_call_count;
    const auto result = sink.TryWriteInterleavedS16(samples.data(), 2U);
    ExpectMalformedPositive(context, result, test_case.sink_code, 2U,
                            native_error);
    BOOMPI_EXPECT(context, device.status_call_count == statuses_before + 2U);
    BOOMPI_EXPECT(context, device.write_call_count == writes_before + 1U);
    BOOMPI_EXPECT(context, device.clock_call_count == clocks_before + 1U);
  }

  device.SetStatuses(before_write,
                     {static_cast<PcmPlaybackDeviceCode>(0xFFU), 14U, 7000150U,
                      -599, PcmPlaybackTimelineState::kRunning});
  device.SetMonotonicReadings(7000100U, 7000200U);
  auto statuses_before = device.status_call_count;
  auto clocks_before = device.clock_call_count;
  auto result = sink.TryWriteInterleavedS16(samples.data(), 2U);
  ExpectMalformedPositive(context, result, PcmPlaybackWriteCode::kFatal, 2U,
                          -599);
  BOOMPI_EXPECT(context, device.status_call_count == statuses_before + 2U);
  BOOMPI_EXPECT(context, device.clock_call_count == clocks_before + 1U);

  device.SetStatuses(before_write,
                     {PcmPlaybackDeviceCode::kSucceeded, 14U, 7000150U, -600,
                      PcmPlaybackTimelineState::kRunning});
  device.SetMonotonicReadings(7000100U, 7000200U);
  statuses_before = device.status_call_count;
  clocks_before = device.clock_call_count;
  result = sink.TryWriteInterleavedS16(samples.data(), 2U);
  ExpectMalformedPositive(context, result, PcmPlaybackWriteCode::kFatal, 2U,
                          -600);
  BOOMPI_EXPECT(context, device.status_call_count == statuses_before + 2U);
  BOOMPI_EXPECT(context, device.clock_call_count == clocks_before + 2U);

  device.SetStatuses(
      before_write,
      SuccessfulStatus(14U, 7000150U, PcmPlaybackTimelineState::kPrepared));
  device.SetMonotonicReadings(7000100U, 7000200U);
  statuses_before = device.status_call_count;
  clocks_before = device.clock_call_count;
  result = sink.TryWriteInterleavedS16(samples.data(), 2U);
  ExpectMalformedPositive(context, result, PcmPlaybackWriteCode::kFatal, 2U, 0);
  BOOMPI_EXPECT(context, device.status_call_count == statuses_before + 2U);
  BOOMPI_EXPECT(context, device.clock_call_count == clocks_before + 2U);

  constexpr std::array<PcmPlaybackDeviceStatusResult, 4U>
      kMalformedSuccessfulPostStatuses{{
          SuccessfulStatus(14U, 0U, PcmPlaybackTimelineState::kRunning),
          SuccessfulStatus(PcmPlaybackWriteResult::kMaximumQueuedSampleFrames +
                               1U,
                           7000150U, PcmPlaybackTimelineState::kRunning),
          SuccessfulStatus(14U, 7000150U, PcmPlaybackTimelineState::kUnset),
          SuccessfulStatus(14U, 7000150U,
                           static_cast<PcmPlaybackTimelineState>(0xFFU)),
      }};
  for (const auto &malformed_post_status : kMalformedSuccessfulPostStatuses) {
    device.SetStatuses(before_write, malformed_post_status);
    device.SetMonotonicReadings(7000100U, 7000200U);
    statuses_before = device.status_call_count;
    const auto writes_before = device.write_call_count;
    clocks_before = device.clock_call_count;
    result = sink.TryWriteInterleavedS16(samples.data(), 2U);
    ExpectMalformedPositive(context, result, PcmPlaybackWriteCode::kFatal, 2U,
                            0);
    BOOMPI_EXPECT(context, device.status_call_count == statuses_before + 2U);
    BOOMPI_EXPECT(context, device.write_call_count == writes_before + 1U);
    BOOMPI_EXPECT(context, device.clock_call_count == clocks_before + 2U);
  }
}

void TestClockTimestampAndOverflowPolicy(boompi::test::TestContext &context) {
  FakePlaybackDevice device;
  PcmPlaybackSink48k sink;
  if (!ConfigureSink(context, &device, &sink)) {
    return;
  }
  const std::array<std::int16_t, 4U> samples{{50, 60, 70, 80}};
  device.write_result = SuccessfulWrite(2U);

  device.SetStatuses(
      SuccessfulStatus(9U, 7000000U, PcmPlaybackTimelineState::kPrepared),
      SuccessfulStatus(2U, 7000100U, PcmPlaybackTimelineState::kRunning));
  device.SetMonotonicReadings(0U, 7000200U);
  auto statuses_before = device.status_call_count;
  auto clocks_before = device.clock_call_count;
  auto result = sink.TryWriteInterleavedS16(samples.data(), 4U);
  ExpectMalformedPositive(context, result, PcmPlaybackWriteCode::kFatal, 2U, 0);
  BOOMPI_EXPECT(context, device.status_call_count == statuses_before + 1U);
  BOOMPI_EXPECT(context, device.clock_call_count == clocks_before + 1U);

  device.SetStatuses(
      SuccessfulStatus(9U, 7000000U, PcmPlaybackTimelineState::kPrepared),
      SuccessfulStatus(2U, 7000100U, PcmPlaybackTimelineState::kRunning));
  device.SetMonotonicReadings(7000100U, 7000200U);
  statuses_before = device.status_call_count;
  clocks_before = device.clock_call_count;
  result = sink.TryWriteInterleavedS16(samples.data(), 4U);
  BOOMPI_EXPECT(context, result.valid());
  BOOMPI_EXPECT(context, result.accepted());
  BOOMPI_EXPECT(context, result.write_completed_timestamp_us == 7000100U);
  BOOMPI_EXPECT(context,
                result.estimated_presentation_timestamp_us == 7000100U);
  BOOMPI_EXPECT(context, result.queued_sample_frames_before_write == 9U);
  BOOMPI_EXPECT(context, device.status_call_count == statuses_before + 2U);
  BOOMPI_EXPECT(context, device.clock_call_count == clocks_before + 2U);

  device.SetStatuses(
      SuccessfulStatus(777U, 8000000U, PcmPlaybackTimelineState::kRunning),
      SuccessfulStatus(50U, 8000550U, PcmPlaybackTimelineState::kRunning));
  device.SetMonotonicReadings(8000500U, 8000600U);
  result = sink.TryWriteInterleavedS16(samples.data(), 4U);
  BOOMPI_EXPECT(context, result.valid());
  BOOMPI_EXPECT(context, result.accepted_sample_frames == 2U);
  BOOMPI_EXPECT(context, result.queued_sample_frames_before_write == 777U);
  BOOMPI_EXPECT(context,
                result.estimated_presentation_timestamp_us == 8001550U);

  device.SetStatuses(
      SuccessfulStatus(0U, 9000000U, PcmPlaybackTimelineState::kPrepared),
      SuccessfulStatus(3U, 9000010U, PcmPlaybackTimelineState::kRunning));
  device.SetMonotonicReadings(9000001U, 9000020U);
  result = sink.TryWriteInterleavedS16(samples.data(), 4U);
  BOOMPI_EXPECT(context, result.valid());
  BOOMPI_EXPECT(context,
                result.estimated_presentation_timestamp_us == 9000030U);

  device.SetStatuses(
      SuccessfulStatus(0U, 9100000U, PcmPlaybackTimelineState::kPrepared),
      SuccessfulStatus(1U, 9100020U, PcmPlaybackTimelineState::kRunning));
  device.SetMonotonicReadings(9100001U, 9100020U);
  result = sink.TryWriteInterleavedS16(samples.data(), 4U);
  BOOMPI_EXPECT(context, result.valid());
  BOOMPI_EXPECT(context,
                result.estimated_presentation_timestamp_us == 9100020U);

  device.SetStatuses(
      SuccessfulStatus(0U, 9200000U, PcmPlaybackTimelineState::kPrepared),
      SuccessfulStatus(50U, 9202050U, PcmPlaybackTimelineState::kRunning));
  device.SetMonotonicReadings(9202000U, 9202100U);
  result = sink.TryWriteInterleavedS16(samples.data(), 4U);
  BOOMPI_EXPECT(context, result.valid());
  BOOMPI_EXPECT(context,
                result.estimated_presentation_timestamp_us == 9203050U);

  device.SetStatuses(
      SuccessfulStatus(0U, std::numeric_limits<std::uint64_t>::max() - 2000U,
                       PcmPlaybackTimelineState::kPrepared),
      SuccessfulStatus(50U, std::numeric_limits<std::uint64_t>::max() - 999U,
                       PcmPlaybackTimelineState::kRunning));
  device.SetMonotonicReadings(std::numeric_limits<std::uint64_t>::max() - 1000U,
                              std::numeric_limits<std::uint64_t>::max());
  result = sink.TryWriteInterleavedS16(samples.data(), 4U);
  ExpectMalformedPositive(context, result, PcmPlaybackWriteCode::kFatal, 2U, 0);

  device.SetStatuses(
      SuccessfulStatus(PcmPlaybackWriteResult::kMaximumQueuedSampleFrames,
                       10000000U, PcmPlaybackTimelineState::kPrepared),
      SuccessfulStatus(PcmPlaybackWriteResult::kMaximumQueuedSampleFrames,
                       11500000U, PcmPlaybackTimelineState::kRunning));
  device.SetMonotonicReadings(11500000U, 11600000U);
  result = sink.TryWriteInterleavedS16(samples.data(), 4U);
  BOOMPI_EXPECT(context, result.valid());
  BOOMPI_EXPECT(context, result.accepted());
  BOOMPI_EXPECT(context,
                result.queued_sample_frames_before_write ==
                    PcmPlaybackWriteResult::kMaximumQueuedSampleFrames);
  BOOMPI_EXPECT(context,
                result.estimated_presentation_timestamp_us == 12999958U);

  constexpr std::array<std::array<std::uint64_t, 3U>, 4U>
      kInvalidClockAndTimestampCases{{
          {{9300100U, 0U, 9300100U}},
          {{9300200U, 9300100U, 9300100U}},
          {{9300100U, 9300200U, 9300201U}},
          {{9300100U, 9300200U, 9300099U}},
      }};
  for (const auto &test_case : kInvalidClockAndTimestampCases) {
    device.SetStatuses(
        SuccessfulStatus(0U, 9300000U, PcmPlaybackTimelineState::kPrepared),
        SuccessfulStatus(2U, test_case[2U],
                         PcmPlaybackTimelineState::kRunning));
    device.SetMonotonicReadings(test_case[0U], test_case[1U]);
    result = sink.TryWriteInterleavedS16(samples.data(), 4U);
    ExpectMalformedPositive(context, result, PcmPlaybackWriteCode::kFatal, 2U,
                            0);
  }
}

struct ControlCodeCase final {
  PcmPlaybackDeviceCode device_code;
  PcmPlaybackControlCode sink_code;
};

constexpr std::array<ControlCodeCase, 10U> kControlCases{{
    {PcmPlaybackDeviceCode::kUnset, PcmPlaybackControlCode::kFatal},
    {PcmPlaybackDeviceCode::kSucceeded, PcmPlaybackControlCode::kSucceeded},
    {PcmPlaybackDeviceCode::kWouldBlock, PcmPlaybackControlCode::kFatal},
    {PcmPlaybackDeviceCode::kInterrupted, PcmPlaybackControlCode::kInterrupted},
    {PcmPlaybackDeviceCode::kXrun, PcmPlaybackControlCode::kFatal},
    {PcmPlaybackDeviceCode::kSuspended, PcmPlaybackControlCode::kFatal},
    {PcmPlaybackDeviceCode::kDeviceLost, PcmPlaybackControlCode::kDeviceLost},
    {PcmPlaybackDeviceCode::kZeroProgress, PcmPlaybackControlCode::kFatal},
    {PcmPlaybackDeviceCode::kFatal, PcmPlaybackControlCode::kFatal},
    {PcmPlaybackDeviceCode::kUnavailable, PcmPlaybackControlCode::kUnavailable},
}};

void TestDropPrepareMappingsAndCallCounts(boompi::test::TestContext &context) {
  FakePlaybackDevice device;
  PcmPlaybackSink48k sink;
  if (!ConfigureSink(context, &device, &sink)) {
    return;
  }

  for (std::size_t index = 0U; index < kControlCases.size(); ++index) {
    const auto &test_case = kControlCases[index];
    const auto native_error =
        test_case.device_code == PcmPlaybackDeviceCode::kSucceeded
            ? 0
            : -static_cast<std::int32_t>(400U + index);
    device.drop_result = {test_case.device_code, native_error};
    device.prepare_result = {test_case.device_code, native_error};

    const auto drop = sink.Drop();
    const auto prepare = sink.Prepare();
    BOOMPI_EXPECT(context, drop.code == test_case.sink_code);
    BOOMPI_EXPECT(context, prepare.code == test_case.sink_code);
    BOOMPI_EXPECT(context, drop.native_error == native_error);
    BOOMPI_EXPECT(context, prepare.native_error == native_error);
    BOOMPI_EXPECT(context, drop.valid());
    BOOMPI_EXPECT(context, prepare.valid());
    BOOMPI_EXPECT(context,
                  drop.succeeded() == (test_case.sink_code ==
                                       PcmPlaybackControlCode::kSucceeded));
    BOOMPI_EXPECT(context,
                  prepare.succeeded() == (test_case.sink_code ==
                                          PcmPlaybackControlCode::kSucceeded));
  }

  device.drop_result = {PcmPlaybackDeviceCode::kSucceeded, -5};
  device.prepare_result = {PcmPlaybackDeviceCode::kSucceeded, -6};
  auto drop = sink.Drop();
  auto prepare = sink.Prepare();
  BOOMPI_EXPECT(context, drop.code == PcmPlaybackControlCode::kFatal);
  BOOMPI_EXPECT(context, prepare.code == PcmPlaybackControlCode::kFatal);
  BOOMPI_EXPECT(context, drop.native_error == -5);
  BOOMPI_EXPECT(context, prepare.native_error == -6);
  BOOMPI_EXPECT(context, drop.valid());
  BOOMPI_EXPECT(context, prepare.valid());

  device.drop_result = {static_cast<PcmPlaybackDeviceCode>(0xFFU), -7};
  device.prepare_result = {static_cast<PcmPlaybackDeviceCode>(0xFFU), -8};
  drop = sink.Drop();
  prepare = sink.Prepare();
  BOOMPI_EXPECT(context, drop.code == PcmPlaybackControlCode::kFatal);
  BOOMPI_EXPECT(context, prepare.code == PcmPlaybackControlCode::kFatal);
  BOOMPI_EXPECT(context, drop.native_error == -7);
  BOOMPI_EXPECT(context, prepare.native_error == -8);
  BOOMPI_EXPECT(context, drop.valid());
  BOOMPI_EXPECT(context, prepare.valid());

  BOOMPI_EXPECT(context, device.drop_call_count == kControlCases.size() + 2U);
  BOOMPI_EXPECT(context,
                device.prepare_call_count == kControlCases.size() + 2U);
  BOOMPI_EXPECT(context, device.status_call_count == 0U);
  BOOMPI_EXPECT(context, device.write_call_count == 0U);
  BOOMPI_EXPECT(context, device.clock_call_count == 0U);
}

} // namespace

int main() {
  boompi::test::TestContext context;
  TestDeviceConfigContract(context);
  TestCreateValidation(context);
  TestUnconfiguredAndInvalidWriteArguments(context);
  TestMonoFullAndPartialWrites(context);
  TestStereoDuplication(context);
  TestStatusFailuresCauseZeroWrites(context);
  TestZeroProgressAndAllWriteCodes(context);
  TestOverReportAndMalformedPositiveWrites(context);
  TestPostWriteStatusFailures(context);
  TestClockTimestampAndOverflowPolicy(context);
  TestDropPrepareMappingsAndCallCounts(context);

  if (context.failures() != 0U) {
    std::cerr << context.failures()
              << " PCM playback adapter expectation(s) failed\n";
    return 1;
  }
  std::cout << "boomPI PCM playback adapter tests passed\n";
  return 0;
}
