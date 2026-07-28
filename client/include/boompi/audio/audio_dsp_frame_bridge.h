#ifndef BOOMPI_AUDIO_AUDIO_DSP_FRAME_BRIDGE_H_
#define BOOMPI_AUDIO_AUDIO_DSP_FRAME_BRIDGE_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "boompi/audio/audio_frame.h"
#include "boompi/audio/capture_dsp_frontend.h"
#include "boompi/event/status.h"

namespace boompi::audio {

constexpr std::size_t kDspBlockSamplesPerChannel16k = 256U;
constexpr std::size_t kDspFrameBridgeOutputCapacity = 4U;

// A backend block is 16 ms at 16 kHz. The four planar inputs retain the
// logical CaptureRole order; this type does not assert any vendor packing or
// physical ALSA slot order.
using DspCapturePlane16k16ms =
    std::array<std::int16_t, kDspBlockSamplesPerChannel16k>;
using DspCapturePlanes16k16ms =
    std::array<DspCapturePlane16k16ms, kDspCaptureChannelCount>;
using DspMonoBlock16k16ms =
    std::array<std::int16_t, kDspBlockSamplesPerChannel16k>;

enum class AudioDspBlockProcessCode : std::uint8_t {
  kUnset = 0,
  kProduced,
  kBackendFailure,
};

struct AudioDspBlockProcessResult final {
  AudioDspBlockProcessCode code;
  // Counted in mono samples after the platform adapter translates any vendor
  // byte-count return. Only the exact fixed block size is accepted.
  std::uint32_t produced_samples;

  constexpr bool produced_exact_block() const noexcept {
    return code == AudioDspBlockProcessCode::kProduced &&
           produced_samples == kDspBlockSamplesPerChannel16k;
  }
};

// The platform adapter implements only one exact 16 ms transform. Process is
// called by one DSP worker, must not allocate, lock, log, perform I/O, retain
// either pointer, or report success unless all 256 mono samples were written.
class AudioDspBlockProcessor16k {
 public:
  virtual ~AudioDspBlockProcessor16k() = default;

  AudioDspBlockProcessor16k(const AudioDspBlockProcessor16k&) = delete;
  AudioDspBlockProcessor16k& operator=(const AudioDspBlockProcessor16k&) =
      delete;
  AudioDspBlockProcessor16k(AudioDspBlockProcessor16k&&) = delete;
  AudioDspBlockProcessor16k& operator=(AudioDspBlockProcessor16k&&) = delete;

  virtual AudioDspBlockProcessResult Process(
      const DspCapturePlanes16k16ms& input,
      DspMonoBlock16k16ms* output) noexcept = 0;

 protected:
  AudioDspBlockProcessor16k() = default;
};

enum class AudioDspFrameBridgeCode : std::uint8_t {
  kUnset = 0,
  kNeedMoreInput,
  kOutputAvailable,
  kNotConfigured,
  kNotArmed,
  kEpochMismatch,
  kStreamMismatch,
  kDiscontinuity,
  kBackendFailure,
  kOverflow,
  kInvalidOutput,
  kFaulted,
};

struct AudioDspFrameBridgeResult final {
  AudioDspFrameBridgeCode code;
  std::uint32_t processed_blocks;
  std::uint32_t available_outputs;

  constexpr bool output_available() const noexcept {
    return code == AudioDspFrameBridgeCode::kOutputAvailable;
  }

  constexpr bool needs_more_input() const noexcept {
    return code == AudioDspFrameBridgeCode::kNeedMoreInput;
  }

  constexpr bool requires_new_generation() const noexcept {
    return code == AudioDspFrameBridgeCode::kDiscontinuity ||
           code == AudioDspFrameBridgeCode::kBackendFailure ||
           code == AudioDspFrameBridgeCode::kOverflow ||
           code == AudioDspFrameBridgeCode::kFaulted;
  }
};

// Allocation-free 20 ms <-> 16 ms stream framing for a future Rockchip
// adapter. One worker exclusively owns the bridge and its externally-owned
// processor. Process consumes exactly one 4 x 320 input frame, synchronously
// invokes one to two exact 4 x 256 -> mono 256 backend blocks, and queues each
// complete mono 320 output. TryPop returns queued 20 ms frames in stream order.
//
// Arm and Disarm only reset bridge framing. After bridge Arm succeeds, the
// owning platform adapter resets/recreates its backend before the first
// Process. A backend reset failure must Disarm the bridge; the next recovery
// uses a newer epoch. Any result that requires a new generation also requires
// backend recreation. A backend fault invalidates all buffered input, output,
// and metadata; recovery within the same generation is forbidden.
class AudioDspFrameBridge16k final {
 public:
  AudioDspFrameBridge16k() noexcept = default;
  AudioDspFrameBridge16k(const AudioDspFrameBridge16k&) = delete;
  AudioDspFrameBridge16k& operator=(const AudioDspFrameBridge16k&) = delete;
  AudioDspFrameBridge16k(AudioDspFrameBridge16k&&) = delete;
  AudioDspFrameBridge16k& operator=(AudioDspFrameBridge16k&&) = delete;

  // The processor remains externally owned and must outlive the bridge.
  static Status Create(AudioDspBlockProcessor16k* processor,
                       AudioDspFrameBridge16k* output);

  // Epochs are strictly increasing for this instance. Every successful Arm
  // starts empty; a rejected Arm preserves the current stream and buffers.
  Status Arm(std::uint32_t epoch, std::uint32_t stream_id);
  void Disarm() noexcept;

  // kNeedMoreInput is a successful consumption with no complete 20 ms output.
  // kOutputAvailable means available_outputs can be drained with TryPop. Wrong
  // generation frames are ignored without changing backend or bridge state.
  // Every queued output takes metadata from the earliest buffered 20 ms input,
  // including the two outputs made by the fourth call in each 80 ms cycle.
  // Sequence and timestamp continuity must already have passed the upstream
  // CaptureDspFrontend gate; this bridge only enforces generation identity and
  // an explicit discontinuity marker and must not accept hand-built bypasses.
  AudioDspFrameBridgeResult Process(
      const DspCaptureFrame16k& input) noexcept;

  // A successful pop returns kOutputAvailable and one valid frame. When the
  // queue is empty, kNeedMoreInput resets a non-null output header. A null
  // output never consumes a queued frame.
  AudioDspFrameBridgeResult TryPop(Mono16kFrame* output) noexcept;

 private:
  static constexpr std::size_t kInputScratchCapacity =
      kDspBlockSamplesPerChannel16k + kSamplesPer16k20ms;
  static constexpr std::size_t kOutputScratchCapacity =
      kDspBlockSamplesPerChannel16k + kSamplesPer16k20ms;
  static constexpr std::size_t kMetadataCapacity =
      kDspFrameBridgeOutputCapacity + 2U;

  void Flush() noexcept;
  void LatchFault() noexcept;
  bool EnqueueMetadata(const AudioFrameMetadata& metadata) noexcept;
  AudioFrameMetadata DequeueMetadata() noexcept;
  void EnqueueOutput() noexcept;

  AudioDspBlockProcessor16k* processor_{nullptr};
  std::array<std::array<std::int16_t, kInputScratchCapacity>,
             kDspCaptureChannelCount>
      input_scratch_{};
  std::array<std::int16_t, kOutputScratchCapacity> output_scratch_{};
  DspCapturePlanes16k16ms block_input_{};
  DspMonoBlock16k16ms block_output_{};
  std::array<AudioFrameMetadata, kMetadataCapacity> metadata_queue_{};
  std::array<Mono16kFrame, kDspFrameBridgeOutputCapacity> output_queue_{};
  std::size_t input_samples_{0U};
  std::size_t output_samples_{0U};
  std::size_t metadata_head_{0U};
  std::size_t metadata_tail_{0U};
  std::size_t metadata_count_{0U};
  std::size_t output_head_{0U};
  std::size_t output_tail_{0U};
  std::size_t output_count_{0U};
  std::uint32_t epoch_{0U};
  std::uint32_t stream_id_{0U};
  std::uint32_t greatest_epoch_{0U};
  bool configured_{false};
  bool armed_{false};
  bool faulted_{false};
};

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_AUDIO_DSP_FRAME_BRIDGE_H_
