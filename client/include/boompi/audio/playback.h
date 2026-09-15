#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace boompi::playback {

// Failed锁存到close/open，不能被cancel或下一轮begin清掉。
enum class End { None, Natural, Interrupted };
struct Observation {
  bool render_started{false}, output_audible{false};
  End end{End::None};
};
Observation observe();

enum class State { Idle, Playing, Drained, Failed };
struct Status final {
  std::uint32_t generation{0U};
  State state{State::Idle};
};
enum class WriteResult {
  Queued,
  NotOpen,
  InvalidArgument,
  NotActive,
  Ending,
  Full,
  StaleGeneration,
};

// capture已打开后调用；直接拥有播放队列、声卡输出、重采样器和播放线程。
bool open(std::uint8_t volume = 60U);
// 等旧取消收尾；generation非零且递增。网络独占sequence校验，本模块只隔离播放轮次。
bool begin(std::uint32_t generation);
// 16kHz/mono/S16_LE，完整包320样本；短包只能是末包，满队列必须取消整轮。
WriteResult write(std::uint32_t generation, const std::uint8_t* bytes, std::size_t byte_count);
// END只关闭输入；status到Drained才表示重采样器和声卡尾音均已播完。
bool finish(std::uint32_t generation);
// 异步中断write/drain并丢队列；收尾后status为Idle，保留退休generation，旧代数据不可再投递。
void cancel();
void set_volume(std::uint8_t volume);
void set_scale(float scale);
Status status();
std::string error();
void close();

}  // namespace boompi::playback
