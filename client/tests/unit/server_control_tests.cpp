#include <cstdint>
#include <iostream>
#include <string>

#include "boompi/protocol/server_control.h"
#include "boompi/test/test_context.h"

#define BOOMPI_EXPECT(context, expression) \
  (context).Expect((expression), #expression, __FILE__, __LINE__)

namespace {

constexpr char kDeviceId[] = "00112233-4455-6677-8899-aabbccddeeff";

std::string Control(const std::string& type, const std::string& payload) {
  return "{\"version\":1,\"type\":\"" + type +
         "\",\"message_id\":\"message-1\",\"device_id\":\"" +
         kDeviceId +
         "\",\"session_id\":2,\"turn_id\":3,\"stream_id\":4,"
         "\"epoch\":5,\"payload\":" +
         payload + "}";
}

bool Decodes(const std::string& json,
             boompi::protocol::ServerControlMessage* const output) {
  return boompi::protocol::DecodeServerControl(json, kDeviceId, output).ok();
}

void TestAcceptedMessages(boompi::test::TestContext& context) {
  boompi::protocol::ServerControlMessage message{};
  BOOMPI_EXPECT(
      context,
      Decodes(Control("hello.ack",
                      "{\"input_sample_rate_hz\":16000,"
                      "\"output_sample_rate_hz\":24000,"
                      "\"input_frame_ms\":20}"),
              &message));
  BOOMPI_EXPECT(
      context,
      message.type == boompi::protocol::ServerControlType::kHelloAck);
  BOOMPI_EXPECT(context, message.input_sample_rate_hz == 16000U);
  BOOMPI_EXPECT(context, message.output_sample_rate_hz == 24000U);
  BOOMPI_EXPECT(context, message.input_frame_ms == 20U);

  BOOMPI_EXPECT(context,
                Decodes(Control("response.start",
                                "{\"response_id\":\"response-1\"}"),
                        &message));
  BOOMPI_EXPECT(
      context,
      message.type == boompi::protocol::ServerControlType::kResponseStart);
  BOOMPI_EXPECT(context, message.response_id == "response-1");

  BOOMPI_EXPECT(
      context,
      Decodes(Control("response.text_delta",
                      "{\"response_id\":\"response-1\","
                      "\"text\":\"\\u4f60\\u597d \\ud83d\\ude03\"}"),
              &message));
  BOOMPI_EXPECT(
      context,
      message.type == boompi::protocol::ServerControlType::kResponseTextDelta);
  BOOMPI_EXPECT(context, message.text == "\xE4\xBD\xA0\xE5\xA5\xBD \xF0\x9F\x98\x83");

  BOOMPI_EXPECT(
      context,
      Decodes(Control("response.audio_start",
                      "{\"sample_rate_hz\":24000,"
                      "\"response_id\":\"response-1\"}"),
              &message));
  BOOMPI_EXPECT(
      context,
      message.type == boompi::protocol::ServerControlType::kResponseAudioStart);
  BOOMPI_EXPECT(context, message.sample_rate_hz == 24000U);

  BOOMPI_EXPECT(context,
                Decodes(Control("response.done",
                                "{\"response_id\":\"response-1\"}"),
                        &message));
  BOOMPI_EXPECT(
      context,
      message.type == boompi::protocol::ServerControlType::kResponseDone);

  BOOMPI_EXPECT(
      context,
      Decodes(Control("error",
                      "{\"code\":\"provider_error\","
                      "\"message\":\"request failed\"}"),
              &message));
  BOOMPI_EXPECT(context,
                message.type == boompi::protocol::ServerControlType::kError);
  BOOMPI_EXPECT(context, message.error_code == "provider_error");
  BOOMPI_EXPECT(context, message.error_message == "request failed");
}

void TestEnvelopeRejections(boompi::test::TestContext& context) {
  boompi::protocol::ServerControlMessage message{};
  const std::string valid =
      Control("response.done", "{\"response_id\":\"response-1\"}");
  std::string duplicate = valid;
  duplicate.insert(duplicate.size() - 1U, ",\"epoch\":6");
  BOOMPI_EXPECT(context, !Decodes(duplicate, &message));

  std::string unknown = valid;
  unknown.insert(unknown.size() - 1U, ",\"extra\":true");
  BOOMPI_EXPECT(context, !Decodes(unknown, &message));
  BOOMPI_EXPECT(context, !Decodes(valid + " null", &message));
  BOOMPI_EXPECT(
      context,
      !boompi::protocol::DecodeServerControl(
           valid, "10112233-4455-6677-8899-aabbccddeeff", &message)
           .ok());

  std::string oversized(boompi::protocol::kMaximumControlFrameBytes + 1U,
                        'x');
  BOOMPI_EXPECT(context, !Decodes(oversized, &message));

  std::string nested = "0";
  for (std::size_t depth = 0U; depth < 20U; ++depth) {
    nested = "[" + nested + "]";
  }
  BOOMPI_EXPECT(
      context,
      !Decodes(Control("response.done",
                       "{\"response_id\":\"response-1\","
                       "\"nested\":" +
                           nested + "}"),
               &message));

  std::string many_nodes = "[";
  for (std::size_t index = 0U; index < 260U; ++index) {
    if (index != 0U) {
      many_nodes.push_back(',');
    }
    many_nodes.push_back('0');
  }
  many_nodes.push_back(']');
  BOOMPI_EXPECT(
      context,
      !Decodes(Control("response.done",
                       "{\"response_id\":\"response-1\","
                       "\"nodes\":" +
                           many_nodes + "}"),
               &message));

  std::string invalid_utf8 = valid;
  invalid_utf8.insert(invalid_utf8.find("response-1"), 1U,
                      static_cast<char>(0xC0));
  BOOMPI_EXPECT(context, !Decodes(invalid_utf8, &message));
}

void TestPayloadRejections(boompi::test::TestContext& context) {
  boompi::protocol::ServerControlMessage message{};
  BOOMPI_EXPECT(
      context,
      !Decodes(Control("response.done",
                       "{\"response_id\":\"one\","
                       "\"response_id\":\"two\"}"),
               &message));
  BOOMPI_EXPECT(
      context,
      !Decodes(Control("response.done",
                       "{\"response_id\":\"one\",\"extra\":1}"),
               &message));
  BOOMPI_EXPECT(context,
                !Decodes(Control("response.done", "{}"), &message));
  BOOMPI_EXPECT(
      context,
      !Decodes(Control("response.audio_start",
                       "{\"response_id\":\"one\","
                       "\"sample_rate_hz\":-1}"),
               &message));
  BOOMPI_EXPECT(
      context,
      !Decodes(Control("hello.ack",
                       "{\"input_sample_rate_hz\":16000,"
                       "\"output_sample_rate_hz\":24000}"),
               &message));
  BOOMPI_EXPECT(
      context,
      !Decodes(Control("response.text_delta",
                       "{\"response_id\":\"one\",\"text\":\"\"}"),
               &message));
  BOOMPI_EXPECT(
      context,
      !Decodes(Control("response.text_delta",
                       "{\"response_id\":\"one\",\"text\":\"" +
                           std::string(4097U, 'x') + "\"}"),
               &message));
  BOOMPI_EXPECT(
      context,
      !Decodes(Control("response.done",
                       "{\"response_id\":\"\\ud800\"}"),
               &message));
  BOOMPI_EXPECT(
      context,
      !Decodes(Control("state.update", "{}"), &message));
}

}  // namespace

int main() {
  boompi::test::TestContext context;
  TestAcceptedMessages(context);
  TestEnvelopeRejections(context);
  TestPayloadRejections(context);
  if (context.failures() != 0U) {
    std::cerr << context.failures()
              << " server control expectation(s) failed\n";
    return 1;
  }
  std::cout << "boomPI server control tests passed\n";
  return 0;
}
