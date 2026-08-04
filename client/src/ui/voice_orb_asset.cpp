#include "voice_orb_asset.h"

#include <cstdint>

namespace boompi::ui::assets {
namespace {

alignas(4) const std::uint8_t kVoiceOrbPixels[] = {
#include "voice_orb_rgb565.inc"
};

const lv_img_dsc_t kVoiceOrb = [] {
  lv_img_dsc_t image{};
  image.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
  image.header.w = 112;
  image.header.h = 112;
  image.data_size = sizeof(kVoiceOrbPixels);
  image.data = kVoiceOrbPixels;
  return image;
}();

static_assert(sizeof(kVoiceOrbPixels) == 112U * 112U * 3U);

}  // namespace

const lv_img_dsc_t* VoiceOrb() noexcept { return &kVoiceOrb; }

}  // namespace boompi::ui::assets
