#include "boompi/audio/audio_dsp_frame_bridge.h"

#include <algorithm>
#include <type_traits>

namespace boompi::audio {
namespace {

static_assert(std::is_trivial<AudioDspBlockProcessResult>::value,
              "DSP block process result must remain a POD value");
static_assert(std::is_standard_layout<AudioDspBlockProcessResult>::value,
              "DSP block process result must remain standard-layout");
static_assert(std::is_trivial<AudioDspFrameBridgeResult>::value,
              "DSP frame bridge result must remain a POD value");
static_assert(std::is_standard_layout<AudioDspFrameBridgeResult>::value,
              "DSP frame bridge result must remain standard-layout");

constexpr AudioFormat kMono16k20msFormat{
    16000U, 20U, 1U, SampleFormat::kPcmS16Le};

constexpr AudioDspFrameBridgeResult MakeResult(
    const AudioDspFrameBridgeCode code,
    const std::size_t processed_blocks,
    const std::size_t available_outputs) noexcept {
  return {code, static_cast<std::uint32_t>(processed_blocks),
          static_cast<std::uint32_t>(available_outputs)};
}

}  // namespace

Status AudioDspFrameBridge16k::Create(
    AudioDspBlockProcessor16k* const processor,
    AudioDspFrameBridge16k* const output) {
  if (processor == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "DSP frame bridge processor must not be null");
  }
  if (output == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "DSP frame bridge output must not be null");
  }
  if (output->configured_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "DSP frame bridge is already configured");
  }

  output->processor_ = processor;
  output->configured_ = true;
  output->Flush();
  return Status::Ok();
}

Status AudioDspFrameBridge16k::Arm(const std::uint32_t epoch,
                                   const std::uint32_t stream_id) {
  if (!configured_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "DSP frame bridge is not configured");
  }
  if (epoch == 0U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "DSP frame bridge epoch must be non-zero");
  }
  if (stream_id == 0U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "DSP frame bridge stream id must be non-zero");
  }
  if (epoch <= greatest_epoch_) {
    return Status::Error(
        StatusCode::kFailedPrecondition,
        "DSP frame bridge Arm requires a strictly increasing epoch");
  }

  Flush();
  epoch_ = epoch;
  stream_id_ = stream_id;
  greatest_epoch_ = epoch;
  armed_ = true;
  return Status::Ok();
}

void AudioDspFrameBridge16k::Disarm() noexcept {
  Flush();
  epoch_ = 0U;
  stream_id_ = 0U;
  armed_ = false;
}

AudioDspFrameBridgeResult AudioDspFrameBridge16k::Process(
    const DspCaptureFrame16k& input) noexcept {
  if (!configured_) {
    return MakeResult(AudioDspFrameBridgeCode::kNotConfigured, 0U,
                      output_count_);
  }
  if (!armed_) {
    return MakeResult(AudioDspFrameBridgeCode::kNotArmed, 0U,
                      output_count_);
  }
  if (input.metadata.epoch != epoch_) {
    return MakeResult(AudioDspFrameBridgeCode::kEpochMismatch, 0U,
                      output_count_);
  }
  if (input.metadata.stream_id != stream_id_) {
    return MakeResult(AudioDspFrameBridgeCode::kStreamMismatch, 0U,
                      output_count_);
  }
  if (faulted_) {
    return MakeResult(AudioDspFrameBridgeCode::kFaulted, 0U, 0U);
  }
  if (input.metadata.discontinuity) {
    LatchFault();
    return MakeResult(AudioDspFrameBridgeCode::kDiscontinuity, 0U, 0U);
  }

  const std::size_t samples_after_append =
      input_samples_ + kSamplesPer16k20ms;
  const std::size_t prospective_blocks =
      samples_after_append / kDspBlockSamplesPerChannel16k;
  const std::size_t prospective_outputs =
      (output_samples_ +
       prospective_blocks * kDspBlockSamplesPerChannel16k) /
      kSamplesPer16k20ms;
  if (samples_after_append > kInputScratchCapacity ||
      metadata_count_ >= kMetadataCapacity ||
      prospective_outputs >
          kDspFrameBridgeOutputCapacity - output_count_) {
    LatchFault();
    return MakeResult(AudioDspFrameBridgeCode::kOverflow, 0U, 0U);
  }

  for (std::size_t channel = 0U; channel < kDspCaptureChannelCount;
       ++channel) {
    std::copy(input.planes[channel].begin(), input.planes[channel].end(),
              input_scratch_[channel].begin() + input_samples_);
  }
  input_samples_ = samples_after_append;
  if (!EnqueueMetadata(input.metadata)) {
    LatchFault();
    return MakeResult(AudioDspFrameBridgeCode::kOverflow, 0U, 0U);
  }

  std::size_t processed_blocks = 0U;
  while (input_samples_ >= kDspBlockSamplesPerChannel16k) {
    for (std::size_t channel = 0U; channel < kDspCaptureChannelCount;
         ++channel) {
      std::copy_n(input_scratch_[channel].begin(),
                  kDspBlockSamplesPerChannel16k,
                  block_input_[channel].begin());
    }

    const auto backend_result =
        processor_->Process(block_input_, &block_output_);
    if (!backend_result.produced_exact_block()) {
      LatchFault();
      return MakeResult(AudioDspFrameBridgeCode::kBackendFailure,
                        processed_blocks, 0U);
    }
    ++processed_blocks;

    const std::size_t input_remaining =
        input_samples_ - kDspBlockSamplesPerChannel16k;
    for (std::size_t channel = 0U; channel < kDspCaptureChannelCount;
         ++channel) {
      std::move(input_scratch_[channel].begin() +
                    kDspBlockSamplesPerChannel16k,
                input_scratch_[channel].begin() + input_samples_,
                input_scratch_[channel].begin());
    }
    input_samples_ = input_remaining;

    std::copy(block_output_.begin(), block_output_.end(),
              output_scratch_.begin() + output_samples_);
    output_samples_ += kDspBlockSamplesPerChannel16k;
    while (output_samples_ >= kSamplesPer16k20ms) {
      if (metadata_count_ == 0U ||
          output_count_ >= kDspFrameBridgeOutputCapacity) {
        LatchFault();
        return MakeResult(AudioDspFrameBridgeCode::kOverflow,
                          processed_blocks, 0U);
      }
      EnqueueOutput();
    }
  }

  const auto code = output_count_ == 0U
                        ? AudioDspFrameBridgeCode::kNeedMoreInput
                        : AudioDspFrameBridgeCode::kOutputAvailable;
  return MakeResult(code, processed_blocks, output_count_);
}

AudioDspFrameBridgeResult AudioDspFrameBridge16k::TryPop(
    Mono16kFrame* const output) noexcept {
  if (output == nullptr) {
    return MakeResult(AudioDspFrameBridgeCode::kInvalidOutput, 0U,
                      output_count_);
  }
  output->ResetHeader();
  if (!configured_) {
    return MakeResult(AudioDspFrameBridgeCode::kNotConfigured, 0U,
                      output_count_);
  }
  if (!armed_) {
    return MakeResult(AudioDspFrameBridgeCode::kNotArmed, 0U,
                      output_count_);
  }
  if (faulted_) {
    return MakeResult(AudioDspFrameBridgeCode::kFaulted, 0U, 0U);
  }
  if (output_count_ == 0U) {
    return MakeResult(AudioDspFrameBridgeCode::kNeedMoreInput, 0U, 0U);
  }

  *output = output_queue_[output_head_];
  output_head_ = (output_head_ + 1U) % kDspFrameBridgeOutputCapacity;
  --output_count_;
  return MakeResult(AudioDspFrameBridgeCode::kOutputAvailable, 0U,
                    output_count_);
}

void AudioDspFrameBridge16k::Flush() noexcept {
  input_samples_ = 0U;
  output_samples_ = 0U;
  metadata_head_ = 0U;
  metadata_tail_ = 0U;
  metadata_count_ = 0U;
  output_head_ = 0U;
  output_tail_ = 0U;
  output_count_ = 0U;
  faulted_ = false;
}

void AudioDspFrameBridge16k::LatchFault() noexcept {
  Flush();
  faulted_ = true;
}

bool AudioDspFrameBridge16k::EnqueueMetadata(
    const AudioFrameMetadata& metadata) noexcept {
  if (metadata_count_ >= kMetadataCapacity) {
    return false;
  }
  metadata_queue_[metadata_tail_] = metadata;
  metadata_tail_ = (metadata_tail_ + 1U) % kMetadataCapacity;
  ++metadata_count_;
  return true;
}

AudioFrameMetadata AudioDspFrameBridge16k::DequeueMetadata() noexcept {
  const AudioFrameMetadata metadata = metadata_queue_[metadata_head_];
  metadata_head_ = (metadata_head_ + 1U) % kMetadataCapacity;
  --metadata_count_;
  return metadata;
}

void AudioDspFrameBridge16k::EnqueueOutput() noexcept {
  Mono16kFrame& output = output_queue_[output_tail_];
  output.format = kMono16k20msFormat;
  output.metadata = DequeueMetadata();
  output.samples_per_channel =
      static_cast<std::uint32_t>(kSamplesPer16k20ms);
  std::copy_n(output_scratch_.begin(), kSamplesPer16k20ms,
              output.samples.begin());
  output_tail_ = (output_tail_ + 1U) % kDspFrameBridgeOutputCapacity;
  ++output_count_;

  const std::size_t output_remaining =
      output_samples_ - kSamplesPer16k20ms;
  std::move(output_scratch_.begin() + kSamplesPer16k20ms,
            output_scratch_.begin() + output_samples_,
            output_scratch_.begin());
  output_samples_ = output_remaining;
}

}  // namespace boompi::audio
