#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "boompi/application/runtime.h"
#include "boompi/audio/audio_format.h"
#include "boompi/config/client_config.h"
#include "boompi/event/event.h"
#include "boompi/network/reconnect_backoff.h"
#include "boompi/protocol/audio_packet.h"
#include "boompi/protocol/envelope.h"
#include "boompi/test/manual_clock.h"
#include "boompi/test/test_context.h"
#include "boompi/ui/ui_model.h"
#include "boompi/update/app_slot.h"

#define BOOMPI_EXPECT(context, expression) \
  (context).Expect((expression), #expression, __FILE__, __LINE__)

namespace {

void TestConfigurationAndAudio(boompi::test::TestContext& context) {
  auto config = boompi::config::DefaultClientConfig();
  BOOMPI_EXPECT(context, boompi::config::ValidateClientConfig(config).ok());

  const boompi::audio::AudioFormat capture_format{
      config.audio.capture_sample_rate_hz, config.audio.frame_duration_ms,
      config.audio.capture_channels, boompi::audio::SampleFormat::kPcmS16Le};
  BOOMPI_EXPECT(context,
                boompi::audio::ValidateAudioFormat(capture_format).ok());
  BOOMPI_EXPECT(context,
                boompi::audio::SamplesPerChannel(capture_format) == 960U);
  BOOMPI_EXPECT(context,
                boompi::audio::BytesPerFrame(capture_format) == 7680U);

  config.audio.default_volume_percent = 101U;
  BOOMPI_EXPECT(context,
                !boompi::config::ValidateClientConfig(config).ok());

  config = boompi::config::DefaultClientConfig();
  config.audio.capture_sample_rate_hz = 1U;
  BOOMPI_EXPECT(context,
                !boompi::config::ValidateClientConfig(config).ok());

  config = boompi::config::DefaultClientConfig();
  config.audio.dsp_sample_rate_hz = 48000U;
  BOOMPI_EXPECT(context,
                !boompi::config::ValidateClientConfig(config).ok());

  config = boompi::config::DefaultClientConfig();
  config.audio.playback_sample_rate_hz = 16000U;
  BOOMPI_EXPECT(context,
                !boompi::config::ValidateClientConfig(config).ok());

  config = boompi::config::DefaultClientConfig();
  config.audio.frame_duration_ms = 1000U;
  BOOMPI_EXPECT(context,
                !boompi::config::ValidateClientConfig(config).ok());

  config = boompi::config::DefaultClientConfig();
  config.audio.capture_channels = 2U;
  BOOMPI_EXPECT(context, boompi::config::ValidateClientConfig(config).ok());
  config.audio.capture_channels = 1U;
  BOOMPI_EXPECT(context,
                !boompi::config::ValidateClientConfig(config).ok());
  config.audio.capture_channels = 3U;
  BOOMPI_EXPECT(context,
                !boompi::config::ValidateClientConfig(config).ok());
}

void TestBoundedEventBus(boompi::test::TestContext& context) {
  boompi::event::EventBus event_bus(1U);
  const boompi::event::Event event{boompi::event::EventType::kRuntimeStarted,
                                   100U, 1U, 1U, 0U};
  BOOMPI_EXPECT(context, event_bus.Publish(event).ok());
  BOOMPI_EXPECT(context, !event_bus.Publish(event).ok());

  boompi::event::Event popped{};
  BOOMPI_EXPECT(context, event_bus.TryPop(&popped));
  BOOMPI_EXPECT(context, popped.sequence == 1U);
  BOOMPI_EXPECT(context, !event_bus.TryPop(&popped));
}

void TestRuntimeLifecycle(boompi::test::TestContext& context) {
  boompi::test::ManualClock clock(123U);
  boompi::application::Runtime runtime(
      boompi::config::DefaultClientConfig(), clock);

  BOOMPI_EXPECT(context, runtime.Start().ok());
  BOOMPI_EXPECT(context,
                runtime.state() == boompi::application::RuntimeState::kIdle);
  BOOMPI_EXPECT(context, !runtime.Start().ok());

  boompi::event::Event event{};
  BOOMPI_EXPECT(context, runtime.TryPopEvent(&event));
  BOOMPI_EXPECT(context,
                event.type == boompi::event::EventType::kRuntimeStarted);
  BOOMPI_EXPECT(context, event.monotonic_timestamp_ms == 123U);

  clock.SetNowMs(456U);
  BOOMPI_EXPECT(context, runtime.Stop().ok());
  BOOMPI_EXPECT(context,
                runtime.state() == boompi::application::RuntimeState::kStopped);
  BOOMPI_EXPECT(context, runtime.TryPopEvent(&event));
  BOOMPI_EXPECT(context,
                event.type == boompi::event::EventType::kRuntimeStopping);
  BOOMPI_EXPECT(context, event.monotonic_timestamp_ms == 456U);
}

void TestRuntimeStopRetryAfterFullQueue(
    boompi::test::TestContext& context) {
  auto config = boompi::config::DefaultClientConfig();
  config.event_queue_capacity = 1U;
  boompi::test::ManualClock clock(100U);
  boompi::application::Runtime runtime(config, clock);

  BOOMPI_EXPECT(context, runtime.Start().ok());
  BOOMPI_EXPECT(context, !runtime.Stop().ok());
  BOOMPI_EXPECT(context,
                runtime.state() == boompi::application::RuntimeState::kError);

  boompi::event::Event event{};
  BOOMPI_EXPECT(context, runtime.TryPopEvent(&event));
  BOOMPI_EXPECT(context,
                event.type == boompi::event::EventType::kRuntimeStarted);

  clock.SetNowMs(200U);
  BOOMPI_EXPECT(context, runtime.Stop().ok());
  BOOMPI_EXPECT(context,
                runtime.state() == boompi::application::RuntimeState::kStopped);
  BOOMPI_EXPECT(context, runtime.TryPopEvent(&event));
  BOOMPI_EXPECT(context,
                event.type == boompi::event::EventType::kRuntimeStopping);
  BOOMPI_EXPECT(context, event.monotonic_timestamp_ms == 200U);
}

boompi::protocol::AudioPacketHeader MakeAudioHeader() {
  boompi::protocol::AudioPacketHeader header{};
  header.kind = boompi::protocol::AudioPacketKind::kUplink;
  header.flags = 0x0001U;
  header.channels = 1U;
  header.sample_rate_hz = 16000U;
  header.payload_length_bytes = 8U;
  header.sequence = 42U;
  header.monotonic_timestamp_us = 1000000U;
  header.epoch = 7U;
  header.device_uuid = {0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U,
                        0x66U, 0x77U, 0x88U, 0x99U, 0xAAU, 0xBBU,
                        0xCCU, 0xDDU, 0xEEU, 0xFFU};
  header.session_id = 0x01020304U;
  header.turn_id = 0x0A0B0C0DU;
  header.stream_id = 0x11223344U;
  return header;
}

bool ExtractJsonString(const std::string& json, const std::string& field,
                       std::string* const output) {
  if (output == nullptr) {
    return false;
  }
  const auto marker = std::string{"\""} + field + "\"";
  const auto field_position = json.find(marker);
  if (field_position == std::string::npos) {
    return false;
  }
  const auto colon_position = json.find(':', field_position + marker.size());
  const auto quote_position =
      colon_position == std::string::npos ? std::string::npos
                                          : json.find('"', colon_position + 1U);
  const auto end_position =
      quote_position == std::string::npos ? std::string::npos
                                          : json.find('"', quote_position + 1U);
  if (quote_position == std::string::npos || end_position == std::string::npos) {
    return false;
  }
  *output = json.substr(quote_position + 1U,
                        end_position - quote_position - 1U);
  return true;
}

int HexNibble(const char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

bool DecodeHex(const std::string& hex, std::vector<std::uint8_t>* const output) {
  if (output == nullptr || hex.size() % 2U != 0U) {
    return false;
  }
  output->assign(hex.size() / 2U, 0U);
  for (std::size_t index = 0; index < output->size(); ++index) {
    const auto high = HexNibble(hex[index * 2U]);
    const auto low = HexNibble(hex[index * 2U + 1U]);
    if (high < 0 || low < 0) {
      return false;
    }
    (*output)[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

void TestProtocolHeader(boompi::test::TestContext& context,
                        const std::string& fixture_path) {
  std::ifstream fixture_stream(fixture_path, std::ios::binary);
  const bool fixture_open = fixture_stream.is_open();
  BOOMPI_EXPECT(context, fixture_open);
  if (!fixture_open) {
    return;
  }
  const std::string fixture_json{std::istreambuf_iterator<char>(fixture_stream),
                                 std::istreambuf_iterator<char>()};
  std::string header_hex;
  std::string payload_hex;
  std::string wire_hex;
  const bool fixture_fields_found =
      ExtractJsonString(fixture_json, "header_hex", &header_hex) &&
      ExtractJsonString(fixture_json, "payload_hex", &payload_hex) &&
      ExtractJsonString(fixture_json, "wire_hex", &wire_hex);
  BOOMPI_EXPECT(context, fixture_fields_found);
  if (!fixture_fields_found) {
    return;
  }
  BOOMPI_EXPECT(context, wire_hex == header_hex + payload_hex);

  std::vector<std::uint8_t> fixture_header;
  std::vector<std::uint8_t> fixture_wire;
  const bool fixture_hex_valid = DecodeHex(header_hex, &fixture_header) &&
                                 DecodeHex(wire_hex, &fixture_wire) &&
                                 fixture_header.size() ==
                                     boompi::protocol::kAudioPacketHeaderSize &&
                                 fixture_wire.size() > fixture_header.size();
  BOOMPI_EXPECT(context, fixture_hex_valid);
  if (!fixture_hex_valid) {
    return;
  }

  const auto header = MakeAudioHeader();
  std::array<std::uint8_t, boompi::protocol::kAudioPacketHeaderSize> encoded{};
  BOOMPI_EXPECT(
      context, boompi::protocol::EncodeAudioPacketHeader(header, &encoded).ok());
  BOOMPI_EXPECT(context,
                fixture_header.size() == encoded.size() &&
                    std::equal(encoded.begin(), encoded.end(),
                               fixture_header.begin()));
  BOOMPI_EXPECT(context, encoded[0] == static_cast<std::uint8_t>('B'));
  BOOMPI_EXPECT(context, encoded[3] == static_cast<std::uint8_t>('1'));
  BOOMPI_EXPECT(context, encoded[8] == 0U && encoded[9] == 64U);
  BOOMPI_EXPECT(context, encoded[12] == 0U && encoded[13] == 0U &&
                             encoded[14] == 0x3EU && encoded[15] == 0x80U);
  BOOMPI_EXPECT(context, encoded[20] == 0U && encoded[21] == 0U &&
                             encoded[22] == 0U && encoded[23] == 42U);

  boompi::protocol::AudioPacketHeader decoded{};
  BOOMPI_EXPECT(context,
                boompi::protocol::DecodeAudioPacketHeader(
                    fixture_wire.data(), fixture_wire.size(), &decoded)
                    .ok());
  BOOMPI_EXPECT(context, decoded.version == header.version);
  BOOMPI_EXPECT(context, decoded.kind == header.kind);
  BOOMPI_EXPECT(context, decoded.flags == header.flags);
  BOOMPI_EXPECT(context, decoded.audio_format == header.audio_format);
  BOOMPI_EXPECT(context, decoded.channels == header.channels);
  BOOMPI_EXPECT(context,
                decoded.sample_rate_hz == header.sample_rate_hz);
  BOOMPI_EXPECT(context,
                decoded.payload_length_bytes == header.payload_length_bytes);
  BOOMPI_EXPECT(context, decoded.sequence == header.sequence);
  BOOMPI_EXPECT(context, decoded.device_uuid == header.device_uuid);
  BOOMPI_EXPECT(context, decoded.monotonic_timestamp_us ==
                             header.monotonic_timestamp_us);
  BOOMPI_EXPECT(context, decoded.epoch == header.epoch);
  BOOMPI_EXPECT(context, decoded.session_id == header.session_id);
  BOOMPI_EXPECT(context, decoded.turn_id == header.turn_id);
  BOOMPI_EXPECT(context, decoded.stream_id == header.stream_id);

  boompi::protocol::AudioStreamContext stream_context{};
  stream_context.kind = header.kind;
  stream_context.sample_rate_hz = header.sample_rate_hz;
  stream_context.device_uuid = header.device_uuid;
  stream_context.epoch = header.epoch;
  stream_context.session_id = header.session_id;
  stream_context.turn_id = header.turn_id;
  stream_context.stream_id = header.stream_id;
  stream_context.expected_sequence = header.sequence;
  BOOMPI_EXPECT(
      context,
      boompi::protocol::ValidateAudioPacketContext(header, stream_context)
          .ok());

  auto wrong_context = stream_context;
  wrong_context.epoch += 1U;
  BOOMPI_EXPECT(
      context,
      !boompi::protocol::ValidateAudioPacketContext(header, wrong_context)
           .ok());
  wrong_context = stream_context;
  wrong_context.sample_rate_hz = 48000U;
  BOOMPI_EXPECT(
      context,
      !boompi::protocol::ValidateAudioPacketContext(header, wrong_context)
           .ok());
  wrong_context = stream_context;
  wrong_context.expected_sequence += 1U;
  BOOMPI_EXPECT(
      context,
      !boompi::protocol::ValidateAudioPacketContext(header, wrong_context)
           .ok());

  auto invalid_wire = fixture_wire;
  invalid_wire[0] = static_cast<std::uint8_t>('X');
  BOOMPI_EXPECT(context,
                !boompi::protocol::DecodeAudioPacketHeader(
                     invalid_wire.data(), invalid_wire.size(), &decoded)
                     .ok());
  invalid_wire = fixture_wire;
  invalid_wire[9] = 63U;
  BOOMPI_EXPECT(context,
                !boompi::protocol::DecodeAudioPacketHeader(
                     invalid_wire.data(), invalid_wire.size(), &decoded)
                     .ok());
  invalid_wire = fixture_wire;
  invalid_wire.pop_back();
  BOOMPI_EXPECT(context,
                !boompi::protocol::DecodeAudioPacketHeader(
                     invalid_wire.data(), invalid_wire.size(), &decoded)
                     .ok());

  auto maximum_payload_header = header;
  maximum_payload_header.payload_length_bytes =
      boompi::protocol::kMaximumAudioPayloadBytes;
  std::array<std::uint8_t, boompi::protocol::kAudioPacketHeaderSize>
      maximum_payload_encoded{};
  BOOMPI_EXPECT(
      context,
      boompi::protocol::EncodeAudioPacketHeader(maximum_payload_header,
                                                &maximum_payload_encoded)
          .ok());
  std::vector<std::uint8_t> maximum_payload_wire(
      boompi::protocol::kAudioPacketHeaderSize +
          boompi::protocol::kMaximumAudioPayloadBytes,
      0U);
  std::copy(maximum_payload_encoded.begin(), maximum_payload_encoded.end(),
            maximum_payload_wire.begin());
  BOOMPI_EXPECT(context,
                boompi::protocol::DecodeAudioPacketHeader(
                    maximum_payload_wire.data(), maximum_payload_wire.size(),
                    &decoded)
                    .ok());

  auto oversized_payload_header = maximum_payload_header;
  oversized_payload_header.payload_length_bytes =
      boompi::protocol::kMaximumAudioPayloadBytes + 1U;
  BOOMPI_EXPECT(
      context,
      !boompi::protocol::ValidateAudioPacketHeader(oversized_payload_header)
           .ok());

  auto invalid_flags_header = header;
  invalid_flags_header.flags = 0x0008U;
  BOOMPI_EXPECT(
      context,
      !boompi::protocol::ValidateAudioPacketHeader(invalid_flags_header).ok());

  auto stereo_header = header;
  stereo_header.channels = 2U;
  BOOMPI_EXPECT(context,
                !boompi::protocol::ValidateAudioPacketHeader(stereo_header)
                     .ok());

  boompi::protocol::EnvelopeMetadata metadata{};
  metadata.type = "runtime.ready";
  metadata.message_id = "message-1";
  metadata.device_id = "00112233-4455-6677-8899-aabbccddeeff";
  metadata.frame_size_bytes = boompi::protocol::kMaximumControlFrameBytes;
  BOOMPI_EXPECT(
      context, boompi::protocol::ValidateEnvelopeMetadata(metadata).ok());
  metadata.frame_size_bytes = 0U;
  BOOMPI_EXPECT(
      context, !boompi::protocol::ValidateEnvelopeMetadata(metadata).ok());
  metadata.frame_size_bytes =
      boompi::protocol::kMaximumControlFrameBytes + 1U;
  BOOMPI_EXPECT(
      context, !boompi::protocol::ValidateEnvelopeMetadata(metadata).ok());
  metadata.frame_size_bytes = boompi::protocol::kMaximumControlFrameBytes;
  metadata.device_id = "00000000-0000-0000-0000-000000000000";
  BOOMPI_EXPECT(
      context, !boompi::protocol::ValidateEnvelopeMetadata(metadata).ok());
}

void TestPolicies(boompi::test::TestContext& context) {
  BOOMPI_EXPECT(context,
                boompi::network::ReconnectDelayMs(0U, -100) == 900U);
  BOOMPI_EXPECT(context,
                boompi::network::ReconnectDelayMs(99U, 100) == 33000U);

  boompi::ui::UiModel ui_model{};
  BOOMPI_EXPECT(context, boompi::ui::SetVolume(&ui_model, 75).ok());
  BOOMPI_EXPECT(context, ui_model.volume_percent == 75U);
  BOOMPI_EXPECT(context, !boompi::ui::SetBrightness(&ui_model, 101).ok());

  boompi::update::AppSlot slot{};
  BOOMPI_EXPECT(context, boompi::update::ParseAppSlot("A", &slot).ok());
  BOOMPI_EXPECT(context,
                boompi::update::InactiveSlot(slot) ==
                    boompi::update::AppSlot::kB);
  BOOMPI_EXPECT(context, !boompi::update::ParseAppSlot("invalid", &slot).ok());
}

}  // namespace

int main(const int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "usage: boompi_host_tests <protocol-fixture.json>\n";
    return 2;
  }
  boompi::test::TestContext context;
  TestConfigurationAndAudio(context);
  TestBoundedEventBus(context);
  TestRuntimeLifecycle(context);
  TestRuntimeStopRetryAfterFullQueue(context);
  TestProtocolHeader(context, argv[1]);
  TestPolicies(context);

  if (context.failures() != 0U) {
    std::cerr << context.failures() << " boomPI host expectation(s) failed\n";
    return 1;
  }
  std::cout << "boomPI host tests passed\n";
  return 0;
}
