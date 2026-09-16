/** @file playback.h
 * @brief 接收 16k 单声道 PCM，异步转换并写声卡；对话何时开始或结束由应用决定。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace boompi::playback {

// 错误由同一份原因记录导出Failed状态，保留到下次open；cancel不能清掉。
enum class State { Idle, Playing, Drained, Failed };
enum class WriteResult { Queued, Full, Rejected, InvalidArgument };

// capture已打开后调用；直接拥有播放队列、声卡输出、重采样器和播放线程。
bool open(std::uint8_t volume = 60U);
// 主线程是唯一生产者；首次write起播，等旧取消完成后才能接新音频。
// 输入为16kHz/mono/S16_LE；按采样缓存，容量不足明确返回Full，不静默丢弃。
WriteResult write(const void* bytes, std::size_t byte_count);
// DONE只关闭输入；status到Drained才表示重采样器和声卡尾音均已播完。
void finish();
// 异步中断write/drain并丢队列；收尾后status为Idle，旧数据不可再投递。
void cancel();
// 试探期间不消费TTS/滤波历史，只写设备静音；相同true不续期，最多500ms。
void hold(bool enabled);
// 输入线程取无锁观测；true仅表示播放线程已写入试探静音，仍须检查实际参考。
bool held() noexcept;
/** @brief 原子更新 0..100 音量；播放线程在后续输出块应用，参数超过 100 时限制为 100。 */
void set_volume(std::uint8_t volume);
/** @brief 返回瞬时状态；Failed 优先于播放进度，Drained 只在正常尾播完成后出现。 */
State status();
/** @brief 复制本次 open 以来的首个错误原因；cancel 不会抹掉故障。 */
std::string error();
/** @brief 请求停止并打断输出，join 后关闭 PCM 和转换器，清空队列。 */
void close();

}  // namespace boompi::playback
