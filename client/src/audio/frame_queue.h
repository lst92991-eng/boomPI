/** @file frame_queue.h
 * @brief 固定容量的帧存储；线程同步、溢出和覆盖策略由调用方决定。
 */
#pragma once

#include <array>
#include <cstddef>

namespace boompi::audio {

template <typename Frame, std::size_t Capacity>
class FrameQueue final {
  static_assert(Capacity > 0U, "frame queue needs at least one slot");

 public:
  bool Push(const Frame& frame) noexcept {
    if (count_ == Capacity) {
      return false;
    }
    frames_[(head_ + count_) % Capacity] = frame;
    ++count_;
    return true;
  }

  // output 为空表示只移除队首，供调用方明确实现历史音频滚动策略。
  bool Pop(Frame* output = nullptr) noexcept {
    if (count_ == 0U) {
      return false;
    }
    if (output != nullptr) {
      *output = frames_[head_];
    }
    head_ = (head_ + 1U) % Capacity;
    --count_;
    return true;
  }

  void Clear() noexcept {
    head_ = count_ = 0U;
  }

  std::size_t Size() const noexcept {
    return count_;
  }

  // 索引从当前队首计数，调用方保证 index < Size()。
  Frame& operator[](std::size_t index) noexcept {
    return frames_[(head_ + index) % Capacity];
  }
  const Frame& operator[](std::size_t index) const noexcept {
    return frames_[(head_ + index) % Capacity];
  }

 private:
  std::array<Frame, Capacity> frames_{};
  std::size_t head_{0U};
  std::size_t count_{0U};
};

}  // namespace boompi::audio
