#ifndef BOOMPI_PLATFORM_RV1106_ALSA_SINGLE_TURN_IO_H_
#define BOOMPI_PLATFORM_RV1106_ALSA_SINGLE_TURN_IO_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "boompi/event/status.h"

namespace boompi::platform::rv1106 {

struct AlsaSingleTurnConfig final {
  std::string capture_device_name;
  std::string playback_device_name;
};

// Concrete, bounded ALSA I/O for the manual single-turn milestone. This is
// deliberately not a generic capture/playback interface and owns no threads.
// Both handles negotiate the board-proven 48 kHz, S16_LE, stereo,
// 960-frame-period/3840-frame-buffer contract exactly.
class AlsaSingleTurnIo final {
 public:
  static constexpr std::uint32_t kSampleRateHz = 48000U;
  static constexpr std::uint8_t kChannels = 2U;
  static constexpr std::uint32_t kPeriodFrames = 960U;
  static constexpr std::uint32_t kBufferFrames = 3840U;
  static constexpr std::size_t kCaptureInterleavedSamples =
      static_cast<std::size_t>(kPeriodFrames) * kChannels;
  using CapturePeriod =
      std::array<std::int16_t, kCaptureInterleavedSamples>;

  AlsaSingleTurnIo() noexcept = default;
  ~AlsaSingleTurnIo() noexcept;
  AlsaSingleTurnIo(const AlsaSingleTurnIo&) = delete;
  AlsaSingleTurnIo& operator=(const AlsaSingleTurnIo&) = delete;
  AlsaSingleTurnIo(AlsaSingleTurnIo&&) = delete;
  AlsaSingleTurnIo& operator=(AlsaSingleTurnIo&&) = delete;

  static Status Open(const AlsaSingleTurnConfig& config,
                     AlsaSingleTurnIo* output);

  Status Capture20Ms(CapturePeriod* output, std::uint32_t timeout_ms);
  // Stops this turn's capture stream before response playback begins. The
  // capture handle stays owned until Abort(), but no further reads are valid.
  Status FinishCapture();
  Status WriteMono48k(const std::int16_t* samples,
                      std::uint16_t sample_frames,
                      std::uint32_t timeout_ms);
  // Stops queued TTS immediately and prepares the same playback handle for
  // the next response. Capture is deliberately left running for barge-in.
  Status DropPlayback();
  Status DrainPlayback(std::uint32_t timeout_ms);

  // Drops and closes only the two handles owned by this object. It never
  // changes mixer controls or signals another process.
  void Abort() noexcept;
  bool is_open() const noexcept;

 private:
  void* capture_handle_{nullptr};
  void* playback_handle_{nullptr};
  bool capture_started_{false};
  bool capture_finished_{false};
  std::array<std::int16_t, kCaptureInterleavedSamples>
      playback_interleaved_scratch_{};
};

}  // namespace boompi::platform::rv1106

#endif  // BOOMPI_PLATFORM_RV1106_ALSA_SINGLE_TURN_IO_H_
