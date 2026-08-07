#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "boompi/application/voice_client.h"
#include "support/voice_client_harness.h"

namespace {

using boompi::audio::CaptureFrame;
using boompi::network::ControlKind;
using boompi::network::Inbound;
using boompi::network::InboundKind;
using boompi::network::SendOutcome;
using boompi::tests::CaptureStep;
using boompi::tests::HarnessPlan;
using boompi::tests::HarnessSnapshot;
using boompi::ui::DeviceUiAction;
using boompi::ui::DeviceUiState;

constexpr char kExplicitSpki[] =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";

void Check(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

CaptureStep Quiet() { return {}; }

CaptureStep DelayedQuiet(const unsigned delay_ms) {
  CaptureStep step{};
  step.delay_ms = delay_ms;
  return step;
}

CaptureStep Timeout(const bool stop_after = false) {
  CaptureStep step{};
  step.timeout = true;
  step.stop_after = stop_after;
  return step;
}

CaptureStep Wake() {
  CaptureStep step{};
  step.frame.wake = true;
  step.frame.input_dbfs = -20.0F;
  return step;
}

CaptureStep SpeechStart() {
  CaptureStep step{};
  step.frame.vad_now = true;
  step.frame.vad_started = true;
  step.frame.near_voice = true;
  step.frame.input_dbfs = -20.0F;
  step.frame.voice_dbfs = -20.0F;
  return step;
}

CaptureStep SpeechEnd() {
  CaptureStep step{};
  step.frame.vad_ended = true;
  step.frame.input_dbfs = -50.0F;
  return step;
}

CaptureStep NearVoice(const bool reference_active) {
  CaptureStep step{};
  step.frame.vad_now = true;
  step.frame.near_voice = true;
  step.frame.reference_active = reference_active;
  step.frame.input_dbfs = -18.0F;
  step.frame.voice_dbfs = -18.0F;
  return step;
}

Inbound Event(const InboundKind kind) {
  Inbound event{};
  event.kind = kind;
  event.ids = {101U, 1U, 1U, 1U};
  if (kind == InboundKind::kResponseStart ||
      kind == InboundKind::kTextDelta ||
      kind == InboundKind::kAudioStart || kind == InboundKind::kDone)
    event.response_id = "response-1";
  return event;
}

Inbound TextDelta(const char* const text) {
  Inbound event = Event(InboundKind::kTextDelta);
  event.text = text;
  return event;
}

CaptureStep StartResponse() {
  CaptureStep step{};
  step.inbound.push_back(Event(InboundKind::kResponseStart));
  step.inbound.push_back(Event(InboundKind::kAudioStart));
  Inbound pcm = Event(InboundKind::kPcm);
  pcm.pcm.ids = pcm.ids;
  pcm.pcm.flags = 3U;  // 单帧测试音频：START + END。
  pcm.audio_size = 2U;
  step.inbound.push_back(std::move(pcm));
  return step;
}

std::size_t CountControl(const HarnessSnapshot& snapshot,
                         const ControlKind kind) {
  return static_cast<std::size_t>(std::count_if(
      snapshot.controls.begin(), snapshot.controls.end(),
      [kind](const auto& control) { return control.kind == kind; }));
}

const boompi::network::Control& NthControl(const HarnessSnapshot& snapshot,
                                           const ControlKind kind,
                                           const std::size_t wanted) {
  std::size_t seen = 0U;
  for (const auto& control : snapshot.controls) {
    if (control.kind != kind) continue;
    if (seen++ == wanted) return control;
  }
  throw std::runtime_error("missing expected control");
}

bool SameIds(const boompi::network::WireIds& left,
             const boompi::network::WireIds& right) {
  return left.session == right.session && left.turn == right.turn &&
         left.stream == right.stream && left.epoch == right.epoch;
}

void CheckCompletePcmTurn(const HarnessSnapshot& snapshot,
                          const boompi::network::WireIds& ids) {
  std::size_t frame_count = 0U;
  std::uint32_t expected_sequence = 0U;
  const boompi::tests::PcmSend* first = nullptr;
  const boompi::tests::PcmSend* last = nullptr;
  for (const auto& send : snapshot.pcm) {
    if (!SameIds(send.header.ids, ids)) continue;
    if (first == nullptr) first = &send;
    last = &send;
    Check(send.header.sequence == expected_sequence++,
          "barge turn PCM sequence is not continuous");
    ++frame_count;
  }
  Check(frame_count != 0U, "barge turn sent no PCM");
  Check((first->header.flags & 1U) != 0U,
        "barge turn first PCM is missing START");
  Check((last->header.flags & 2U) != 0U,
        "barge turn last PCM is missing END");
}

std::size_t CountState(const HarnessSnapshot& snapshot,
                       const DeviceUiState state) {
  return static_cast<std::size_t>(
      std::count(snapshot.ui_states.begin(), snapshot.ui_states.end(), state));
}

bool ContainsScale(const HarnessSnapshot& snapshot, const float value) {
  return std::find(snapshot.playback_scales.begin(),
                   snapshot.playback_scales.end(), value) !=
         snapshot.playback_scales.end();
}

bool ContainsVisibleText(const HarnessSnapshot& snapshot) {
  return std::any_of(snapshot.ui_text.begin(), snapshot.ui_text.end(),
                     [](const auto& text) {
                       return !text.first.empty() || !text.second.empty();
                     });
}

bool EndsWithEmptyText(const HarnessSnapshot& snapshot) {
  return !snapshot.ui_text.empty() && snapshot.ui_text.back().first.empty() &&
         snapshot.ui_text.back().second.empty();
}

bool ContainsText(const HarnessSnapshot& snapshot, const char* const expected) {
  return std::any_of(snapshot.ui_text.begin(), snapshot.ui_text.end(),
                     [expected](const auto& text) {
                       return text.first == expected || text.second == expected;
                     });
}

HarnessSnapshot Run(HarnessPlan plan) {
  volatile std::sig_atomic_t stop = 0;
  boompi::tests::LoadHarness(std::move(plan), &stop);

  boompi::config::VoiceClientConfig config{};
  config.server_ip = "127.0.0.1";
  config.server_port = 17806U;
  config.server_spki_sha256 = kExplicitSpki;
  config.device_id = "00000000-0000-4000-8000-000000000001";
  config.capture_pcm = "test-capture";
  config.playback_pcm = "test-playback";
  config.snowboy_resource_path = "test-common.res";
  config.snowboy_model_path = "test-model.pmdl";

  std::string error;
  const bool succeeded =
      boompi::application::RunVoiceClient(config, &stop, &error);
  if (!succeeded)
    throw std::runtime_error(error.empty() ? "voice client returned false" : error);
  return boompi::tests::ReadHarness();
}

void ExplicitEndpointConnectsWithoutSavedServer() {
  HarnessPlan plan{};
  plan.fallback_server_available = false;
  CaptureStep stop = Quiet();
  stop.stop_after = true;
  plan.capture.push_back(stop);

  const auto result = Run(std::move(plan));
  Check(result.network_bootstraps == 1U,
        "network link/bootstrap was not attempted");
  Check(result.explicit_endpoint_seen,
        "explicit BOOMPI_SERVER endpoint was not passed to bootstrap");
  Check(result.connections == 1U,
        "explicit endpoint did not enter Connect without saved server");
  Check(result.connected_host == "127.0.0.1" &&
            result.connected_port == 17806U &&
            result.connected_spki == kExplicitSpki,
        "Connect did not receive the explicit endpoint unchanged");
}

void HelloDeadlineAdvancesWithoutCaptureFrames() {
  HarnessPlan plan{};
  plan.acknowledge_hello = false;
  plan.stop_after_transport_close = true;
  plan.capture.push_back(Quiet());
  for (unsigned tick = 0U; tick != 10U; ++tick)
    plan.capture.push_back(Timeout());

  const auto result = Run(std::move(plan));
  Check(CountControl(result, ControlKind::kHello) >= 1U,
        "hello timeout test never sent hello");
  Check(result.capture_timeouts != 0U,
        "hello deadline was not exercised without capture frames");
  Check(result.transport_closes >= 1U,
        "missing hello ACK did not close the timed-out transport");
  Check(CountState(result, DeviceUiState::kOffline) >= 2U,
        "missing hello ACK was not surfaced as offline");
}

void HelloDeadlineConsumesDequeuedCaptureFrame() {
  HarnessPlan plan{};
  plan.acknowledge_hello = false;
  plan.capture = {Quiet(), DelayedQuiet(40U)};

  const auto result = Run(std::move(plan));
  Check(CountControl(result, ControlKind::kHello) >= 2U,
        "hello frame-fence test did not reconnect after timeout");
  Check(result.transport_closes >= 1U,
        "hello frame-fence timeout did not close the old transport");
  Check(result.listener_resets == 1U,
        "hello frame-fence fabricated a sequence gap and extra listener reset");
}

void HeartbeatDeadlineAdvancesWithoutCaptureFrames() {
  HarnessPlan plan{};
  plan.stop_after_transport_close = true;
  plan.capture.push_back(Quiet());
  for (unsigned tick = 0U; tick != 200U; ++tick)
    plan.capture.push_back(Timeout());

  const auto result = Run(std::move(plan));
  Check(CountControl(result, ControlKind::kHello) >= 1U,
        "heartbeat timeout test never established a session");
  Check(result.capture_timeouts != 0U,
        "heartbeat deadline was not exercised without capture frames");
  Check(result.transport_closes >= 1U,
        "missing heartbeat did not close the timed-out transport");
  Check(CountState(result, DeviceUiState::kOffline) >= 2U,
        "missing heartbeat was not surfaced as offline");
}

void HeartbeatDeadlineConsumesDequeuedCaptureFrame() {
  HarnessPlan plan{};
  plan.capture = {Quiet(), DelayedQuiet(3100U)};

  const auto result = Run(std::move(plan));
  Check(CountControl(result, ControlKind::kHello) >= 2U,
        "heartbeat frame-fence test did not reconnect after timeout");
  Check(result.transport_closes >= 1U,
        "heartbeat frame-fence timeout did not close the old transport");
  Check(result.listener_resets == 3U,
        "heartbeat frame-fence fabricated a sequence gap and extra listener reset");
}

void CaptureWallDeadlineCancelsWithoutReconnect() {
  HarnessPlan plan{};
  plan.stop_after_cancel_control = true;
  plan.capture = {Wake(), SpeechStart()};
  // Stop on the observed cancel instead of guessing which compressed 20 ms
  // poll will cross the wall-clock deadline on a shared CI runner.
  for (unsigned tick = 0U; tick != 40U; ++tick)
    plan.capture.push_back(Timeout());

  const auto result = Run(std::move(plan));
  Check(result.capture_timeouts != 0U,
        "capture wall deadline was not exercised without capture frames");
  Check(CountControl(result, ControlKind::kTurnCancel) == 1U,
        "capture wall deadline did not cancel the incomplete turn");
  Check(CountControl(result, ControlKind::kTurnCommit) == 0U,
        "capture wall deadline fabricated a turn commit");
  Check(result.connections == 1U,
        "capture wall deadline unnecessarily rebuilt WSS");
  Check(CountState(result, DeviceUiState::kOffline) == 1U,
        "capture wall deadline incorrectly marked healthy WSS offline");
}

void CancelDeadlineAdvancesWithoutCaptureFrames() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd(), StartResponse()};
  CaptureStep touch = Quiet();
  touch.actions.push_back(DeviceUiAction::kInterrupt);
  plan.capture.push_back(touch);
  plan.capture.push_back(Timeout());
  plan.capture.push_back(Timeout());
  plan.capture.push_back(Timeout(true));

  const auto result = Run(std::move(plan));
  Check(CountControl(result, ControlKind::kResponseCancel) == 1U,
        "cancel timeout test did not send exactly one response.cancel");
  Check(result.capture_timeouts == 3U,
        "cancel ACK deadline was not exercised without capture frames");
  Check(result.transport_closes >= 1U,
        "missing cancel ACK did not retire the ambiguous connection");
}

void CancelDeadlineConsumesDequeuedCaptureFrame() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd(), StartResponse()};
  CaptureStep touch = Quiet();
  touch.actions.push_back(DeviceUiAction::kInterrupt);
  plan.capture.push_back(touch);
  plan.capture.push_back(DelayedQuiet(260U));

  const auto result = Run(std::move(plan));
  Check(CountControl(result, ControlKind::kResponseCancel) == 1U,
        "cancel frame-fence sent a duplicate or missing response.cancel");
  Check(result.transport_closes >= 1U,
        "cancel frame-fence timeout did not retire the ambiguous transport");
  Check(result.listener_resets == 4U,
        "cancel frame-fence fabricated a sequence gap and extra listener reset");
}

void LateCancelAckCannotCrossDeadline() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd(), StartResponse()};
  CaptureStep touch = Quiet();
  touch.actions.push_back(DeviceUiAction::kInterrupt);
  plan.capture.push_back(touch);
  CaptureStep late_ack = DelayedQuiet(260U);
  late_ack.inbound.push_back(Event(InboundKind::kCancelled));
  late_ack.stop_after = true;
  plan.capture.push_back(std::move(late_ack));

  const auto result = Run(std::move(plan));
  Check(CountControl(result, ControlKind::kResponseCancel) == 1U,
        "late ACK test did not send exactly one response.cancel");
  Check(result.transport_closes >= 1U,
        "ACK dequeued after its deadline revived the old transport");
  Check(CountState(result, DeviceUiState::kHappy) == 0U,
        "ACK dequeued after its deadline completed the cancelled turn");
}

void FollowUpDeadlineAdvancesWithoutCaptureFrames() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd(), StartResponse()};
  CaptureStep done = Quiet();
  done.inbound.push_back(Event(InboundKind::kDone));
  plan.capture.push_back(done);
  CaptureStep playback_finished = Timeout();
  playback_finished.playback_done = true;
  plan.capture.push_back(playback_finished);
  // FinishTurn starts the 30 ms follow-up deadline after the preceding poll.
  // Two further 20 ms polls cross it deterministically.
  plan.capture.push_back(Timeout());
  plan.capture.push_back(Timeout(true));

  const auto result = Run(std::move(plan));
  Check(result.capture_timeouts == 3U,
        "follow-up deadline was not exercised without capture frames");
  Check(CountState(result, DeviceUiState::kHappy) == 1U,
        "completed playback did not enter follow-up");
  Check(CountState(result, DeviceUiState::kIdle) >= 2U,
        "expired follow-up did not return to wake-word idle state");
  Check(result.connections == 1U,
        "follow-up expiry unnecessarily rebuilt WSS");
}

void FollowUpDeadlineConsumesDequeuedCaptureFrame() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd(), StartResponse()};
  CaptureStep done = Quiet();
  done.inbound.push_back(Event(InboundKind::kDone));
  plan.capture.push_back(done);
  CaptureStep playback_finished = Timeout();
  playback_finished.playback_done = true;
  plan.capture.push_back(playback_finished);
  plan.capture.push_back(DelayedQuiet(40U));
  plan.capture.push_back(Wake());
  plan.capture.push_back(SpeechStart());

  const auto result = Run(std::move(plan));
  Check(CountControl(result, ControlKind::kTurnStart) == 2U,
        "follow-up frame-fence swallowed the next wake or speech start");
  Check(CountControl(result, ControlKind::kTurnCancel) == 0U &&
            CountControl(result, ControlKind::kResponseCancel) == 0U,
        "follow-up frame-fence fabricated a cancellation");
  Check(result.connections == 1U,
        "follow-up frame-fence unnecessarily rebuilt WSS");
}

void CaptureSequenceHoleCancelsOnlyTurn() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart()};
  CaptureStep gap = Quiet();
  gap.sequence_gap_before = true;
  gap.stop_after = true;
  plan.capture.push_back(gap);

  const auto result = Run(std::move(plan));
  Check(CountControl(result, ControlKind::kTurnStart) == 1U,
        "turn did not start before sequence hole");
  Check(CountControl(result, ControlKind::kTurnCancel) == 1U,
        "sequence hole did not cancel current turn");
  Check(result.connections == 1U,
        "local capture hole unnecessarily rebuilt WSS");
  Check(CountState(result, DeviceUiState::kOffline) == 1U,
        "local capture hole incorrectly marked WSS offline");
}

void TouchWakeIgnoresStaleVadEdge() {
  CaptureStep stale = SpeechStart();
  stale.actions.push_back(DeviceUiAction::kWake);

  HarnessPlan stale_only{};
  stale.stop_after = true;
  stale_only.capture.push_back(stale);
  const auto isolated = Run(std::move(stale_only));
  Check(CountControl(isolated, ControlKind::kTurnStart) == 0U,
        "touch wake consumed a stale VAD edge from the same capture frame");
  Check(CountState(isolated, DeviceUiState::kListening) == 1U,
        "touch wake did not enter listening state");

  HarnessPlan fresh_vad{};
  stale.stop_after = false;
  fresh_vad.capture.push_back(stale);
  CaptureStep fresh = SpeechStart();
  fresh.stop_after = true;
  fresh_vad.capture.push_back(fresh);
  const auto admitted = Run(std::move(fresh_vad));
  Check(CountControl(admitted, ControlKind::kTurnStart) == 1U,
        "fresh VAD edge after touch wake did not start a turn");
}

void DrainRemainsTouchInterruptible() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd(), StartResponse()};
  CaptureStep done = Quiet();
  done.inbound.push_back(Event(InboundKind::kDone));
  done.actions.push_back(DeviceUiAction::kInterrupt);
  plan.capture.push_back(done);
  CaptureStep acknowledged = Quiet();
  acknowledged.inbound.push_back(Event(InboundKind::kCancelled));
  acknowledged.stop_after = true;
  plan.capture.push_back(acknowledged);

  const auto result = Run(std::move(plan));
  Check(result.playback_begins == 1U && result.playback_ends == 1U,
        "response.done did not enter playback drain");
  Check(CountState(result, DeviceUiState::kSpeakingTail) == 1U,
        "drain was not exposed as an interruptible UI state");
  Check(result.playback_drops >= 1U,
        "touch did not drop pending playback tail");
  Check(CountControl(result, ControlKind::kResponseCancel) == 1U,
        "touch did not discard the unheard completed response");
  Check(CountState(result, DeviceUiState::kHappy) == 1U,
        "touch cancellation did not wait for ACK before finishing");
}

void DrainRemainsVoiceInterruptible() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd(), StartResponse()};
  CaptureStep done = Quiet();
  done.inbound.push_back(Event(InboundKind::kDone));
  plan.capture.push_back(done);
  for (unsigned i = 0U; i != 6U; ++i)
    plan.capture.push_back(NearVoice(true));
  for (unsigned i = 0U; i != 6U; ++i)
    plan.capture.push_back(NearVoice(false));
  for (unsigned i = 0U; i != 3U; ++i) {
    auto step = NearVoice(false);
    plan.capture.push_back(step);
  }
  CaptureStep acknowledged = Quiet();
  acknowledged.inbound.push_back(Event(InboundKind::kCancelled));
  acknowledged.stop_after = true;
  plan.capture.push_back(acknowledged);

  const auto result = Run(std::move(plan));
  Check(CountControl(result, ControlKind::kResponseCancel) == 1U,
        "voice barge did not discard the unheard completed response");
  Check(result.playback_drops >= 1U,
        "voice barge did not drop the playback tail");
}

void DrainTimeoutCancelsWithoutReconnect() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd(), StartResponse()};
  CaptureStep done = Quiet();
  done.inbound.push_back(Event(InboundKind::kDone));
  plan.capture.push_back(done);
  CaptureStep timeout = Quiet();
  timeout.timeout = true;
  plan.capture.push_back(timeout);
  plan.capture.push_back(timeout);
  CaptureStep acknowledged = Quiet();
  acknowledged.inbound.push_back(Event(InboundKind::kCancelled));
  acknowledged.stop_after = true;
  plan.capture.push_back(acknowledged);

  const auto result = Run(std::move(plan));
  Check(CountControl(result, ControlKind::kResponseCancel) == 1U,
        "drain timeout did not discard the unheard response");
  Check(result.connections == 1U,
        "drain timeout unnecessarily rebuilt WSS");
  Check(result.capture_timeouts == 2U,
        "drain deadline was not exercised across capture timeouts");
  Check(CountState(result, DeviceUiState::kHappy) == 1U,
        "drain timeout did not finish after cancellation ACK");
}

void DrainCompletionAdvancesWithoutCaptureFrames() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd(), StartResponse()};
  CaptureStep done = Quiet();
  done.inbound.push_back(Event(InboundKind::kDone));
  plan.capture.push_back(done);
  CaptureStep playback_finished = Quiet();
  playback_finished.timeout = true;
  playback_finished.playback_done = true;
  playback_finished.stop_after = true;
  plan.capture.push_back(playback_finished);

  const auto result = Run(std::move(plan));
  Check(result.capture_timeouts == 1U,
        "playback completion was not exercised on a capture timeout tick");
  Check(CountControl(result, ControlKind::kResponseCancel) == 0U,
        "completed playback was incorrectly cancelled without capture frames");
  Check(CountState(result, DeviceUiState::kHappy) == 1U,
        "completed playback did not enter follow-up without capture frames");
}

void DrainCompletionConsumesDequeuedCaptureFrame() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd(), StartResponse()};
  CaptureStep done = Quiet();
  done.inbound.push_back(Event(InboundKind::kDone));
  plan.capture.push_back(done);
  CaptureStep playback_finished = Quiet();
  playback_finished.playback_done = true;
  plan.capture.push_back(playback_finished);
  CaptureStep next = Quiet();
  next.stop_after = true;
  plan.capture.push_back(next);

  const auto result = Run(std::move(plan));
  Check(CountState(result, DeviceUiState::kHappy) == 1U,
        "completed playback did not enter follow-up on a capture frame");
  Check(result.playback_drops == 0U,
        "discarded drain-transition frame fabricated a sequence gap");
  Check(CountControl(result, ControlKind::kResponseCancel) == 0U,
        "fabricated sequence gap cancelled a completed response");
}

void DrainDiscontinuityCancelsWithoutReconnect() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd(), StartResponse()};
  CaptureStep done = Quiet();
  done.inbound.push_back(Event(InboundKind::kDone));
  plan.capture.push_back(done);
  CaptureStep gap = Quiet();
  gap.sequence_gap_before = true;
  plan.capture.push_back(gap);
  CaptureStep acknowledged = Quiet();
  acknowledged.inbound.push_back(Event(InboundKind::kCancelled));
  acknowledged.stop_after = true;
  plan.capture.push_back(acknowledged);

  const auto result = Run(std::move(plan));
  Check(CountControl(result, ControlKind::kResponseCancel) == 1U,
        "drain discontinuity did not cancel the completed response");
  Check(result.connections == 1U,
        "drain discontinuity unnecessarily rebuilt WSS");
  Check(CountState(result, DeviceUiState::kHappy) == 1U,
        "drain discontinuity did not finish after cancellation ACK");
}

void CancelPendingDiscontinuityWaitsForAck() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd(), StartResponse()};
  CaptureStep touch = Quiet();
  touch.actions.push_back(DeviceUiAction::kInterrupt);
  plan.capture.push_back(touch);
  CaptureStep gap = Quiet();
  gap.sequence_gap_before = true;
  plan.capture.push_back(gap);
  CaptureStep acknowledged = Quiet();
  acknowledged.inbound.push_back(Event(InboundKind::kCancelled));
  acknowledged.stop_after = true;
  plan.capture.push_back(acknowledged);

  const auto result = Run(std::move(plan));
  Check(CountControl(result, ControlKind::kResponseCancel) == 1U,
        "cancel-pending discontinuity sent a duplicate or missing cancel");
  Check(result.connections == 1U,
        "cancel-pending discontinuity rebuilt healthy WSS");
  Check(CountState(result, DeviceUiState::kHappy) == 1U,
        "cancel-pending discontinuity did not wait for its ACK");
}

void BargeTurnDiscontinuityKeepsConnection() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd(), StartResponse()};
  for (unsigned i = 0U; i != 6U; ++i)
    plan.capture.push_back(NearVoice(true));
  for (unsigned i = 0U; i != 9U; ++i)
    plan.capture.push_back(NearVoice(false));
  CaptureStep acknowledged = Quiet();
  acknowledged.inbound.push_back(Event(InboundKind::kCancelled));
  plan.capture.push_back(acknowledged);
  CaptureStep gap = Quiet();
  gap.sequence_gap_before = true;
  plan.capture.push_back(gap);
  CaptureStep turn_cancelled = Quiet();
  auto replacement_ack = Event(InboundKind::kCancelled);
  replacement_ack.ids = {101U, 2U, 2U, 2U};
  turn_cancelled.inbound.push_back(std::move(replacement_ack));
  turn_cancelled.stop_after = true;
  plan.capture.push_back(std::move(turn_cancelled));

  const auto result = Run(std::move(plan));
  Check(CountControl(result, ControlKind::kResponseCancel) == 1U,
        "barge probe did not reach cancellation");
  Check(CountControl(result, ControlKind::kTurnCancel) == 1U,
        "replacement turn discontinuity was not cancelled exactly once");
  Check(result.connections == 1U,
        "barge turn discontinuity rebuilt healthy WSS");
  Check(CountState(result, DeviceUiState::kHappy) == 1U,
        "barge turn discontinuity did not finish after its cancellation ACK");
}

void ResponseStartDoesNotHideFirstResponseHint() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd()};
  CaptureStep waiting = Quiet();
  waiting.timeout = true;
  plan.capture.push_back(waiting);
  CaptureStep started = Quiet();
  started.inbound.push_back(Event(InboundKind::kResponseStart));
  started.stop_after = true;
  plan.capture.push_back(started);

  const auto result = Run(std::move(plan));
  Check(ContainsText(result, "还在思考"),
        "15-second progress hint was not displayed");
  Check(result.capture_timeouts == 1U,
        "first-response hint was not exercised on a capture timeout tick");
  Check(!result.ui_text.empty() && result.ui_text.back().first == "还在思考",
        "response.start hid the progress hint before content arrived");
}

void FirstContentReplacesResponseHint() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd()};
  CaptureStep waiting = Quiet();
  waiting.timeout = true;
  plan.capture.push_back(waiting);
  CaptureStep started = Quiet();
  started.inbound.push_back(Event(InboundKind::kResponseStart));
  plan.capture.push_back(started);
  CaptureStep content = Quiet();
  content.inbound.push_back(TextDelta("answer arrived"));
  content.stop_after = true;
  plan.capture.push_back(content);

  const auto result = Run(std::move(plan));
  Check(ContainsText(result, "answer arrived"),
        "first text did not replace the progress hint");
  Check(!result.ui_text.empty() && result.ui_text.back().first == "answer arrived",
        "first text left the progress hint visible");
}

void ResponseProgressCannotExtendTheAbsoluteDeadline() {
  // Validated deltas may extend the 300 ms no-progress window. Five 80 ms
  // intervals cross the original deadline while leaving scheduler margin.
  HarnessPlan progress{};
  progress.capture = {Wake(), SpeechStart(), SpeechEnd()};
  CaptureStep started = Quiet();
  started.inbound.push_back(Event(InboundKind::kResponseStart));
  progress.capture.push_back(std::move(started));
  for (unsigned index = 0U; index != 5U; ++index) {
    CaptureStep delta = DelayedQuiet(80U);
    delta.inbound.push_back(TextDelta(index == 0U ? "progress-1" : "progress"));
    delta.stop_after = index == 4U;
    progress.capture.push_back(std::move(delta));
  }
  const auto progressing = Run(std::move(progress));
  Check(CountControl(progressing, ControlKind::kResponseCancel) == 0U,
        "validated response progress did not extend the inactivity window");
  Check(progressing.connections == 1U,
        "validated response progress rebuilt WSS");

  // Continuous valid progress still cannot move the compressed harness hard
  // cap. The harness stops on the observed cancel, independent of scheduler
  // jitter between individual delayed frames.
  HarnessPlan absolute{};
  absolute.stop_after_cancel_control = true;
  absolute.capture = {Wake(), SpeechStart(), SpeechEnd()};
  CaptureStep absolute_start = Quiet();
  absolute_start.inbound.push_back(Event(InboundKind::kResponseStart));
  absolute.capture.push_back(std::move(absolute_start));
  for (unsigned index = 0U; index != 20U; ++index) {
    CaptureStep delta = DelayedQuiet(100U);
    delta.inbound.push_back(TextDelta("still-streaming"));
    absolute.capture.push_back(std::move(delta));
  }
  const auto expired = Run(std::move(absolute));
  Check(CountControl(expired, ControlKind::kResponseCancel) == 1U,
        "streaming response crossed its absolute deadline");
  Check(CountControl(expired, ControlKind::kTurnCancel) == 0U,
        "absolute response deadline cancelled the wrong identity");
  Check(expired.connections == 1U,
        "absolute response deadline unnecessarily rebuilt WSS");
}

void StopDuringBlockedCaptureIsBounded() {
  HarnessPlan plan{};
  plan.stop_during_capture_timeout = true;
  const auto started = std::chrono::steady_clock::now();
  const auto result = Run(std::move(plan));
  const auto elapsed = std::chrono::steady_clock::now() - started;
  Check(result.capture_timeouts == 1U,
        "harness did not exercise the capture timeout path");
  Check(result.capture_poll_wait_ms == 20U,
        "Transport dispatch poll was not bounded to one 20 ms audio period");
  Check(elapsed < std::chrono::milliseconds(500),
        "stop flag could not escape a blocked capture wait");
}

void BackpressureDoesNotReconnect() {
  HarnessPlan plan{};
  plan.pcm_outcomes = {SendOutcome::kBackpressure};
  plan.capture = {Wake()};
  CaptureStep start = SpeechStart();
  start.stop_after = true;
  plan.capture.push_back(start);

  const auto result = Run(std::move(plan));
  Check(CountControl(result, ControlKind::kTurnCancel) == 1U,
        "backpressure did not cancel only the active turn");
  Check(result.connections == 1U,
        "backpressure rebuilt the persistent connection");
  Check(CountState(result, DeviceUiState::kOffline) == 1U,
        "backpressure incorrectly marked the server offline");
}

void RejectedSendReconnects() {
  HarnessPlan plan{};
  plan.pcm_outcomes = {SendOutcome::kRejected};
  plan.capture = {Wake(), SpeechStart()};
  CaptureStep after_reconnect = Quiet();
  after_reconnect.stop_after = true;
  plan.capture.push_back(after_reconnect);

  const auto result = Run(std::move(plan));
  Check(result.connections == 2U,
        "rejected transport send did not establish a new connection");
  Check(CountControl(result, ControlKind::kHello) == 2U,
        "reconnected transport did not repeat hello");
  Check(CountState(result, DeviceUiState::kOffline) >= 2U,
        "real transport rejection was not surfaced offline");
}

void ConnectionLossClearsSubtitleImmediately() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd(), StartResponse()};
  CaptureStep text = Quiet();
  text.inbound.push_back(TextDelta("partial answer"));
  plan.capture.push_back(text);
  CaptureStep failure = Quiet();
  Inbound error = Event(InboundKind::kError);
  error.error_code = "transport";
  error.text = "connection lost";
  failure.inbound.push_back(std::move(error));
  failure.stop_after = true;
  plan.capture.push_back(std::move(failure));

  const auto result = Run(std::move(plan));
  Check(ContainsVisibleText(result), "test never displayed a partial subtitle");
  Check(EndsWithEmptyText(result),
        "connection loss left a partial subtitle on screen");
}

void AbnormalTurnFinishClearsSubtitle() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd(), StartResponse()};
  CaptureStep text = Quiet();
  text.inbound.push_back(TextDelta("unfinished provider text"));
  plan.capture.push_back(text);
  CaptureStep failure = Quiet();
  Inbound error = Event(InboundKind::kError);
  error.error_code = "provider_error";
  error.text = "provider stopped";
  failure.inbound.push_back(std::move(error));
  failure.stop_after = true;
  plan.capture.push_back(std::move(failure));

  const auto result = Run(std::move(plan));
  Check(ContainsVisibleText(result), "test never displayed a partial subtitle");
  Check(EndsWithEmptyText(result),
        "abnormal turn finish left a partial subtitle on screen");
}

void SpeakToBargeUsesBoundedProbe() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd(), StartResponse()};
  for (unsigned i = 0U; i != 6U; ++i)
    plan.capture.push_back(NearVoice(true));
  for (unsigned i = 0U; i != 3U; ++i)
    plan.capture.push_back(NearVoice(false));
  for (unsigned i = 0U; i != 3U; ++i)
    plan.capture.push_back(NearVoice(false));
  for (unsigned i = 0U; i != 3U; ++i) {
    auto step = NearVoice(false);
    step.stop_after = i == 2U;
    plan.capture.push_back(step);
  }

  const auto result = Run(std::move(plan));
  Check(CountControl(result, ControlKind::kResponseCancel) == 1U,
        "confirmed near voice did not cancel the response");
  Check(ContainsScale(result, 0.0F) && ContainsScale(result, 1.0F),
        "barge probe did not mute and restore playback");
  Check(result.playback_drops >= 1U,
        "confirmed barge did not drop playback");
  Check(result.listener_resets == 2U,
        "barge verification reset and destroyed the active VAD decision");
}

HarnessPlan BargeUtterancePlan() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd(), StartResponse()};
  for (unsigned i = 0U; i != 6U; ++i) {
    auto step = NearVoice(true);
    // The barge utterance has exactly one VAD start edge.  The same edge must
    // survive response.cancel instead of requiring the user to speak again.
    step.frame.vad_started = i == 0U;
    plan.capture.push_back(step);
  }
  for (unsigned i = 0U; i != 3U; ++i)
    plan.capture.push_back(NearVoice(false));
  for (unsigned i = 0U; i != 3U; ++i)
    plan.capture.push_back(NearVoice(false));
  for (unsigned i = 0U; i != 3U; ++i)
    plan.capture.push_back(NearVoice(false));
  return plan;
}

HarnessPlan MaximumBargeProbePlan() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd(), StartResponse()};
  for (unsigned i = 0U; i != 6U; ++i) {
    auto step = NearVoice(true);
    step.frame.vad_started = i == 0U;
    plan.capture.push_back(step);
  }
  // 最慢合法路径在 reference wait 的最后 3 帧才观察到连续低参考。
  for (unsigned i = 0U; i != 12U; ++i)
    plan.capture.push_back(NearVoice(true));
  for (unsigned i = 0U; i != 3U; ++i)
    plan.capture.push_back(NearVoice(false));
  for (unsigned i = 0U; i != 3U; ++i)
    plan.capture.push_back(NearVoice(false));
  for (unsigned i = 0U; i != 3U; ++i)
    plan.capture.push_back(NearVoice(false));
  return plan;
}

void CheckBargeCommittedAsNewTurn(const HarnessSnapshot& result) {
  Check(CountControl(result, ControlKind::kResponseCancel) == 1U,
        "barge utterance did not cancel exactly one response");
  Check(CountControl(result, ControlKind::kTurnStart) == 2U,
        "barge utterance did not start exactly one replacement turn");
  Check(CountControl(result, ControlKind::kTurnCommit) == 2U,
        "barge utterance did not commit the replacement turn");

  const auto& first_start = NthControl(result, ControlKind::kTurnStart, 0U);
  const auto& second_start = NthControl(result, ControlKind::kTurnStart, 1U);
  Check(second_start.ids.session == first_start.ids.session &&
            second_start.ids.turn == first_start.ids.turn + 1U &&
            second_start.ids.stream == first_start.ids.stream + 1U &&
            second_start.ids.epoch == first_start.ids.epoch + 1U,
        "barge replacement did not advance turn/stream/epoch");
  const auto& second_commit = NthControl(result, ControlKind::kTurnCommit, 1U);
  Check(SameIds(second_commit.ids, second_start.ids),
        "barge commit used different wire IDs from turn.start");
  CheckCompletePcmTurn(result, second_start.ids);
}

void BargeShortUtteranceEndsBeforeAck() {
  // First prove that ending the utterance alone cannot start a new turn: the
  // protocol fence is released only by response.cancelled.
  auto before_ack = BargeUtterancePlan();
  before_ack.capture.push_back(SpeechEnd());
  before_ack.capture.push_back(Timeout(true));
  const auto waiting = Run(std::move(before_ack));
  Check(CountControl(waiting, ControlKind::kTurnStart) == 1U,
        "short barge turn started before response.cancelled");

  auto plan = BargeUtterancePlan();
  plan.capture.push_back(SpeechEnd());
  CaptureStep ack = Quiet();
  ack.inbound.push_back(Event(InboundKind::kCancelled));
  ack.stop_after = true;
  plan.capture.push_back(std::move(ack));

  CheckBargeCommittedAsNewTurn(Run(std::move(plan)));

  // A local capture hole can empty the admitted barge buffer while the old
  // response cancellation is still pending. The ACK retires that response,
  // but the empty replacement input must not rebuild a healthy WSS session.
  auto empty = BargeUtterancePlan();
  CaptureStep gap = Quiet();
  gap.sequence_gap_before = true;
  empty.capture.push_back(std::move(gap));
  CaptureStep empty_ack = Quiet();
  empty_ack.inbound.push_back(Event(InboundKind::kCancelled));
  empty_ack.stop_after = true;
  empty.capture.push_back(std::move(empty_ack));
  const auto discarded = Run(std::move(empty));
  Check(CountControl(discarded, ControlKind::kResponseCancel) == 1U,
        "empty barge did not finish the old response cancellation");
  Check(CountControl(discarded, ControlKind::kTurnStart) == 1U,
        "empty barge fabricated a replacement turn");
  Check(discarded.connections == 1U,
        "empty barge rebuilt healthy WSS");
}

void BargeLongUtteranceCrossesAck() {
  auto plan = BargeUtterancePlan();
  plan.capture.push_back(NearVoice(false));
  CaptureStep ack = NearVoice(false);
  ack.inbound.push_back(Event(InboundKind::kCancelled));
  plan.capture.push_back(std::move(ack));
  plan.capture.push_back(NearVoice(false));
  plan.capture.push_back(NearVoice(false));
  plan.capture.push_back(SpeechEnd());

  CheckBargeCommittedAsNewTurn(Run(std::move(plan)));
}

void BargeBufferCoversDelayedAck() {
  auto plan = MaximumBargeProbePlan();
  // 150 帧覆盖 production 的 3 s ACK 窗；再覆盖确认 cancel 时 capture actor
  // 可能已经持有的完整 4 槽队列。ACK 帧本身在事件封口后进入新 turn。
  for (unsigned i = 0U; i != 154U; ++i)
    plan.capture.push_back(NearVoice(false));
  CaptureStep ack = SpeechEnd();
  ack.inbound.push_back(Event(InboundKind::kCancelled));
  ack.stop_after = true;
  plan.capture.push_back(std::move(ack));

  const auto result = Run(std::move(plan));
  Check(result.connections == 1U,
        "delayed cancel ACK unnecessarily rebuilt WSS");
  CheckBargeCommittedAsNewTurn(result);
}

void CancelDoneReorderingIsIdempotent() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd(), StartResponse()};
  CaptureStep touch = Quiet();
  touch.actions.push_back(DeviceUiAction::kInterrupt);
  plan.capture.push_back(touch);
  CaptureStep done = Quiet();
  done.inbound.push_back(Event(InboundKind::kDone));
  plan.capture.push_back(done);
  CaptureStep stale_cancel = Quiet();
  stale_cancel.inbound.push_back(Event(InboundKind::kCancelled));
  stale_cancel.stop_after = true;
  plan.capture.push_back(stale_cancel);

  const auto result = Run(std::move(plan));
  Check(CountControl(result, ControlKind::kResponseCancel) == 1U,
        "touch interrupt did not send one response.cancel");
  Check(CountControl(result, ControlKind::kTurnStart) == 1U,
        "done/cancel reordering started a duplicate turn");
  Check(result.connections == 1U,
        "done/cancel reordering rebuilt WSS");
}

void StaleEpochCannotMutateNextTurn() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd(), StartResponse()};
  CaptureStep done = Quiet();
  done.inbound.push_back(Event(InboundKind::kDone));
  plan.capture.push_back(std::move(done));
  CaptureStep drained = Quiet();
  drained.playback_done = true;
  plan.capture.push_back(std::move(drained));
  CaptureStep stale = Quiet();
  stale.inbound.push_back(TextDelta("late stale text"));
  stale.stop_after = true;
  plan.capture.push_back(std::move(stale));

  const auto result = Run(std::move(plan));
  Check(result.connections == 1U, "stale epoch rebuilt healthy WSS");
  Check(!ContainsText(result, "late stale text"),
        "stale epoch changed the next-turn subtitle");
  Check(CountControl(result, ControlKind::kTurnStart) == 1U,
        "stale epoch started a duplicate turn");
}

void SessionErrorSurvivesTurnEpochAdvance() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd(), StartResponse()};
  CaptureStep done = Quiet();
  done.inbound.push_back(Event(InboundKind::kDone));
  plan.capture.push_back(std::move(done));
  CaptureStep drained = Quiet();
  drained.playback_done = true;
  plan.capture.push_back(std::move(drained));
  CaptureStep failed = Quiet();
  Inbound session_error = Event(InboundKind::kError);
  session_error.ids = {101U, 0U, 0U, 1U};
  session_error.error_code = "session_error";
  session_error.text = "session closed";
  failed.inbound.push_back(std::move(session_error));
  failed.stop_after = true;
  plan.capture.push_back(std::move(failed));

  const auto result = Run(std::move(plan));
  Check(result.transport_closes >= 1U,
        "session error was mistaken for a stale turn event");
}

void DownlinkSequenceGapRejectsConnection() {
  HarnessPlan plan{};
  plan.capture = {Wake(), SpeechStart(), SpeechEnd()};
  CaptureStep response = Quiet();
  response.inbound.push_back(Event(InboundKind::kResponseStart));
  response.inbound.push_back(Event(InboundKind::kAudioStart));
  Inbound gap = Event(InboundKind::kPcm);
  gap.pcm.ids = gap.ids;
  gap.pcm.sequence = 1U;
  gap.audio_size = 2U;
  response.inbound.push_back(std::move(gap));
  response.stop_after = true;
  plan.capture.push_back(std::move(response));

  const auto result = Run(std::move(plan));
  Check(result.transport_closes >= 1U,
        "downlink sequence gap did not retire ambiguous WSS");
}

void MaximumInputCommitsInsteadOfCancelling() {
  HarnessPlan plan{};
  plan.capture.reserve(3010U);
  plan.capture = {Wake(), SpeechStart()};
  for (unsigned index = 0U; index != 3005U; ++index) {
    CaptureStep step = Quiet();
    step.stop_after = index == 3004U;
    plan.capture.push_back(step);
  }

  const auto result = Run(std::move(plan));
  Check(result.pcm.size() == 3000U,
        "maximum input did not contain exactly 60 seconds of PCM");
  Check((result.pcm.back().header.flags & 2U) != 0U,
        "maximum input final PCM did not carry END");
  Check(CountControl(result, ControlKind::kTurnCommit) == 1U,
        "maximum input was not committed");
  Check(CountControl(result, ControlKind::kTurnCancel) == 0U,
        "maximum input was incorrectly cancelled");
}

}  // namespace

int main(const int argc, char** const argv) {
  if (argc != 2) {
    std::cerr << "expected one scenario name\n";
    return EXIT_FAILURE;
  }
  const std::string scenario = argv[1];
  try {
    if (scenario == "explicit-endpoint") ExplicitEndpointConnectsWithoutSavedServer();
    else if (scenario == "capture-hole") CaptureSequenceHoleCancelsOnlyTurn();
    else if (scenario == "touch-wake-stale-vad") TouchWakeIgnoresStaleVadEdge();
    else if (scenario == "hello-timeout")
      HelloDeadlineAdvancesWithoutCaptureFrames();
    else if (scenario == "hello-frame-timeout")
      HelloDeadlineConsumesDequeuedCaptureFrame();
    else if (scenario == "heartbeat-timeout")
      HeartbeatDeadlineAdvancesWithoutCaptureFrames();
    else if (scenario == "heartbeat-frame-timeout")
      HeartbeatDeadlineConsumesDequeuedCaptureFrame();
    else if (scenario == "capture-wall-timeout")
      CaptureWallDeadlineCancelsWithoutReconnect();
    else if (scenario == "cancel-timeout")
      CancelDeadlineAdvancesWithoutCaptureFrames();
    else if (scenario == "cancel-frame-timeout")
      CancelDeadlineConsumesDequeuedCaptureFrame();
    else if (scenario == "cancel-late-ack")
      LateCancelAckCannotCrossDeadline();
    else if (scenario == "follow-up-timeout")
      FollowUpDeadlineAdvancesWithoutCaptureFrames();
    else if (scenario == "follow-up-frame-timeout")
      FollowUpDeadlineConsumesDequeuedCaptureFrame();
    else if (scenario == "drain-touch") DrainRemainsTouchInterruptible();
    else if (scenario == "drain-barge") DrainRemainsVoiceInterruptible();
    else if (scenario == "drain-timeout") DrainTimeoutCancelsWithoutReconnect();
    else if (scenario == "drain-complete-timeout")
      DrainCompletionAdvancesWithoutCaptureFrames();
    else if (scenario == "drain-complete-frame")
      DrainCompletionConsumesDequeuedCaptureFrame();
    else if (scenario == "drain-discontinuity")
      DrainDiscontinuityCancelsWithoutReconnect();
    else if (scenario == "cancel-discontinuity")
      CancelPendingDiscontinuityWaitsForAck();
    else if (scenario == "barge-turn-discontinuity")
      BargeTurnDiscontinuityKeepsConnection();
    else if (scenario == "first-response-hint")
      ResponseStartDoesNotHideFirstResponseHint();
    else if (scenario == "first-response-content") {
      FirstContentReplacesResponseHint();
      ResponseProgressCannotExtendTheAbsoluteDeadline();
    }
    else if (scenario == "stop-stalled-capture")
      StopDuringBlockedCaptureIsBounded();
    else if (scenario == "backpressure") BackpressureDoesNotReconnect();
    else if (scenario == "reconnect") RejectedSendReconnects();
    else if (scenario == "connection-subtitle")
      ConnectionLossClearsSubtitleImmediately();
    else if (scenario == "abnormal-subtitle")
      AbnormalTurnFinishClearsSubtitle();
    else if (scenario == "barge") SpeakToBargeUsesBoundedProbe();
    else if (scenario == "barge-short-before-ack")
      BargeShortUtteranceEndsBeforeAck();
    else if (scenario == "barge-long-across-ack")
      BargeLongUtteranceCrossesAck();
    else if (scenario == "barge-delayed-ack")
      BargeBufferCoversDelayedAck();
    else if (scenario == "cancel-done") CancelDoneReorderingIsIdempotent();
    else if (scenario == "stale-epoch") StaleEpochCannotMutateNextTurn();
    else if (scenario == "session-error-after-turn")
      SessionErrorSurvivesTurnEpochAdvance();
    else if (scenario == "downlink-sequence-gap")
      DownlinkSequenceGapRejectsConnection();
    else if (scenario == "max-input") MaximumInputCommitsInsteadOfCancelling();
    else throw std::runtime_error("unknown scenario");
  } catch (const std::exception& error) {
    std::cerr << scenario << ": " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << scenario << ": passed\n";
  return EXIT_SUCCESS;
}
