#include "boompi/test/scripted_pcm_playback_sink.h"

#include <cstdint>
#include <type_traits>

namespace boompi::test {
namespace {

constexpr audio::PcmPlaybackWriteResult MakeEmptyWriteResult(
    const audio::PcmPlaybackWriteCode code,
    const std::int32_t native_error = 0) noexcept {
  return {code, 0U, 0U, 0U, 0U, audio::PlaybackTimingSource::kUnset,
          native_error};
}

constexpr audio::PcmPlaybackWriteResult kUnavailableWriteResult =
    MakeEmptyWriteResult(audio::PcmPlaybackWriteCode::kUnavailable);
constexpr audio::PcmPlaybackWriteResult kFatalWriteResult =
    MakeEmptyWriteResult(audio::PcmPlaybackWriteCode::kFatal);
constexpr audio::PcmPlaybackControlResult kSucceededControlResult{
    audio::PcmPlaybackControlCode::kSucceeded, 0};
constexpr audio::PcmPlaybackControlResult kUnavailableControlResult{
    audio::PcmPlaybackControlCode::kUnavailable, 0};

static_assert(std::is_trivial<ScriptedPcmPlaybackTraceEntry>::value,
              "scripted PCM trace entries must remain POD values");
static_assert(
    std::is_standard_layout<ScriptedPcmPlaybackTraceEntry>::value,
    "scripted PCM trace entries must remain standard-layout");
static_assert(kUnavailableWriteResult.valid() &&
                  kFatalWriteResult.valid() &&
                  kSucceededControlResult.valid() &&
                  kUnavailableControlResult.valid(),
              "scripted PCM default results must be self-consistent");

}  // namespace

bool ScriptedPcmPlaybackSink::PushWriteResult(
    const audio::PcmPlaybackWriteResult& result) noexcept {
  if (!result.valid()) {
    return false;
  }
  return PushUncheckedWriteResult(result);
}

bool ScriptedPcmPlaybackSink::PushUncheckedWriteResultForProtocolTest(
    const audio::PcmPlaybackWriteResult& result) noexcept {
  return PushUncheckedWriteResult(result);
}

bool ScriptedPcmPlaybackSink::PushUncheckedWriteResult(
    const audio::PcmPlaybackWriteResult& result) noexcept {
  if (write_script_size_ >= write_script_.size()) {
    return false;
  }
  write_script_[write_script_size_] = result;
  ++write_script_size_;
  return true;
}

bool ScriptedPcmPlaybackSink::PushAccepted(
    const std::uint16_t accepted_sample_frames,
    const std::uint64_t write_completed_timestamp_us,
    const std::uint64_t estimated_presentation_timestamp_us,
    const std::uint32_t queued_sample_frames_before_write,
    const audio::PlaybackTimingSource timing_source) noexcept {
  return PushWriteResult(
      {audio::PcmPlaybackWriteCode::kAccepted, accepted_sample_frames,
       write_completed_timestamp_us,
       estimated_presentation_timestamp_us,
       queued_sample_frames_before_write, timing_source, 0});
}

bool ScriptedPcmPlaybackSink::PushWriteCode(
    const audio::PcmPlaybackWriteCode code,
    const std::int32_t native_error) noexcept {
  if (code == audio::PcmPlaybackWriteCode::kAccepted) {
    return false;
  }
  return PushWriteResult(MakeEmptyWriteResult(code, native_error));
}

bool ScriptedPcmPlaybackSink::PushDropResult(
    const audio::PcmPlaybackControlResult& result) noexcept {
  if (!result.valid()) {
    return false;
  }
  return PushUncheckedControlResult(result, &drop_script_,
                                    &drop_script_size_);
}

bool ScriptedPcmPlaybackSink::PushPrepareResult(
    const audio::PcmPlaybackControlResult& result) noexcept {
  if (!result.valid()) {
    return false;
  }
  return PushUncheckedControlResult(result, &prepare_script_,
                                    &prepare_script_size_);
}

bool ScriptedPcmPlaybackSink::PushUncheckedDropResultForProtocolTest(
    const audio::PcmPlaybackControlResult& result) noexcept {
  return PushUncheckedControlResult(result, &drop_script_,
                                    &drop_script_size_);
}

bool ScriptedPcmPlaybackSink::PushUncheckedPrepareResultForProtocolTest(
    const audio::PcmPlaybackControlResult& result) noexcept {
  return PushUncheckedControlResult(result, &prepare_script_,
                                    &prepare_script_size_);
}

bool ScriptedPcmPlaybackSink::PushUncheckedControlResult(
    const audio::PcmPlaybackControlResult& result,
    std::array<audio::PcmPlaybackControlResult,
               kMaximumControlSteps>* const script,
    std::size_t* const script_size) noexcept {
  if (*script_size >= script->size()) {
    return false;
  }
  (*script)[*script_size] = result;
  ++(*script_size);
  return true;
}

void ScriptedPcmPlaybackSink::SetPreWriteHook(
    const WriteHook hook, void* const context) noexcept {
  pre_write_hook_ = hook;
  pre_write_hook_context_ = context;
}

void ScriptedPcmPlaybackSink::SetPostWriteHook(
    const WriteHook hook, void* const context) noexcept {
  post_write_hook_ = hook;
  post_write_hook_context_ = context;
}

void ScriptedPcmPlaybackSink::ClearWriteHooks() noexcept {
  pre_write_hook_ = nullptr;
  pre_write_hook_context_ = nullptr;
  post_write_hook_ = nullptr;
  post_write_hook_context_ = nullptr;
}

void ScriptedPcmPlaybackSink::ClearScripts() noexcept {
  write_script_size_ = 0U;
  next_write_step_ = 0U;
  drop_script_size_ = 0U;
  next_drop_step_ = 0U;
  prepare_script_size_ = 0U;
  next_prepare_step_ = 0U;
}

void ScriptedPcmPlaybackSink::ResetTrace() noexcept {
  trace_size_ = 0U;
  trace_overflowed_ = false;
  write_call_count_ = 0U;
  drop_call_count_ = 0U;
  prepare_call_count_ = 0U;
}

std::size_t ScriptedPcmPlaybackSink::pending_write_steps() const noexcept {
  return write_script_size_ - next_write_step_;
}

std::size_t ScriptedPcmPlaybackSink::pending_drop_steps() const noexcept {
  return drop_script_size_ - next_drop_step_;
}

std::size_t ScriptedPcmPlaybackSink::pending_prepare_steps() const noexcept {
  return prepare_script_size_ - next_prepare_step_;
}

const ScriptedPcmPlaybackTraceEntry*
ScriptedPcmPlaybackSink::trace_entry(const std::size_t index) const noexcept {
  return index < trace_size_ ? &trace_[index] : nullptr;
}

audio::PcmPlaybackWriteResult
ScriptedPcmPlaybackSink::TryWriteInterleavedS16(
    const std::int16_t* const samples,
    const std::uint16_t frames) noexcept {
  ++write_call_count_;
  if (pre_write_hook_ != nullptr) {
    pre_write_hook_(pre_write_hook_context_);
  }

  audio::PcmPlaybackWriteResult result = kFatalWriteResult;
  if (samples != nullptr && frames != 0U) {
    if (next_write_step_ < write_script_size_) {
      result = write_script_[next_write_step_];
      ++next_write_step_;
    } else {
      result = kUnavailableWriteResult;
    }
  }

  RecordWrite(samples, frames, result);
  if (post_write_hook_ != nullptr) {
    post_write_hook_(post_write_hook_context_);
  }
  return result;
}

audio::PcmPlaybackControlResult ScriptedPcmPlaybackSink::Drop() noexcept {
  ++drop_call_count_;
  const audio::PcmPlaybackControlResult result = NextControlResult(
      drop_script_, drop_script_size_, &next_drop_step_);
  RecordControl(ScriptedPcmPlaybackCall::kDrop, result);
  return result;
}

audio::PcmPlaybackControlResult ScriptedPcmPlaybackSink::Prepare() noexcept {
  ++prepare_call_count_;
  const audio::PcmPlaybackControlResult result = NextControlResult(
      prepare_script_, prepare_script_size_, &next_prepare_step_);
  RecordControl(ScriptedPcmPlaybackCall::kPrepare, result);
  return result;
}

audio::PcmPlaybackControlResult
ScriptedPcmPlaybackSink::NextControlResult(
    const std::array<audio::PcmPlaybackControlResult,
                     kMaximumControlSteps>& script,
    const std::size_t script_size,
    std::size_t* const next_index) noexcept {
  if (*next_index >= script_size) {
    return kUnavailableControlResult;
  }
  const audio::PcmPlaybackControlResult result = script[*next_index];
  ++(*next_index);
  return result;
}

void ScriptedPcmPlaybackSink::RecordWrite(
    const std::int16_t* const samples, const std::uint16_t frames,
    const audio::PcmPlaybackWriteResult& result) noexcept {
  if (trace_size_ >= trace_.size()) {
    trace_overflowed_ = true;
    return;
  }
  trace_[trace_size_] = {
      ScriptedPcmPlaybackCall::kWrite,
      reinterpret_cast<std::uintptr_t>(samples),
      frames,
      samples != nullptr && frames != 0U
          ? samples[0]
          : static_cast<std::int16_t>(0),
      samples != nullptr && frames != 0U,
      result,
      kSucceededControlResult};
  ++trace_size_;
}

void ScriptedPcmPlaybackSink::RecordControl(
    const ScriptedPcmPlaybackCall call,
    const audio::PcmPlaybackControlResult& result) noexcept {
  if (trace_size_ >= trace_.size()) {
    trace_overflowed_ = true;
    return;
  }
  trace_[trace_size_] = {
      call, 0U, 0U, 0, false, kUnavailableWriteResult, result};
  ++trace_size_;
}

}  // namespace boompi::test
