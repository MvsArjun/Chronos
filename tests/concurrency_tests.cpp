#include <gtest/gtest.h>
#include <chronos/concurrency/pipeline.hpp>
#include <chrono>
#include <memory>
#include <vector>

using namespace chronos;
using namespace chronos::concurrency;
TEST(Core, CheckedArithmetic) {
    EXPECT_EQ(checked_mul(-5, -6), 30);
    EXPECT_EQ(checked_add(-5, 6), 1);
    EXPECT_THROW(checked_add(INT64_MAX, 1), std::overflow_error);
    EXPECT_THROW(checked_add(INT64_MIN, -1), std::overflow_error);
    EXPECT_THROW(checked_mul(INT64_MIN, -1), std::overflow_error);
    EXPECT_THROW(checked_mul(INT64_MIN, 2), std::overflow_error);
    EXPECT_THROW(checked_mul(INT64_MAX, 2), std::overflow_error);
    EXPECT_THROW(checked_mul(-2, INT64_MAX), std::overflow_error);
}
TEST(Ring, EmptyFullWrapAndMoveOwnership) {
    SpscRing<std::unique_ptr<int>, 4> q;
    EXPECT_FALSE(q.try_pop());
    for (int cycle = 0; cycle < 100; ++cycle) {
        for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(std::make_unique<int>(i)));
        auto rejected = std::make_unique<int>(9);
        EXPECT_FALSE(q.try_push(std::move(rejected)));
        EXPECT_NE(rejected, nullptr);
        for (int i = 0; i < 4; ++i) { auto p = q.try_pop(); ASSERT_TRUE(p); EXPECT_EQ(**p, i); }
        EXPECT_FALSE(q.try_pop());
    }
}
TEST(Ring, NonemptyDestructionReleasesObjects) {
    auto owner = std::make_shared<int>(7);
    { SpscRing<std::shared_ptr<int>, 2> q; q.try_push(owner); EXPECT_EQ(owner.use_count(), 2); }
    EXPECT_EQ(owner.use_count(), 1);
}
TEST(Ring, ConcurrentFifoStress) {
    SpscRing<std::uint64_t, 256> q;
    constexpr std::uint64_t count = 500'000;
    std::jthread producer([&] { for (std::uint64_t i = 0; i < count; ++i) while (!q.try_push(i)) std::this_thread::yield(); });
    for (std::uint64_t i = 0; i < count; ++i) {
        std::optional<std::uint64_t> value;
        while (!(value = q.try_pop())) std::this_thread::yield();
        EXPECT_EQ(*value, i);
    }
    producer.join();
    EXPECT_FALSE(q.try_pop());
}
static std::vector<Event> events(std::size_t count) {
    std::vector<Event> v(count);
    for (std::size_t i = 0; i < count; ++i) v[i] = {{static_cast<Timestamp>(i), static_cast<Timestamp>(i), static_cast<Timestamp>(i), i+1, 1}, Trade{100, 1}};
    return v;
}
TEST(Pipeline, BlockingAndFifo) {
    auto input = events(50'000); std::uint64_t expected = 1;
    auto stats = run_pipeline<16>(input, [&](const Event& e) { EXPECT_EQ(e.header.sequence, expected++); });
    EXPECT_EQ(stats.processed, input.size()); EXPECT_EQ(stats.dropped, 0);
}
TEST(Pipeline, DropSaturationMaintainsSubsequence) {
    auto input = events(5000); std::uint64_t last = 0;
    auto stats = run_pipeline<2>(input, [&](const Event& e) {
        EXPECT_GT(e.header.sequence, last); last = e.header.sequence;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }, Backpressure::DropNewest);
    EXPECT_EQ(stats.processed + stats.dropped, input.size());
    EXPECT_GT(stats.dropped, 0);
}
TEST(Pipeline, FailFastAndHandlerExceptionsJoinProducer) {
    auto input = events(10'000);
    EXPECT_THROW((run_pipeline<2>(input, [](const Event&) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }, Backpressure::FailFast)), std::runtime_error);
    EXPECT_THROW((run_pipeline<2>(input, [](const Event&) { throw std::logic_error("handler failed"); })), std::logic_error);
}
TEST(Pipeline, ShutdownDuringActivityDrainsAcceptedEvents) {
    auto input = events(50'000); std::stop_source stop; std::uint64_t expected = 1;
    auto stats = run_pipeline<4>(input, [&](const Event& e) {
        EXPECT_EQ(e.header.sequence, expected++);
        if (expected == 100) stop.request_stop();
    }, Backpressure::Block, stop.get_token());
    EXPECT_GE(stats.processed, 99); EXPECT_LT(stats.processed, input.size());
}
