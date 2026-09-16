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

/** @brief capture 已打开后配置播放资源，再创建线程；失败回收已取得的资源。 */
bool open(std::uint8_t volume = 60U);
/** @brief 主线程投递 16k/mono/S16_LE，首次投递启动播放；容量不足返回 Full，不丢旧采样。
 * 旧回答取消后最多等待 60ms 收尾才接纳新音频；等待超时返回 Rejected 并保留故障。
 */
WriteResult write(const void* bytes, std::size_t byte_count);
/** @brief DONE 只关闭输入；status 到 Drained 才表示滤波器及声卡尾音均已播完。 */
void finish();
/** @brief 异步打断输出并丢弃队列；收尾后为 Idle，调用方需保证不再投递旧轮音频。 */
void cancel();
/** @brief 试探期间不消费 TTS/滤波历史，只写设备静音；相同 true 不续期，最多 500ms。 */
void hold(bool enabled);
/** @brief 输入线程读取无锁观测；true 表示已写入试探静音，仍须检查实际回采参考。 */
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
