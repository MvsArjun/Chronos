#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace chronos {
using Timestamp = std::int64_t; // Unix nanoseconds, UTC; nonnegative
using Price = std::int64_t;     // 1/10,000 currency unit
using Money = std::int64_t;     // same scale as Price; one currency per run
using Quantity = std::int64_t;  // whole shares/contracts; no fractional lots
using InstrumentId = std::uint32_t;
inline constexpr std::int64_t price_scale = 10'000;
inline constexpr std::uint16_t schema_version = 1;

inline std::int64_t checked_add(std::int64_t a, std::int64_t b) {
    if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
        (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b))
        throw std::overflow_error("integer addition overflow");
    return a + b;
}
inline std::int64_t checked_mul(std::int64_t a, std::int64_t b) {
    if (a == 0 || b == 0) return 0;
    constexpr auto lo = std::numeric_limits<std::int64_t>::min();
    constexpr auto hi = std::numeric_limits<std::int64_t>::max();
    if ((a == -1 && b == lo) || (b == -1 && a == lo))
        throw std::overflow_error("integer multiplication overflow");
    if (a > 0 ? (b > 0 ? a > hi / b : b < lo / a)
              : (b > 0 ? a < lo / b : a < hi / b))
        throw std::overflow_error("integer multiplication overflow");
    return a * b;
}
inline std::int64_t checked_negate(std::int64_t a) { return checked_mul(a, -1); }

enum class Side : std::uint8_t { Buy = 1, Sell = 2 };
struct EventHeader {
    Timestamp event_time{};
    Timestamp receive_time{};
    Timestamp available_time{};
    std::uint64_t sequence{}; // globally unique per dataset
    InstrumentId instrument{}; // zero invalid
};
struct Trade { Price price{}; Quantity quantity{}; };
struct Quote { Price bid{}; Price ask{}; Quantity bid_size{}; Quantity ask_size{}; };
struct BookUpdate { Side side{Side::Buy}; Price price{}; Quantity quantity{}; std::uint32_t level{}; };
struct Bar { Price open{}; Price high{}; Price low{}; Price close{}; Quantity volume{}; };
struct Dividend { Price amount_per_share{}; }; // unadjusted data; ex-date event
struct Split { std::int64_t numerator{1}; std::int64_t denominator{1}; };
using Payload = std::variant<Trade, Quote, BookUpdate, Bar, Dividend, Split>;
struct Event { EventHeader header; Payload payload; };
enum class OrderType : std::uint8_t { Market = 1, Limit = 2 };
struct Signal { InstrumentId instrument{}; Timestamp available_time{}; double score{}; };
struct OrderRequest {
    std::uint64_t id{};
    InstrumentId instrument{};
    Side side{Side::Buy};
    OrderType type{OrderType::Market};
    Quantity quantity{};
    Price limit_price{};
};
enum class ExecutionStatus : std::uint8_t { Filled, Partial, Cancelled, Rejected };
struct ExecutionReport {
    std::uint64_t order_id{};
    InstrumentId instrument{};
    Timestamp time{};
    Side side{Side::Buy};
    Quantity quantity{};
    Price price{};
    Money fees{};
    ExecutionStatus status{ExecutionStatus::Filled};
};

inline void validate_event(const Event& e) {
    const auto& h = e.header;
    if (!h.instrument || !h.sequence || h.event_time < 0 ||
        h.receive_time < h.event_time || h.available_time < h.receive_time)
        throw std::invalid_argument("invalid event header");
    std::visit([](const auto& p) {
        using T = std::decay_t<decltype(p)>;
        bool ok = true;
        if constexpr (std::is_same_v<T, Trade>) ok = p.price > 0 && p.quantity > 0;
        if constexpr (std::is_same_v<T, Quote>) ok = p.bid > 0 && p.ask >= p.bid && p.bid_size >= 0 && p.ask_size >= 0;
        if constexpr (std::is_same_v<T, BookUpdate>) ok = (p.side == Side::Buy || p.side == Side::Sell) && p.price > 0 && p.quantity >= 0;
        if constexpr (std::is_same_v<T, Bar>) ok = p.low > 0 && p.high >= p.low && p.open >= p.low && p.open <= p.high && p.close >= p.low && p.close <= p.high && p.volume >= 0;
        if constexpr (std::is_same_v<T, Dividend>) ok = p.amount_per_share >= 0;
        if constexpr (std::is_same_v<T, Split>) ok = p.numerator > 0 && p.denominator > 0;
        if (!ok) throw std::invalid_argument("invalid event payload");
    }, e.payload);
}
} // namespace chronos
