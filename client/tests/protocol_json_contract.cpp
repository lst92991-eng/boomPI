/**
 * @file protocol_json_contract.cpp
 * @brief 直接调用产品编解码器，验证 v2 JSON 拒绝规则和跨语言共享字节样本。
 *
 * main 的 cases 覆盖合法控制帧、歧义数字/字符串、重复字段及长度边界；传入 fixture
 * 路径时再由 SharedFixtures 将产品编码结果逐字节对照 protocol/fixtures 下的金样。
 * 此测试不建网络连接，也不检查跨帧顺序；TLS、generation 隔离和发送队列另见
 * voice_transport_loopback_test.cpp。
 */
#include <cjson/cJSON.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../src/network/voice_codec.h"

namespace {

/// @brief 用异常保留具体 fixture 名和失败阶段，让主入口统一打印并返回非零退出码。
void Check(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

/// @brief 读取测试样本元数据字符串；这是测试文件读取器，不是产品控制帧解码器。
std::string Field(const cJSON* object, const char* key) {
  const auto* item = cJSON_GetObjectItemCaseSensitive(object, key);
  Check(cJSON_IsString(item) && item->valuestring, std::string("fixture string: ") + key);
  return item->valuestring;
}

/// @brief 拒绝样本元数据的缺失、非有限数或越界整数，避免错误 fixture 伪装成产品失败。
std::uint32_t Number(const cJSON* object, const char* key) {
  const auto* item = cJSON_GetObjectItemCaseSensitive(object, key);
  Check(cJSON_IsNumber(item) && std::isfinite(item->valuedouble) && item->valuedouble >= 0 &&
            item->valuedouble <= UINT32_MAX &&
            std::floor(item->valuedouble) == item->valuedouble,
        std::string("fixture integer: ") + key);
  return static_cast<std::uint32_t>(item->valuedouble);
}

/// @brief 把共享金样中的小写十六进制恢复成原始字节，保留 NUL 及任意非文本 PCM。
std::string HexBytes(const std::string& hex) {
  Check(hex.size() % 2 == 0, "fixture hex has odd length");
  std::string bytes;
  const std::string digits = "0123456789abcdef";
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const auto high = digits.find(hex[i]);
    const auto low = digits.find(hex[i + 1]);
    Check(high != std::string::npos && low != std::string::npos, "fixture has invalid hex");
    bytes.push_back(static_cast<char>((high << 4U) | low));
  }
  return bytes;
}

/**
 * @brief 读取 v2 金样后分方向验证：下行解码字段，上行编码字节。
 *
 * 先校验 fixture 自身头部和负载能重组为 wire，再交给产品函数，避免把自相矛盾的
 * 样本作为预期值。最低样本数保证没有因路径或筛选错误而空跑通过。
 */
void SharedFixtures(const char* path) {
  using namespace boompi::network;
  std::ifstream input(path, std::ios::binary);
  Check(input.good(), std::string("cannot read shared fixture: ") + path);
  const std::string json{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};
  const char* end = nullptr;
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
      cJSON_ParseWithLengthOpts(json.c_str(), json.size() + 1, &end, true), &cJSON_Delete);
  Check(root && end == json.c_str() + json.size() && Number(root.get(), "fixture_version") == 2,
        "invalid v2 fixture document");
  const auto* controls = cJSON_GetObjectItemCaseSensitive(root.get(), "control_frames");
  const auto* audio = cJSON_GetObjectItemCaseSensitive(root.get(), "audio_frames");
  Check(cJSON_IsArray(controls) && cJSON_IsArray(audio), "fixture frame arrays missing");
  unsigned control_count = 0, uplink_count = 0, downlink_count = 0;
  for (const auto* item = controls->child; item; item = item->next) {
    const auto* expected = cJSON_GetObjectItemCaseSensitive(item, "expected");
    const auto type = Field(expected, "type");
    // hello/stop 属于上行控制，由真实 TLS 回环测试检查；下行解码器应拒绝这两种类型。
    if (type == "hello" || type == "stop") {
      continue;
    }
    const auto name = Field(item, "name");
    const auto event = detail::DecodeText(Field(item, "wire_text"));
    if (type == "ready") {
      Check(event.kind == LinkEventKind::Online && event.generation == 0,
            name + ": ready mismatch");
    } else {
      Check(event.generation == Number(expected, "generation"), name + ": generation mismatch");
      if (type == "text") {
        Check(event.kind == LinkEventKind::Text && event.text == Field(expected, "text"),
              name + ": text mismatch");
      } else if (type == "done") {
        Check(event.kind == LinkEventKind::Done, name + ": done mismatch");
      } else if (type == "error") {
        Check(event.kind == LinkEventKind::Error && event.code == Field(expected, "code"),
              name + ": error mismatch");
      } else {
        throw std::runtime_error(name + ": unsupported fixture control");
      }
    }
    ++control_count;
  }
  for (const auto* item = audio->child; item; item = item->next) {
    const auto name = Field(item, "name");
    const auto* header = cJSON_GetObjectItemCaseSensitive(item, "header");
    const auto flags = Number(header, "flags");
    const auto generation = Number(header, "generation");
    const auto sequence = Number(header, "sequence");
    const auto payload = HexBytes(Field(item, "payload_hex"));
    const auto wire = HexBytes(Field(item, "wire_hex"));
    Check(wire == HexBytes(Field(item, "header_hex")) + payload,
          name + ": inconsistent fixture bytes");
    const auto direction = Field(item, "direction");
    if (direction == "uplink") {
      Check(payload.size() == detail::kPcmBytes, name + ": uplink size mismatch");
      std::array<std::int16_t, detail::kPcmBytes / 2> pcm{};
      // 金样 PCM 为小端字节；显式还原有符号 sample，避免依赖测试主机端序或对齐。
      for (std::size_t i = 0; i < pcm.size(); ++i) {
        const auto value =
            static_cast<unsigned char>(payload[2 * i]) |
            (static_cast<unsigned>(static_cast<unsigned char>(payload[2 * i + 1])) << 8U);
        pcm[i] = static_cast<std::int16_t>(value < 32768 ? static_cast<int>(value)
                                                         : static_cast<int>(value) - 65536);
      }
      // 预期 wire 来自独立共享金样，不用生产编码器输出反过来构造自己的期望。
      const auto encoded =
          detail::EncodeAudio(generation, sequence, pcm.data(), (flags & 1U) != 0,
                              (flags & 2U) != 0, (flags & 4U) != 0);
      Check(std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size()) == wire,
            name + ": encoded bytes differ from shared golden wire");
      ++uplink_count;
    } else {
      Check(direction == "downlink", name + ": unknown direction");
      const auto event = detail::DecodeAudio(wire);
      Check(event.kind == LinkEventKind::Audio && event.generation == generation &&
                event.sequence == sequence && event.start == ((flags & 1U) != 0) &&
                event.end == ((flags & 2U) != 0) && event.audio_size == payload.size() &&
                std::string(reinterpret_cast<const char*>(event.audio.data()),
                            event.audio_size) == payload,
            name + ": decoded audio differs from shared golden fields");
      ++downlink_count;
    }
  }
  Check(control_count >= 4 && uplink_count >= 2 && downlink_count >= 3,
        "shared fixture coverage is incomplete");
  std::cout << "v2 shared fixture: " << control_count << " controls, " << uplink_count
            << " uplink, " << downlink_count << " downlink passed\n";
}

/// @brief 将生产解码器的“返回/抛异常”转换为数据表可比较的接受结果。
bool Accepted(const std::string& json) {
  try {
    (void)boompi::network::detail::DecodeText(json);
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

/**
 * @brief 运行拒绝规则矩阵，可选再读共享 fixture；任一差异最终返回非零状态。
 *
 * 字面反斜杠 u0000 可以作为正文，真正转义 NUL 必须拒绝；生成 invalid_utf8 与
 * embedded_nul 则覆盖源码字符串字面量难以直观看出的原始字节错误。
 */
int main(int argc, char** argv) {
  if (argc > 2) {
    std::cerr << "usage: protocol-json-test [shared-fixture.json]\n";
    return 1;
  }
  const std::string ready = R"({"type":"ready","sample_rate":16000})";
  const std::string text = R"({"type":"text","generation":1,"text":"你好"})";
  std::string invalid_utf8 = text;
  invalid_utf8.insert(invalid_utf8.find("你好"), "\xc3\x28", 2);
  std::string embedded_nul = text;
  embedded_nul.insert(embedded_nul.find("你好"), 1, '\0');
  // 第一段列合法值，随后依次覆盖对象形状、代号表示法、文本/错误码和长度上限。
  const std::vector<std::pair<std::string, bool>> cases{
      {ready, true},
      {ready + " \n\t", true},
      {text, true},
      {R"({"type":"done","generation":4294967295})", true},
      {R"({"type":"error","generation":1,"code":"provider_timeout"})", true},
      {R"({"type":"text","generation":1,"text":"\\u0000"})", true},
      {R"({"type":"text","generation":1,"text":"\ud83d\ude42"})", true},
      {R"([])", false},
      {R"({"type":1})", false},
      {R"({"type":"ready"})", false},
      {R"({"type":"ready","sample_rate":24000})", false},
      {R"({"type":"ready","sample_rate":"16000"})", false},
      {R"({"type":"ready","sample_rate":true})", false},
      {R"({"type":"ready","sample_rate":16000.0})", false},
      {R"({"type":"ready","sample_rate":16e3})", false},
      {R"({"type":"ready","sample_rate":16000,"sample_rate":16000})", false},
      {R"({"type":"ready","extra":0})", false},
      {R"({"type":"ready","type":"ready"})", false},
      {R"({"type":"ready","\u0074ype":"ready"})", false},
      {R"({"type\u0000hidden":"ready"})", false},
      {R"({"type":"ready\u0000hidden"})", false},
      {R"({"type":"done"})", false},
      {R"({"type":"done","generation":"1"})", false},
      {R"({"type":"done","generation":true})", false},
      {R"({"type":"done","generation":null})", false},
      {R"({"type":"done","generation":[]})", false},
      {R"({"type":"done","generation":{}})", false},
      {R"({"type":"done","generation":0})", false},
      {R"({"type":"done","generation":-0})", false},
      {R"({"type":"done","generation":-1})", false},
      {R"({"type":"done","generation":01})", false},
      {R"({"type":"done","generation":1.0})", false},
      {R"({"type":"done","generation":1e0})", false},
      {R"({"type":"done","generation":4294967296})", false},
      {R"({"type":"done","generation":1,"generation":1})", false},
      {R"({"type":"done","generation":1,"\u0067eneration":1})", false},
      {R"({"type":"text","generation":1,"text":false})", false},
      {R"({"type":"text","generation":1,"text":""})", false},
      {R"({"type":"text","generation":1,"text":"bad\qescape"})", false},
      {R"({"type":"text","generation":1,"text":"\u0000"})", false},
      {R"({"type":"text","generation":1,"text":"\ud800"})", false},
      {R"({"type":"text","generation":1,"text":"\udc00"})", false},
      {R"({"type":"text","generation":1,"text":"ok","other":{"x":1,"x":2}})", false},
      {R"({"type":"error","generation":1,"code":null})", false},
      {R"({"type":"error","generation":1,"code":"raw exception\n"})", false},
      {R"({"type":"hello"})", false},
      {R"({"version":1,"type":"hello.ack"})", false},
      {ready + "{}", false},
      {invalid_utf8, false},
      {embedded_nul, false},
      {R"({"type":"text","generation":1,"text":")" + std::string(4096, 'x') + "\"}", true},
      {R"({"type":"text","generation":1,"text":")" + std::string(4097, 'x') + "\"}", false},
      {ready + std::string(8192, ' '), false},
  };
  unsigned failures = 0;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    if (Accepted(cases[i].first) != cases[i].second) {
      std::cerr << "v2 JSON case " << i << " failed\n";
      ++failures;
    }
  }
  if (argc == 2) {
    try {
      SharedFixtures(argv[1]);
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      ++failures;
    }
  }
  return failures ? 1 : 0;
}
