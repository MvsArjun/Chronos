#pragma once
#include <chronos/core/types.hpp>
#include <chronos/concurrency/spsc_ring.hpp>
#include <atomic>
#include <exception>
#include <span>
#include <stop_token>
#include <thread>

namespace chronos::concurrency {
enum class Backpressure { Block, DropNewest, FailFast };
struct PipelineStats { std::size_t processed{}, dropped{}; };
// Caller owns source throughout run; handler runs only on caller's thread.
// Source must already be in replay order. Accepted events retain FIFO order.
template<std::size_t Capacity = 1024, class Handler>
PipelineStats run_pipeline(std::span<const Event> source, Handler&& handler,
                          Backpressure policy = Backpressure::Block,
                          std::stop_token stop = {}) {
    SpscRing<Event, Capacity> queue;
    std::atomic<bool> done{false}, cancelled{false};
    std::size_t dropped = 0;
    std::exception_ptr producer_error;
    std::jthread producer([&] {
        try {
            for (const auto& event : source) {
                if (stop.stop_requested() || cancelled.load(std::memory_order_relaxed)) break;
                while (!queue.try_push(event)) {
                    if (policy == Backpressure::DropNewest) { ++dropped; break; }
                    if (policy == Backpressure::FailFast) throw std::runtime_error("pipeline queue full");
                    if (stop.stop_requested() || cancelled.load(std::memory_order_relaxed)) {
                        done.store(true, std::memory_order_release); return;
                    }
                    std::this_thread::yield();
                }
            }
        } catch (...) { producer_error = std::current_exception(); }
        done.store(true, std::memory_order_release);
    });
    PipelineStats stats;
    try {
        for (;;) {
            if (auto event = queue.try_pop()) { handler(*event); ++stats.processed; }
            else if (done.load(std::memory_order_acquire)) {
                // Acquire done before rechecking: producer may have published after the first pop.
                if (auto last = queue.try_pop()) { handler(*last); ++stats.processed; }
                else break;
            } else std::this_thread::yield();
        }
    } catch (...) { cancelled.store(true, std::memory_order_relaxed); producer.join(); throw; }
    producer.join();
    if (producer_error) std::rethrow_exception(producer_error);
    stats.dropped = dropped;
    return stats;
}
} // namespace chronos::concurrency
