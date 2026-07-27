#ifndef BOOMPI_TEST_SCRIPTED_PCM_PLAYBACK_SINK_H_
#define BOOMPI_TEST_SCRIPTED_PCM_PLAYBACK_SINK_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "boompi/audio/pcm_playback_sink.h"

namespace boompi::test {

enum class ScriptedPcmPlaybackCall : std::uint8_t {
  kWrite = 0,
  kDrop,
  kPrepare,
};

struct ScriptedPcmPlaybackTraceEntry final {
  ScriptedPcmPlaybackCall call;
  std::uintptr_t sample_address;
  std::uint16_t requested_frames;
  std::int16_t first_sample;
  bool has_first_sample;
  audio::PcmPlaybackWriteResult write_result;
  audio::PcmPlaybackControlResult control_result;
};

// Fixed-capacity deterministic fake for playback worker tests. Scripts and
// traces never allocate. Hook callbacks run synchronously on the caller's
// thread: pre-write runs before argument validation/script consumption;
// post-write runs after the trace entry is visible and before return. Hooks
// must not throw, block, sleep, or recursively call this sink.
class ScriptedPcmPlaybackSink final : public audio::PcmPlaybackSink {
 public:
  using WriteHook = void (*)(void* context) noexcept;

  static constexpr std::size_t kMaximumWriteSteps = 128U;
  static constexpr std::size_t kMaximumControlSteps = 32U;
  static constexpr std::size_t kMaximumTraceEntries = 256U;

  ScriptedPcmPlaybackSink() noexcept = default;

  bool PushWriteResult(
      const audio::PcmPlaybackWriteResult& result) noexcept;
  // These explicitly unsafe injection points exist only to verify that a
  // consumer fails closed on malformed backend results.
  bool PushUncheckedWriteResultForProtocolTest(
      const audio::PcmPlaybackWriteResult& result) noexcept;
  bool PushAccepted(
      std::uint16_t accepted_sample_frames,
      std::uint64_t write_completed_timestamp_us,
      std::uint64_t estimated_presentation_timestamp_us,
      std::uint32_t queued_sample_frames_before_write,
      audio::PlaybackTimingSource timing_source) noexcept;
  bool PushWriteCode(audio::PcmPlaybackWriteCode code,
                     std::int32_t native_error = 0) noexcept;

  bool PushDropResult(
      const audio::PcmPlaybackControlResult& result) noexcept;
  bool PushPrepareResult(
      const audio::PcmPlaybackControlResult& result) noexcept;
  bool PushUncheckedDropResultForProtocolTest(
      const audio::PcmPlaybackControlResult& result) noexcept;
  bool PushUncheckedPrepareResultForProtocolTest(
      const audio::PcmPlaybackControlResult& result) noexcept;

  void SetPreWriteHook(WriteHook hook, void* context) noexcept;
  void SetPostWriteHook(WriteHook hook, void* context) noexcept;
  void ClearWriteHooks() noexcept;

  void ClearScripts() noexcept;
  void ResetTrace() noexcept;

  std::size_t pending_write_steps() const noexcept;
  std::size_t pending_drop_steps() const noexcept;
  std::size_t pending_prepare_steps() const noexcept;
  std::size_t trace_size() const noexcept { return trace_size_; }
  const ScriptedPcmPlaybackTraceEntry* trace_entry(
      std::size_t index) const noexcept;
  bool trace_overflowed() const noexcept { return trace_overflowed_; }
  std::uint32_t write_call_count() const noexcept {
    return write_call_count_;
  }
  std::uint32_t drop_call_count() const noexcept {
    return drop_call_count_;
  }
  std::uint32_t prepare_call_count() const noexcept {
    return prepare_call_count_;
  }

  audio::PcmPlaybackWriteResult TryWriteInterleavedS16(
      const std::int16_t* samples,
      std::uint16_t frames) noexcept override;
  audio::PcmPlaybackControlResult Drop() noexcept override;
  audio::PcmPlaybackControlResult Prepare() noexcept override;

 private:
  audio::PcmPlaybackControlResult NextControlResult(
      const std::array<audio::PcmPlaybackControlResult,
                       kMaximumControlSteps>& script,
      std::size_t script_size,
      std::size_t* next_index) noexcept;
  bool PushUncheckedWriteResult(
      const audio::PcmPlaybackWriteResult& result) noexcept;
  bool PushUncheckedControlResult(
      const audio::PcmPlaybackControlResult& result,
      std::array<audio::PcmPlaybackControlResult,
                 kMaximumControlSteps>* script,
      std::size_t* script_size) noexcept;
  void RecordWrite(const std::int16_t* samples, std::uint16_t frames,
                   const audio::PcmPlaybackWriteResult& result) noexcept;
  void RecordControl(ScriptedPcmPlaybackCall call,
                     const audio::PcmPlaybackControlResult& result) noexcept;

  std::array<audio::PcmPlaybackWriteResult, kMaximumWriteSteps>
      write_script_{};
  std::array<audio::PcmPlaybackControlResult, kMaximumControlSteps>
      drop_script_{};
  std::array<audio::PcmPlaybackControlResult, kMaximumControlSteps>
      prepare_script_{};
  std::array<ScriptedPcmPlaybackTraceEntry, kMaximumTraceEntries> trace_{};
  std::size_t write_script_size_{0U};
  std::size_t next_write_step_{0U};
  std::size_t drop_script_size_{0U};
  std::size_t next_drop_step_{0U};
  std::size_t prepare_script_size_{0U};
  std::size_t next_prepare_step_{0U};
  std::size_t trace_size_{0U};
  WriteHook pre_write_hook_{nullptr};
  void* pre_write_hook_context_{nullptr};
  WriteHook post_write_hook_{nullptr};
  void* post_write_hook_context_{nullptr};
  std::uint32_t write_call_count_{0U};
  std::uint32_t drop_call_count_{0U};
  std::uint32_t prepare_call_count_{0U};
  bool trace_overflowed_{false};
};

}  // namespace boompi::test

#endif  // BOOMPI_TEST_SCRIPTED_PCM_PLAYBACK_SINK_H_
