#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

namespace chronos::concurrency {
// Exactly one producer and one consumer. Destruction requires both to have stopped.
// Queue operations do not allocate; T's own construction may do so.
template<class T, std::size_t Capacity> class SpscRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0);
    static_assert(Capacity < static_cast<std::size_t>(-1) / 2);
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "Chronos requires lock-free size_t atomics on this target");
    static_assert(std::is_nothrow_move_constructible_v<T>);
    struct alignas(64) Cursor { std::atomic<std::size_t> value{0}; };
    Cursor head_;
    Cursor tail_;
    std::array<std::optional<T>, Capacity> slots_{};
public:
    SpscRing() = default;
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    static constexpr std::size_t capacity() noexcept { return Capacity; }
    template<class U> bool try_push(U&& value) {
        const auto h = head_.value.load(std::memory_order_relaxed);
        if (h - tail_.value.load(std::memory_order_acquire) == Capacity) return false;
        slots_[h & (Capacity - 1)].emplace(std::forward<U>(value));
        head_.value.store(h + 1, std::memory_order_release);
        return true;
    }
    std::optional<T> try_pop() noexcept {
        const auto t = tail_.value.load(std::memory_order_relaxed);
        if (t == head_.value.load(std::memory_order_acquire)) return std::nullopt;
        auto result = std::move(slots_[t & (Capacity - 1)]);
        slots_[t & (Capacity - 1)].reset();
        tail_.value.store(t + 1, std::memory_order_release);
        return result;
    }
};
} // namespace chronos::concurrency
