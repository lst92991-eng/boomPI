#ifndef BOOMPI_AUDIO_SPSC_AUDIO_FRAME_QUEUE_H_
#define BOOMPI_AUDIO_SPSC_AUDIO_FRAME_QUEUE_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <utility>

namespace boompi::audio {

// The queue owns its frame pool. Exactly one producer may acquire/publish
// writable leases and exactly one consumer may acquire/release readable
// leases. Leases must not cross to a third thread and must not outlive the
// queue. A full queue rejects the newest frame without blocking or overwriting
// a slot that may still be owned by the consumer.
template <typename Frame, std::uint32_t Capacity>
class SpscAudioFrameQueue final {
  static_assert(Capacity > 0U, "SPSC queue capacity must be non-zero");
  static_assert((Capacity & (Capacity - 1U)) == 0U,
                "SPSC queue capacity must be a power of two");
  static_assert(Capacity < (std::numeric_limits<std::uint32_t>::max() / 2U),
                "SPSC queue capacity is too large for position arithmetic");
  static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                "RV1106 audio queues require lock-free 32-bit atomics");
  static constexpr std::uint32_t kSlotMask = Capacity - 1U;
  static constexpr std::uint32_t kPositionMask = (Capacity * 2U) - 1U;

 public:
  class ProducerLease final {
   public:
    ProducerLease() noexcept = default;
    ProducerLease(const ProducerLease&) = delete;
    ProducerLease& operator=(const ProducerLease&) = delete;

    ProducerLease(ProducerLease&& other) noexcept { MoveFrom(other); }

    ProducerLease& operator=(ProducerLease&& other) noexcept {
      if (this != &other) {
        Cancel();
        MoveFrom(other);
      }
      return *this;
    }

    ~ProducerLease() noexcept { Cancel(); }

    explicit operator bool() const noexcept { return frame_ != nullptr; }
    // All pointers and references obtained from a lease become invalid as
    // soon as that lease is published, cancelled, released, moved-from, or
    // destroyed. They must never escape to another thread or owner.
    Frame* get() noexcept { return frame_; }
    Frame& frame() noexcept { return *frame_; }
    Frame* operator->() noexcept { return frame_; }
    Frame& operator*() noexcept { return *frame_; }

    bool Publish() noexcept {
      if (owner_ == nullptr || frame_ == nullptr ||
          !frame_->HasValidLength() || frame_->metadata.epoch == 0U ||
          frame_->metadata.stream_id == 0U) {
        return false;
      }
      owner_->PublishWrite(position_);
      Invalidate();
      return true;
    }

    bool Cancel() noexcept {
      if (owner_ == nullptr) {
        return false;
      }
      owner_->CancelWrite();
      Invalidate();
      return true;
    }

   private:
    friend class SpscAudioFrameQueue<Frame, Capacity>;

    ProducerLease(SpscAudioFrameQueue* const owner, Frame* const frame,
                  const std::uint32_t position) noexcept
        : owner_(owner), frame_(frame), position_(position) {}

    void MoveFrom(ProducerLease& other) noexcept {
      owner_ = other.owner_;
      frame_ = other.frame_;
      position_ = other.position_;
      other.Invalidate();
    }

    void Invalidate() noexcept {
      owner_ = nullptr;
      frame_ = nullptr;
      position_ = 0U;
    }

    SpscAudioFrameQueue* owner_{nullptr};
    Frame* frame_{nullptr};
    std::uint32_t position_{0U};
  };

  class ConsumerLease final {
   public:
    ConsumerLease() noexcept = default;
    ConsumerLease(const ConsumerLease&) = delete;
    ConsumerLease& operator=(const ConsumerLease&) = delete;

    ConsumerLease(ConsumerLease&& other) noexcept { MoveFrom(other); }

    ConsumerLease& operator=(ConsumerLease&& other) noexcept {
      if (this != &other) {
        Release();
        MoveFrom(other);
      }
      return *this;
    }

    ~ConsumerLease() noexcept { Release(); }

    explicit operator bool() const noexcept { return frame_ != nullptr; }
    // The returned view is borrowed only for this lease's lifetime.
    const Frame* get() const noexcept { return frame_; }
    const Frame& frame() const noexcept { return *frame_; }
    const Frame* operator->() const noexcept { return frame_; }
    const Frame& operator*() const noexcept { return *frame_; }

    bool Release() noexcept {
      if (owner_ == nullptr) {
        return false;
      }
      owner_->ReleaseRead(position_);
      Invalidate();
      return true;
    }

   private:
    friend class SpscAudioFrameQueue<Frame, Capacity>;

    ConsumerLease(SpscAudioFrameQueue* const owner, const Frame* const frame,
                  const std::uint32_t position) noexcept
        : owner_(owner), frame_(frame), position_(position) {}

    void MoveFrom(ConsumerLease& other) noexcept {
      owner_ = other.owner_;
      frame_ = other.frame_;
      position_ = other.position_;
      other.Invalidate();
    }

    void Invalidate() noexcept {
      owner_ = nullptr;
      frame_ = nullptr;
      position_ = 0U;
    }

    SpscAudioFrameQueue* owner_{nullptr};
    const Frame* frame_{nullptr};
    std::uint32_t position_{0U};
  };

  SpscAudioFrameQueue() noexcept = default;
  SpscAudioFrameQueue(const SpscAudioFrameQueue&) = delete;
  SpscAudioFrameQueue& operator=(const SpscAudioFrameQueue&) = delete;
  SpscAudioFrameQueue(SpscAudioFrameQueue&&) = delete;
  SpscAudioFrameQueue& operator=(SpscAudioFrameQueue&&) = delete;

  ProducerLease TryAcquireWrite() noexcept {
    if (producer_lease_active_) {
      return ProducerLease{};
    }

    const auto write_position =
        write_position_.load(std::memory_order_relaxed);
    const auto read_position = read_position_.load(std::memory_order_acquire);
    const auto occupied = static_cast<std::uint32_t>(
        (write_position - read_position) & kPositionMask);
    if (occupied >= Capacity) {
      return ProducerLease{};
    }

    auto& slot = slots_[write_position & kSlotMask];
    slot.ResetHeader();
    producer_lease_active_ = true;
    return ProducerLease{this, &slot, write_position};
  }

  ConsumerLease TryAcquireRead() noexcept {
    if (consumer_lease_active_) {
      return ConsumerLease{};
    }

    const auto read_position = read_position_.load(std::memory_order_relaxed);
    const auto write_position =
        write_position_.load(std::memory_order_acquire);
    if (read_position == write_position) {
      return ConsumerLease{};
    }

    const auto& slot = slots_[read_position & kSlotMask];
    consumer_lease_active_ = true;
    return ConsumerLease{this, &slot, read_position};
  }

  static constexpr std::uint32_t capacity() noexcept { return Capacity; }

  // This value is intended for diagnostics only. Concurrent observations may
  // be stale and must not be used as a prerequisite for acquire operations.
  std::uint32_t size_approx() const noexcept {
    const auto read_position = read_position_.load(std::memory_order_acquire);
    const auto write_position =
        write_position_.load(std::memory_order_acquire);
    const auto observed = static_cast<std::uint32_t>(
        (write_position - read_position) & kPositionMask);
    return observed > Capacity ? Capacity : observed;
  }

 private:
  void PublishWrite(const std::uint32_t position) noexcept {
    producer_lease_active_ = false;
    write_position_.store((position + 1U) & kPositionMask,
                          std::memory_order_release);
  }

  void CancelWrite() noexcept { producer_lease_active_ = false; }

  void ReleaseRead(const std::uint32_t position) noexcept {
    consumer_lease_active_ = false;
    read_position_.store((position + 1U) & kPositionMask,
                         std::memory_order_release);
  }

  std::array<Frame, Capacity> slots_{};

  std::atomic<std::uint32_t> write_position_{0U};
  bool producer_lease_active_{false};
  std::atomic<std::uint32_t> read_position_{0U};
  bool consumer_lease_active_{false};
};

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_SPSC_AUDIO_FRAME_QUEUE_H_
