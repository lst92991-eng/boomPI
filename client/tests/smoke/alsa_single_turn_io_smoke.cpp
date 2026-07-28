#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "boompi/platform/rv1106/alsa_single_turn_io.h"

namespace {

int Fail(const char* const operation, const boompi::Status& status) {
  std::cerr << "boomPI ALSA single-turn null smoke failed: " << operation
            << ": " << status.message() << '\n';
  return EXIT_FAILURE;
}

}  // namespace

int main() {
  boompi::platform::rv1106::AlsaSingleTurnIo io;
  const auto open_status =
      boompi::platform::rv1106::AlsaSingleTurnIo::Open({"null", "null"}, &io);
  if (!open_status.ok()) {
    return Fail("open", open_status);
  }

  boompi::platform::rv1106::AlsaSingleTurnIo::CapturePeriod captured{};
  const auto capture_status = io.Capture20Ms(&captured, 500U);
  if (!capture_status.ok()) {
    return Fail("capture", capture_status);
  }
  const auto finish_capture_status = io.FinishCapture();
  if (!finish_capture_status.ok()) {
    return Fail("finish capture", finish_capture_status);
  }
  const auto capture_after_finish = io.Capture20Ms(&captured, 500U);
  if (capture_after_finish.ok() ||
      capture_after_finish.code() != boompi::StatusCode::kFailedPrecondition) {
    return Fail("capture after finish", capture_after_finish);
  }

  std::array<std::int16_t,
             boompi::platform::rv1106::AlsaSingleTurnIo::kPeriodFrames>
      mono{};
  for (std::size_t index = 0U; index < mono.size(); ++index) {
    mono[index] = static_cast<std::int16_t>(
        static_cast<std::int32_t>(index % 257U) - 128);
  }
  const auto write_status = io.WriteMono48k(
      mono.data(), static_cast<std::uint16_t>(mono.size()), 500U);
  if (!write_status.ok()) {
    return Fail("playback", write_status);
  }
  const auto drain_status = io.DrainPlayback(500U);
  if (!drain_status.ok()) {
    return Fail("drain", drain_status);
  }
  io.Abort();
  if (io.is_open()) {
    return EXIT_FAILURE;
  }

  std::cout << "boomPI ALSA single-turn null smoke passed; "
               "this proves bounded API flow only\n";
  return EXIT_SUCCESS;
}
