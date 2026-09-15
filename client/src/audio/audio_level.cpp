#include <cmath>

#include "audio_convert.h"
namespace boompi::audio_convert {
float ac_rms_dbfs(const audio::VoiceFrame16k& samples) noexcept {
  double sum = 0.0;
  for (const std::int16_t sample : samples) {
    sum += sample;
  }
  const double mean = sum / static_cast<double>(samples.size());
  double energy = 0.0;
  for (const std::int16_t sample : samples) {
    const double centered = static_cast<double>(sample) - mean;
    energy += centered * centered;
  }
  if (energy <= 0.0) {
    return -120.0F;
  }
  const double rms = std::sqrt(energy / static_cast<double>(samples.size()));
  return static_cast<float>(20.0 * std::log10(rms / 32768.0));
}

}  // namespace boompi::audio_convert
