#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <utility>

namespace mirage {

// Lock-free single-producer single-consumer ring of N slots (N a power of two).
// One thread calls push, one other thread calls pop; neither allocates or blocks.
// T must be default-constructible and movable (e.g. std::unique_ptr<X>).
template <class T, size_t N>
class SpscQueue {
  static_assert(N >= 2 && (N & (N - 1)) == 0, "SpscQueue capacity must be a power of two");

 public:
  // False when full; `v` is then left untouched (a unique_ptr payload stays with the caller).
  bool push(T&& v) {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_.load(std::memory_order_acquire) == N) return false;
    slots_[tail & kMask] = std::move(v);
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }
  bool push(const T& v) {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_.load(std::memory_order_acquire) == N) return false;
    slots_[tail & kMask] = v;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Moves the oldest item into `out`; false when empty.
  bool pop(T& out) {
    const size_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_.load(std::memory_order_acquire)) return false;
    out = std::move(slots_[head & kMask]);
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Item count as seen from either thread; may be stale by the time it is read.
  size_t sizeApprox() const {
    const size_t head = head_.load(std::memory_order_acquire);
    return tail_.load(std::memory_order_acquire) - head;
  }

 private:
  static constexpr size_t kMask = N - 1;
  std::array<T, N> slots_{};
  // Free-running counters (slot = counter & kMask), so full is tail - head == N.
  std::atomic<size_t> head_{0};  // next slot to pop; written by the consumer
  std::atomic<size_t> tail_{0};  // next slot to push; written by the producer
};

}  // namespace mirage
