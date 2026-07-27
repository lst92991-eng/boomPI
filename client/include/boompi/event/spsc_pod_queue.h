#ifndef BOOMPI_EVENT_SPSC_POD_QUEUE_H_
#define BOOMPI_EVENT_SPSC_POD_QUEUE_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace boompi::event {

// Fixed-capacity value queue for one producer and one consumer. Unlike the
// audio-frame queue, this queue deliberately has no lease: a control message
// becomes visible atomically with TryPush and no unpublished command can be
// retained across a cancellation boundary. Message contracts must be
// self-contained values (no borrowed pointers/references). The queue must
// outlive every endpoint access; both endpoints must stop and join before it
// is destroyed.
template <typename Message, std::uint32_t Capacity>
class SpscPodQueue final {
  static_assert(Capacity > 0U, "SPSC POD queue capacity must be non-zero");
  static_assert((Capacity & (Capacity - 1U)) == 0U,
                "SPSC POD queue capacity must be a power of two");
  static_assert(Capacity < (std::numeric_limits<std::uint32_t>::max() / 2U),
                "SPSC POD queue capacity is too large");
  static_assert(std::is_trivially_copyable<Message>::value,
                "SPSC POD queue messages must be trivially copyable");
  static_assert(std::is_standard_layout<Message>::value,
                "SPSC POD queue messages must be standard-layout");
  static_assert(std::is_nothrow_copy_assignable<Message>::value,
                "SPSC POD queue messages must be nothrow copy-assignable");
  static_assert(
      std::is_nothrow_default_constructible<Message>::value,
      "SPSC POD queue messages must be nothrow default-constructible");
  static_assert(!std::is_pointer<Message>::value,
                "SPSC POD queue messages must be self-contained values");
  static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                "real-time control queues require lock-free 32-bit atomics");

  static constexpr std::uint32_t kSlotMask = Capacity - 1U;
  static constexpr std::uint32_t kPositionMask = (Capacity * 2U) - 1U;

 public:
  SpscPodQueue() noexcept = default;
  SpscPodQueue(const SpscPodQueue&) = delete;
  SpscPodQueue& operator=(const SpscPodQueue&) = delete;
  SpscPodQueue(SpscPodQueue&&) = delete;
  SpscPodQueue& operator=(SpscPodQueue&&) = delete;

  bool TryPush(const Message& message) noexcept {
    const std::uint32_t write_position =
        write_position_.load(std::memory_order_relaxed);
    const std::uint32_t read_position =
        read_position_.load(std::memory_order_acquire);
    const std::uint32_t occupied = static_cast<std::uint32_t>(
        (write_position - read_position) & kPositionMask);
    if (occupied >= Capacity) {
      return false;
    }

    slots_[write_position & kSlotMask] = message;
    write_position_.store((write_position + 1U) & kPositionMask,
                          std::memory_order_release);
    return true;
  }

  bool TryPop(Message* const output) noexcept {
    if (output == nullptr) {
      return false;
    }

    const std::uint32_t read_position =
        read_position_.load(std::memory_order_relaxed);
    const std::uint32_t write_position =
        write_position_.load(std::memory_order_acquire);
    if (read_position == write_position) {
      return false;
    }

    *output = slots_[read_position & kSlotMask];
    read_position_.store((read_position + 1U) & kPositionMask,
                         std::memory_order_release);
    return true;
  }

  static constexpr std::uint32_t capacity() noexcept { return Capacity; }

  // Diagnostics only. A concurrent observation can immediately become stale
  // and must not be used to reserve capacity or prove a queue is empty.
  std::uint32_t size_approx() const noexcept {
    const std::uint32_t read_position =
        read_position_.load(std::memory_order_acquire);
    const std::uint32_t write_position =
        write_position_.load(std::memory_order_acquire);
    const std::uint32_t observed = static_cast<std::uint32_t>(
        (write_position - read_position) & kPositionMask);
    return observed > Capacity ? Capacity : observed;
  }

 private:
  std::array<Message, Capacity> slots_{};
  alignas(std::atomic<std::uint32_t>)
      std::atomic<std::uint32_t> write_position_{0U};
  alignas(std::atomic<std::uint32_t>) std::atomic<std::uint32_t> read_position_{
      0U};
};

}  // namespace boompi::event

#endif  // BOOMPI_EVENT_SPSC_POD_QUEUE_H_
