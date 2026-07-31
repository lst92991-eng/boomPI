#include "boompi/voice/audio_engine.h"

#include <alsa/asoundlib.h>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <webrtc_vad.h>
}

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <thread>

#include "../platform/rv1106/snowboy_legacy_bridge.h"
#include "boompi/platform/rv1106/rockchip_voice_dsp.h"

namespace boompi::voice {
namespace {

constexpr std::size_t kCapture48Frames = 960U, kVoice16Frames = 320U;
constexpr std::size_t kTts24Frames = 480U, kTtsSlots = 75U;
constexpr std::size_t kReferenceSlots = 16U, kReferenceLead = 3U;
constexpr std::uint32_t kFrameMs = 20U;

int OpenPcm(const std::string& name, snd_pcm_stream_t stream, snd_pcm_t** output, const char** stage) noexcept {
  constexpr int flags = SND_PCM_NO_AUTO_RESAMPLE | SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_FORMAT;
  snd_pcm_t* pcm = nullptr;
  int rc = snd_pcm_open(&pcm, name.c_str(), stream, flags);
  if (rc < 0) { *stage = "ALSA open"; return rc; }
  snd_pcm_hw_params_t* hw = nullptr;
  snd_pcm_hw_params_alloca(&hw);
  unsigned rate = 48000U;
  snd_pcm_uframes_t period = kCapture48Frames;
  snd_pcm_uframes_t buffer = 4U * kCapture48Frames;
  int direction = 0;
  rc = snd_pcm_hw_params_any(pcm, hw);
  if (rc >= 0) rc = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  if (rc >= 0) rc = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
  if (rc >= 0) rc = snd_pcm_hw_params_set_rate_resample(pcm, hw, 0U);
  if (rc >= 0) rc = snd_pcm_hw_params_set_channels(pcm, hw, 2U);
  if (rc >= 0) rc = snd_pcm_hw_params_set_rate(pcm, hw, rate, 0);
  if (rc >= 0) rc = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, &direction);
  if (rc >= 0) rc = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
  if (rc >= 0) rc = snd_pcm_hw_params(pcm, hw);
  if (rc < 0 || period != kCapture48Frames || buffer != 4U * kCapture48Frames) {
    *stage = "ALSA exact 48k/stereo/960/3840 setup"; snd_pcm_close(pcm);
    return rc < 0 ? rc : -EINVAL;
  }
  snd_pcm_sw_params_t* sw = nullptr;
  snd_pcm_sw_params_alloca(&sw);
  rc = snd_pcm_sw_params_current(pcm, sw);
  if (rc >= 0) rc = snd_pcm_sw_params_set_avail_min(pcm, sw, kCapture48Frames);
  const snd_pcm_uframes_t start = stream == SND_PCM_STREAM_CAPTURE ? 1U : 3U * kCapture48Frames;
  if (rc >= 0) rc = snd_pcm_sw_params_set_start_threshold(pcm, sw, start);
  if (rc >= 0) rc = snd_pcm_sw_params(pcm, sw);
  if (rc >= 0) rc = snd_pcm_prepare(pcm);
  if (rc < 0) { *stage = "ALSA software setup"; snd_pcm_close(pcm); return rc; }
  *output = pcm; return 0;
}

SwrContext* NewResampler(int input_rate, int input_channels, int output_rate, int output_channels) noexcept {
  SwrContext* swr = swr_alloc_set_opts(nullptr, av_get_default_channel_layout(output_channels), AV_SAMPLE_FMT_S16, output_rate, av_get_default_channel_layout(input_channels), AV_SAMPLE_FMT_S16,
                                       input_rate, 0, nullptr);
  if (swr == nullptr || swr_init(swr) < 0) { swr_free(&swr); return nullptr; }
  return swr;
}

double Energy(const std::array<std::int16_t, kVoice16Frames>& samples) noexcept {
  double sum = 0.0;
  for (const std::int16_t sample : samples) { const double value = sample; sum += value * value; }
  return sum / static_cast<double>(samples.size());
}

}  // namespace

struct AudioEngine::Impl final {
  using Voice = std::array<std::int16_t, kVoice16Frames>;
  struct TtsSlot { std::array<std::int16_t, kTts24Frames> pcm{}; std::size_t used{0U}; };

  AudioEngineConfig config{};
  snd_pcm_t *capture_pcm{nullptr}, *playback_pcm{nullptr};
  SwrContext *capture_swr{nullptr}, *playback_swr{nullptr}, *reference_swr{nullptr};
  platform::rv1106::RockchipVoiceDsp dsp{};
  BoompiSnowboyLegacyHandle* snowboy{nullptr};
  VadInst* vad{nullptr};
  std::thread playback_thread{};
  mutable std::mutex mutex{}, error_mutex{};
  std::condition_variable condition{};
  std::array<char, 192U> error{};
  std::array<TtsSlot, kTtsSlots> tts{};
  std::array<Voice, kReferenceSlots> references{};
  std::array<std::int16_t, kCapture48Frames * 2U> capture48{};
  std::array<std::int16_t, kVoice16Frames * 2U> capture16{};
  std::array<std::int16_t, kTts24Frames> playback24{};
  std::array<std::int16_t, kCapture48Frames * 2U> playback48{}, playback_stereo{};
  std::array<std::int16_t, kCapture48Frames> reference48{};
  Voice mic_left{}, mic_right{}, reference{}, processed{};
  std::size_t tts_head{0U}, tts_tail{0U}, tts_count{0U}, queued_samples{0U};
  std::size_t ref_head{0U}, ref_tail{0U}, ref_count{0U}, ref48_used{0U};
  std::uint64_t capture_sequence{0U}, next_tts_sequence{0U};
  std::uint32_t vad_speech_ms{0U}, vad_silence_ms{0U};
  std::uint32_t echo_tail{0U}, echo_calibration{0U}, echo_hold{0U};
  std::uint8_t echo_votes{0U};
  double previous_mic{0.0}, previous_ref{0.0}, ref_envelope{0.0};
  double mic_ratio{0.0}, output_ratio{0.0}, output_floor{1.0};
  bool open{false}, stop{false}, active{false}, ending{false};
  bool drop{false}, sequence_set{false}, ref_started{false};
  bool ref_done{false}, ref_broken{false}, vad_in_speech{false};
  bool previous_valid{false}, previous_ref_aligned{false};
  std::atomic<bool> duck{false}, is_playing{false}, is_done{true};

  void SetError(const char* text, int code = 0) noexcept {
    std::lock_guard<std::mutex> lock(error_mutex);
    if (code == 0) std::snprintf(error.data(), error.size(), "%s", text);
    else std::snprintf(error.data(), error.size(), "%s: %s", text, snd_strerror(code));
  }

  void ClearQueue() noexcept {
    for (auto& slot : tts) { slot.pcm.fill(0); slot.used = 0U; }
    tts_head = tts_tail = tts_count = queued_samples = 0U;
  }

  void ClearReferences(bool broken) noexcept {
    for (auto& slot : references) slot.fill(0);
    ref_head = ref_tail = ref_count = ref48_used = 0U;
    ref_started = ref_done = false;
    ref_broken = ref_broken || broken;
    reference48.fill(0);
  }

  bool ResetSwr(SwrContext* swr, const std::int16_t* input, int input_frames, int output_frames, std::int16_t* output) noexcept {
    swr_close(swr);
    if (swr_init(swr) < 0) return false;
    const std::uint8_t* in[] = {reinterpret_cast<const std::uint8_t*>(input)};
    std::uint8_t* out[] = {reinterpret_cast<std::uint8_t*>(output)};
    return swr_convert(swr, out, output_frames, in, input_frames) >= 0;
  }

  bool ResetListener() noexcept {
    int reset = 0;
    if (boompi_snowboy_legacy_reset(snowboy, &reset) != BOOMPI_SNOWBOY_LEGACY_OK || WebRtcVad_Init(vad) != 0 || WebRtcVad_set_mode(vad, config.vad_mode) != 0) return false;
    vad_speech_ms = vad_silence_ms = 0U; vad_in_speech = false;
    return true;
  }

  bool ResetFrontEnd() noexcept {
    capture48.fill(0);
    if (!ResetSwr(capture_swr, capture48.data(), kCapture48Frames, kVoice16Frames, capture16.data())) return false;
    dsp.Close();
    if (dsp.Open() != platform::rv1106::RockchipVoiceDspStatus::kOk || !ResetListener()) return false;
    echo_tail = echo_calibration = echo_hold = 0U; echo_votes = 0U;
    previous_mic = previous_ref = ref_envelope = mic_ratio = output_ratio = 0.0;
    output_floor = 1.0; previous_valid = previous_ref_aligned = false;
    return true;
  }

  bool ReadCapture(bool* discontinuity) noexcept {
    std::size_t offset = 0U;
    while (offset < kCapture48Frames) {
      const snd_pcm_sframes_t rc = snd_pcm_readi(capture_pcm, capture48.data() + 2U * offset, kCapture48Frames - offset);
      if (rc > 0) { offset += static_cast<std::size_t>(rc); continue; }
      if (rc == -EINTR) continue;
      if (rc == -EPIPE || rc == -ESTRPIPE) {
        const int recovered = snd_pcm_recover(capture_pcm, static_cast<int>(rc), 1);
        if (recovered < 0) { SetError("ALSA capture recovery", recovered); return false; }
        offset = 0U; *discontinuity = true; continue;
      }
      SetError("ALSA capture read", static_cast<int>(rc)); return false;
    }
    return true;
  }

  bool WritePlayback(const std::int16_t* mono, std::size_t frames, bool* xrun) noexcept {
    for (std::size_t i = 0U; i < frames; ++i) { playback_stereo[2U * i] = mono[i]; playback_stereo[2U * i + 1U] = mono[i]; }
    std::size_t offset = 0U;
    while (offset < frames) {
      const snd_pcm_sframes_t rc = snd_pcm_writei(playback_pcm, playback_stereo.data() + 2U * offset, frames - offset);
      if (rc > 0) { offset += static_cast<std::size_t>(rc); continue; }
      if (rc == -EINTR) continue;
      if (rc == -EPIPE || rc == -ESTRPIPE) {
        const int recovered = snd_pcm_recover(playback_pcm, static_cast<int>(rc), 1);
        if (recovered < 0) { SetError("ALSA playback recovery", recovered); return false; }
        offset = 0U; *xrun = true; continue;
      }
      SetError("ALSA playback write", static_cast<int>(rc)); return false;
    }
    return true;
  }

  void PushReference(const Voice& value) noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    if (ref_count == kReferenceSlots) { ClearReferences(true); return; }
    references[ref_tail] = value; ref_tail = (ref_tail + 1U) % kReferenceSlots; ++ref_count;
  }

  void PublishReference(const std::int16_t* samples, std::size_t frames) noexcept {
    while (frames != 0U) {
      const std::size_t copied = std::min(frames, kCapture48Frames - ref48_used);
      std::copy_n(samples, copied, reference48.begin() + ref48_used);
      samples += copied; frames -= copied; ref48_used += copied;
      if (ref48_used != kCapture48Frames) continue;
      Voice output{};
      const std::uint8_t* in[] = {reinterpret_cast<const std::uint8_t*>(reference48.data())};
      std::uint8_t* out[] = {reinterpret_cast<std::uint8_t*>(output.data())};
      const int converted = swr_convert(reference_swr, out, kVoice16Frames, in, kCapture48Frames);
      ref48_used = 0U; reference48.fill(0);
      if (converted == static_cast<int>(kVoice16Frames)) PushReference(output);
      else { std::lock_guard<std::mutex> lock(mutex); ClearReferences(true); }
    }
  }

  bool Render(const TtsSlot& slot) noexcept {
    playback24.fill(0);
    std::copy_n(slot.pcm.begin(), slot.used, playback24.begin());
    const std::uint8_t* in[] = {reinterpret_cast<const std::uint8_t*>(playback24.data())};
    std::uint8_t* out[] = {reinterpret_cast<std::uint8_t*>(playback48.data())};
    const int converted = swr_convert(playback_swr, out, playback48.size(), in, kTts24Frames);
    if (converted <= 0 || converted > static_cast<int>(playback48.size())) { SetError("TTS resampling failed"); return false; }
    const float gain = config.playback_gain * (duck.load(std::memory_order_relaxed) ? config.duck_gain : 1.0F);
    for (int i = 0; i < converted; ++i) {
      const long value = std::lround(static_cast<float>(playback48[i]) * gain);
      playback48[i] = static_cast<std::int16_t>(std::clamp<long>(value, std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()));
    }
    bool xrun = false;
    std::size_t offset = 0U;
    while (offset < static_cast<std::size_t>(converted)) {
      const std::size_t count = std::min(kCapture48Frames, static_cast<std::size_t>(converted) - offset);
      if (!WritePlayback(playback48.data() + offset, count, &xrun)) return false;
      if (xrun) { std::lock_guard<std::mutex> lock(mutex); ClearReferences(true); }
      PublishReference(playback48.data() + offset, count); offset += count;
    }
    return true;
  }

  void FinishPlayback(bool discard, bool failed) noexcept {
    if (!discard && !failed) { std::lock_guard<std::mutex> lock(mutex); ref_done = true; }
    if (!discard && !failed) {
      const int rc = snd_pcm_drain(playback_pcm);
      if (rc < 0) { SetError("ALSA playback drain", rc); failed = true; }
    } else {
      snd_pcm_drop(playback_pcm);
    }
    snd_pcm_prepare(playback_pcm);
    std::lock_guard<std::mutex> lock(mutex);
    if (discard || failed) ClearReferences(failed);
    ref_done = true; active = ending = drop = false;
    ClearQueue();
    is_playing.store(false, std::memory_order_release);
    is_done.store(true, std::memory_order_release);
    condition.notify_all();
  }

  void PlaybackLoop() noexcept {
    for (;;) {
      TtsSlot slot{};
      bool have_slot = false, finish = false, must_drop = false;
      {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [this] { return stop || drop || (active && ((tts_count != 0U && (tts[tts_head].used == kTts24Frames || ending)) || (ending && tts_count == 0U))); });
        if (stop) break;
        must_drop = drop;
        if (!must_drop && tts_count != 0U) {
          slot = tts[tts_head]; queued_samples -= slot.used; tts[tts_head] = {};
          tts_head = (tts_head + 1U) % kTtsSlots; --tts_count; have_slot = true;
        }
        finish = must_drop || (ending && tts_count == 0U);
      }
      bool failed = false;
      if (have_slot && !Render(slot)) failed = true;
      if (finish || failed) FinishPlayback(must_drop, failed);
    }
  }

  bool TakeReference(Voice* output, bool* aligned, bool* broken) noexcept {
    output->fill(0); *aligned = false;
    std::lock_guard<std::mutex> lock(mutex);
    *broken = ref_broken; ref_broken = false;
    if (!ref_started && (ref_count >= kReferenceLead || (ref_done && ref_count != 0U))) ref_started = true;
    if (!ref_started || ref_count == 0U) return true;
    *output = references[ref_head];
    references[ref_head].fill(0);
    ref_head = (ref_head + 1U) % kReferenceSlots; --ref_count; *aligned = true;
    return true;
  }

  bool UpdateVad(CaptureFrame* frame) noexcept {
    const int speech = WebRtcVad_Process(vad, 16000, processed.data(), processed.size());
    if (speech < 0) return false;
    frame->vad_now = speech == 1;
    if (frame->vad_now) {
      vad_speech_ms = std::min<std::uint32_t>(vad_speech_ms + kFrameMs, config.vad_start_ms); vad_silence_ms = 0U;
    } else {
      vad_silence_ms = std::min<std::uint32_t>(vad_silence_ms + kFrameMs, config.vad_end_ms); vad_speech_ms = 0U;
    }
    if (!vad_in_speech && vad_speech_ms >= config.vad_start_ms) {
      vad_in_speech = true; frame->vad_started = true;
    } else if (vad_in_speech && vad_silence_ms >= config.vad_end_ms) {
      vad_in_speech = false; frame->vad_ended = true;
    }
    return true;
  }

  void UpdateEcho(bool aligned, CaptureFrame* frame) noexcept {
    const double output_energy = Energy(processed);
    const bool reference_active = previous_ref_aligned && previous_ref >= 64.0;
    if (reference_active) {
      echo_tail = 25U; ref_envelope = std::max(previous_ref, ref_envelope * 0.72);
    } else if (echo_tail != 0U) { --echo_tail; ref_envelope *= 0.72; }
    frame->echo_context = reference_active || echo_tail != 0U;
    bool admitted = !frame->echo_context;
    if (previous_valid && frame->echo_context) {
      if (reference_active && echo_calibration < 30U) {
        const double weight = echo_calibration == 0U ? 1.0 : 0.10;
        mic_ratio += weight * (previous_mic / previous_ref - mic_ratio); output_ratio += weight * (output_energy / previous_ref - output_ratio);
        ++echo_calibration;
      } else if (echo_calibration >= 30U) {
        const double predicted_mic = std::max(1.0, mic_ratio * ref_envelope);
        const double predicted_out = std::max(output_floor, output_ratio * ref_envelope);
        const bool evidence =
            frame->vad_now && ((previous_mic > 3.0 * predicted_mic && output_energy > 3.0 * predicted_out) || (previous_mic > 1.5 * predicted_mic && output_energy > 6.0 * predicted_out));
        echo_votes = static_cast<std::uint8_t>(((echo_votes << 1U) | evidence) & 7U);
        const unsigned votes = (echo_votes & 1U) + ((echo_votes >> 1U) & 1U) + ((echo_votes >> 2U) & 1U);
        if (votes >= 2U) echo_hold = 8U;
        admitted = echo_hold != 0U; if (echo_hold != 0U) --echo_hold;
      }
    } else if (!frame->vad_now) {
      const double bounded = std::clamp(output_energy, output_floor * 0.5, output_floor * 2.0);
      output_floor += (bounded - output_floor) * (bounded < output_floor ? 0.2 : 0.005);
    }
    frame->near_voice = frame->vad_now && admitted;
    previous_mic = std::max(Energy(mic_left), Energy(mic_right));
    previous_ref = Energy(reference); previous_ref_aligned = aligned; previous_valid = true;
  }
};

AudioEngine::~AudioEngine() noexcept { Close(); delete impl_; }

bool AudioEngine::Open(const AudioEngineConfig& config) noexcept {
  if (impl_ == nullptr) impl_ = new (std::nothrow) Impl;
  if (impl_ == nullptr) return false;
  if (impl_->open) { impl_->SetError("audio engine is already open"); return false; }
  if (config.capture_pcm.empty() || config.playback_pcm.empty() || config.snowboy_resource.empty() || config.snowboy_model.empty() || (config.left_polarity != 1 && config.left_polarity != -1) ||
      (config.right_polarity != 1 && config.right_polarity != -1) || config.vad_mode < 0 || config.vad_mode > 3 || config.vad_start_ms == 0U || config.vad_end_ms == 0U ||
      config.playback_gain < 0.0F || config.duck_gain < 0.0F || config.duck_gain > 1.0F) {
    impl_->SetError("invalid audio engine configuration"); return false;
  }
  try { impl_->config = config; }
  catch (...) { impl_->SetError("audio configuration allocation failed"); return false; }
  const char* stage = nullptr;
  int rc = OpenPcm(config.capture_pcm, SND_PCM_STREAM_CAPTURE, &impl_->capture_pcm, &stage);
  if (rc >= 0) rc = OpenPcm(config.playback_pcm, SND_PCM_STREAM_PLAYBACK, &impl_->playback_pcm, &stage);
  if (rc < 0) { impl_->SetError(stage, rc); Close(); return false; }
  impl_->capture_swr = NewResampler(48000, 2, 16000, 2);
  impl_->playback_swr = NewResampler(24000, 1, 48000, 1);
  impl_->reference_swr = NewResampler(48000, 1, 16000, 1);
  BoompiSnowboyLegacyInfo info{};
  if (impl_->capture_swr == nullptr || impl_->playback_swr == nullptr || impl_->reference_swr == nullptr ||
      boompi_snowboy_legacy_create(config.snowboy_resource.c_str(), config.snowboy_model.c_str(), config.snowboy_sensitivity.c_str(), config.snowboy_gain, 0, &impl_->snowboy, &info) !=
          BOOMPI_SNOWBOY_LEGACY_OK ||
      info.sample_rate_hz != 16000U || info.channels != 1U || info.bits_per_sample != 16U || info.keyword_count == 0U) {
    impl_->SetError("audio DSP, resampler, or Snowboy initialization failed"); Close(); return false;
  }
  impl_->vad = WebRtcVad_Create();
  if (impl_->vad == nullptr || WebRtcVad_Init(impl_->vad) != 0 || WebRtcVad_set_mode(impl_->vad, config.vad_mode) != 0 || WebRtcVad_ValidRateAndFrameLength(16000, kVoice16Frames) != 0 ||
      !impl_->ResetFrontEnd()) {
    impl_->SetError("WebRTC VAD or voice front end initialization failed"); Close(); return false;
  }
  impl_->stop = false;
  impl_->open = true;
  try { impl_->playback_thread = std::thread(&Impl::PlaybackLoop, impl_); }
  catch (...) { impl_->SetError("playback thread creation failed"); Close(); return false; }
  return true;
}

bool AudioEngine::Capture(CaptureFrame* frame) noexcept {
  if (impl_ == nullptr || !impl_->open || frame == nullptr) return false;
  *frame = {};
  frame->sequence = impl_->capture_sequence++;
  bool discontinuity = false, aligned = false, reference_broken = false;
  if (!impl_->ReadCapture(&discontinuity) || !impl_->TakeReference(&impl_->reference, &aligned, &reference_broken)) return false;
  discontinuity = discontinuity || reference_broken;
  if (discontinuity) {
    frame->discontinuity = true;
    if (!impl_->ResetFrontEnd()) { impl_->SetError("voice front end reset failed"); return false; }
    return true;
  }
  for (std::size_t i = 0U; i < kCapture48Frames; ++i) {
    impl_->capture48[2U * i] = static_cast<std::int16_t>(impl_->capture48[2U * i] * impl_->config.left_polarity);
    impl_->capture48[2U * i + 1U] = static_cast<std::int16_t>(impl_->capture48[2U * i + 1U] * impl_->config.right_polarity);
  }
  const std::uint8_t* in[] = {reinterpret_cast<const std::uint8_t*>(impl_->capture48.data())};
  std::uint8_t* out[] = {reinterpret_cast<std::uint8_t*>(impl_->capture16.data())};
  const int converted = swr_convert(impl_->capture_swr, out, kVoice16Frames, in, kCapture48Frames);
  if (converted != static_cast<int>(kVoice16Frames)) { impl_->SetError("capture resampler lost frame alignment"); return false; }
  for (std::size_t i = 0U; i < kVoice16Frames; ++i) {
    impl_->mic_left[i] = impl_->capture16[2U * i]; impl_->mic_right[i] = impl_->capture16[2U * i + 1U];
  }
  if (impl_->dsp.Process(impl_->mic_left, impl_->mic_right, impl_->reference, &impl_->processed) != platform::rv1106::RockchipVoiceDspStatus::kOk) {
    impl_->SetError("Rockchip 3A rejected a frame"); return false;
  }
  int32_t detection = 0;
  if (boompi_snowboy_legacy_process_s16(impl_->snowboy, impl_->processed.data(), impl_->processed.size(), &detection) != BOOMPI_SNOWBOY_LEGACY_OK) {
    impl_->SetError("Snowboy processing failed"); return false;
  }
  frame->wake = detection > 0;
  if (!impl_->UpdateVad(frame)) { impl_->SetError("WebRTC VAD processing failed"); return false; }
  impl_->UpdateEcho(aligned, frame);
  frame->pcm = impl_->processed;
  return true;
}

bool AudioEngine::ResetListener() noexcept {
  if (impl_ == nullptr || !impl_->open || !impl_->ResetListener()) {
    if (impl_ != nullptr) impl_->SetError("listener reset failed");
    return false;
  }
  return true;
}

bool AudioEngine::QueueTts24k(const std::uint8_t* bytes, std::size_t byte_count, std::uint64_t sequence) noexcept {
  if (impl_ == nullptr || !impl_->open || bytes == nullptr || byte_count == 0U || (byte_count & 1U) != 0U) return false;
  const std::size_t samples = byte_count / 2U;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->active || impl_->ending || samples > kTtsSlots * kTts24Frames - impl_->queued_samples || (impl_->sequence_set && sequence != impl_->next_tts_sequence)) {
    impl_->SetError("TTS queue is full, closed, or discontinuous"); return false;
  }
  std::size_t source = 0U;
  while (source < samples) {
    if (impl_->tts_count == 0U || impl_->tts[(impl_->tts_tail + kTtsSlots - 1U) % kTtsSlots].used == kTts24Frames) {
      impl_->tts_tail = (impl_->tts_tail + 1U) % kTtsSlots; ++impl_->tts_count;
    }
    auto& slot = impl_->tts[(impl_->tts_tail + kTtsSlots - 1U) % kTtsSlots];
    const std::size_t copied = std::min(samples - source, kTts24Frames - slot.used);
    for (std::size_t i = 0U; i < copied; ++i) {
      const std::size_t offset = 2U * (source + i);
      const std::uint16_t value = static_cast<std::uint16_t>(bytes[offset]) | (static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
      slot.pcm[slot.used + i] = static_cast<std::int16_t>(value);
    }
    slot.used += copied; source += copied;
  }
  impl_->queued_samples += samples; impl_->sequence_set = true;
  impl_->next_tts_sequence = sequence + 1U; impl_->condition.notify_one(); return true;
}

bool AudioEngine::BeginPlayback() noexcept {
  if (impl_ == nullptr || !impl_->open) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->active) return false;
  impl_->ClearQueue(); impl_->ClearReferences(false); impl_->ref_broken = false;
  impl_->playback24.fill(0); impl_->reference48.fill(0);
  if (!impl_->ResetSwr(impl_->playback_swr, impl_->playback24.data(), kTts24Frames, impl_->playback48.size(), impl_->playback48.data()) ||
      !impl_->ResetSwr(impl_->reference_swr, impl_->reference48.data(), kCapture48Frames, kVoice16Frames, impl_->reference.data())) {
    impl_->SetError("playback resampler reset failed"); return false;
  }
  impl_->sequence_set = impl_->ending = impl_->drop = false; impl_->active = true;
  impl_->is_playing.store(true, std::memory_order_release); impl_->is_done.store(false, std::memory_order_release);
  return true;
}

bool AudioEngine::EndPlayback() noexcept {
  if (impl_ == nullptr || !impl_->open) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->active || impl_->ending) return false;
  impl_->ending = true; impl_->condition.notify_one(); return true;
}

void AudioEngine::Duck(bool enabled) noexcept { if (impl_ != nullptr) impl_->duck.store(enabled, std::memory_order_release); }

void AudioEngine::DropPlayback() noexcept {
  if (impl_ == nullptr || !impl_->open) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->active) { impl_->drop = true; impl_->ClearQueue(); impl_->condition.notify_one(); }
}

bool AudioEngine::playing() const noexcept { return impl_ != nullptr && impl_->is_playing.load(std::memory_order_acquire); }

bool AudioEngine::playback_done() const noexcept { return impl_ == nullptr || impl_->is_done.load(std::memory_order_acquire); }

std::string AudioEngine::last_error() const {
  if (impl_ == nullptr) return "audio engine is not allocated";
  std::lock_guard<std::mutex> lock(impl_->error_mutex);
  return impl_->error.data();
}

void AudioEngine::Close() noexcept {
  if (impl_ == nullptr) return;
  if (impl_->open) {
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      impl_->stop = true;
    }
    impl_->condition.notify_all();
    if (impl_->playback_pcm != nullptr) snd_pcm_drop(impl_->playback_pcm);
    if (impl_->playback_thread.joinable()) impl_->playback_thread.join();
  }
  if (impl_->capture_pcm != nullptr) snd_pcm_close(impl_->capture_pcm);
  if (impl_->playback_pcm != nullptr) snd_pcm_close(impl_->playback_pcm);
  impl_->capture_pcm = impl_->playback_pcm = nullptr;
  swr_free(&impl_->capture_swr); swr_free(&impl_->playback_swr); swr_free(&impl_->reference_swr);
  impl_->dsp.Close();
  if (impl_->snowboy != nullptr) boompi_snowboy_legacy_destroy(impl_->snowboy);
  if (impl_->vad != nullptr) WebRtcVad_Free(impl_->vad);
  impl_->snowboy = nullptr; impl_->vad = nullptr;
  impl_->open = impl_->stop = impl_->active = impl_->ending = impl_->drop = false;
  impl_->sequence_set = impl_->ref_broken = false; impl_->capture_sequence = 0U; impl_->ClearQueue(); impl_->ClearReferences(false);
  impl_->is_playing.store(false, std::memory_order_release);
  impl_->is_done.store(true, std::memory_order_release);
}

}  // namespace boompi::voice
