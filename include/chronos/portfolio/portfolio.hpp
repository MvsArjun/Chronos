#pragma once
#include <chronos/core/types.hpp>
#include <algorithm>
#include <cmath>
#include <map>

namespace chronos {
namespace accounting_detail {
// Exact floor(a*b/d), without a wide integer extension. Here b <= d, so the
// result is at most a. Unsigned partial remainders stay below 2*INT64_MAX.
inline std::int64_t allocate(std::int64_t a, std::int64_t b, std::int64_t d) {
    if (a < 0 || b < 0 || d <= 0 || b > d) throw std::invalid_argument("invalid allocation");
    const auto ua = static_cast<std::uint64_t>(a);
    const auto ub = static_cast<std::uint64_t>(b);
    const auto ud = static_cast<std::uint64_t>(d);
    std::uint64_t quotient = 0, remainder = 0;
    for (int bit = 62; bit >= 0; --bit) {
        quotient *= 2;
        remainder *= 2;
        quotient += remainder / ud;
        remainder %= ud;
        if ((ub >> bit) & 1U) {
            quotient += ua / ud;
            remainder += ua % ud;
            quotient += remainder / ud;
            remainder %= ud;
        }
    }
    return static_cast<std::int64_t>(quotient);
}
inline Quantity magnitude(Quantity q) { return q >= 0 ? q : checked_negate(q); }
} // namespace accounting_detail

struct Position {
    Quantity quantity{};
    Money cost_basis{}; // nonnegative remaining opening notional, excludes fees
    Price last_price{};
    Money realized_pnl{};
    double average_cost() const {
        return quantity ? static_cast<double>(cost_basis) / static_cast<double>(accounting_detail::magnitude(quantity)) : 0.0;
    }
    Money unrealized_pnl() const {
        if (!quantity) return 0;
        const auto value = checked_mul(accounting_detail::magnitude(quantity), last_price);
        return quantity > 0 ? checked_add(value, checked_negate(cost_basis))
                            : checked_add(cost_basis, checked_negate(value));
    }
};

class Portfolio {
public:
    explicit Portfolio(Money initial_cash = 1'000'000 * price_scale)
        : initial_cash_(initial_cash), cash_(initial_cash) {
        if (initial_cash < 0) throw std::invalid_argument("negative initial cash");
    }
    Money initial_cash() const noexcept { return initial_cash_; }
    Money cash() const noexcept { return cash_; }
    Money available_cash() const noexcept { return cash_; }
    Money reserved_cash() const noexcept { return 0; }
    Money fees() const noexcept { return fees_; }
    Money income() const noexcept { return income_; }
    const std::map<InstrumentId, Position>& positions() const noexcept { return positions_; }
    Quantity quantity(InstrumentId id) const noexcept {
        const auto it = positions_.find(id);
        return it == positions_.end() ? 0 : it->second.quantity;
    }
    Money realized_pnl() const {
        Money result = 0;
        for (const auto& [id, position] : positions_) { (void)id; result = checked_add(result, position.realized_pnl); }
        return result;
    }
    Money unrealized_pnl() const {
        Money result = 0;
        for (const auto& [id, position] : positions_) { (void)id; result = checked_add(result, position.unrealized_pnl()); }
        return result;
    }
    Money net_exposure() const {
        Money result = 0;
        for (const auto& [id, position] : positions_) { (void)id; result = checked_add(result, checked_mul(position.quantity, position.last_price)); }
        return result;
    }
    Money gross_exposure() const {
        Money result = 0;
        for (const auto& [id, position] : positions_) { (void)id; result = checked_add(result, checked_mul(accounting_detail::magnitude(position.quantity), position.last_price)); }
        return result;
    }
    Money equity() const { return checked_add(cash_, net_exposure()); }
    bool invariant() const {
        Money expected = checked_add(initial_cash_, realized_pnl());
        expected = checked_add(expected, unrealized_pnl());
        expected = checked_add(expected, income_);
        expected = checked_add(expected, checked_negate(fees_));
        if (equity() != expected) return false;
        for (const auto& [id, p] : positions_) {
            if (!id || p.cost_basis < 0 || (p.quantity == 0 && p.cost_basis != 0) || (p.quantity && p.last_price <= 0)) return false;
        }
        return true;
    }
    void mark(InstrumentId id, Price price) {
        if (!id || price <= 0) throw std::invalid_argument("invalid mark");
        const auto it = positions_.find(id);
        if (it != positions_.end()) {
            (void)checked_mul(accounting_detail::magnitude(it->second.quantity), price);
            it->second.last_price = price;
        }
    }
    // Ledger updates are transactional: checked arithmetic finishes before mutation.
    void apply_fill(const ExecutionReport& fill) {
        if (!fill.instrument || !fill.order_id || fill.time < 0 || fill.quantity <= 0 || fill.price <= 0 || fill.fees < 0 ||
            (fill.side != Side::Buy && fill.side != Side::Sell) ||
            (fill.status != ExecutionStatus::Filled && fill.status != ExecutionStatus::Partial))
            throw std::invalid_argument("invalid fill");
        Position next;
        if (const auto it = positions_.find(fill.instrument); it != positions_.end()) next = it->second;
        const auto signed_quantity = fill.side == Side::Buy ? fill.quantity : checked_negate(fill.quantity);
        const auto notional = checked_mul(fill.quantity, fill.price);
        const auto cash_delta = fill.side == Side::Buy ? checked_negate(notional) : notional;
        const auto next_cash = checked_add(checked_add(cash_, cash_delta), checked_negate(fill.fees));
        const auto next_fees = checked_add(fees_, fill.fees);
        const bool increases = next.quantity == 0 || (next.quantity > 0) == (signed_quantity > 0);
        if (increases) {
            next.cost_basis = checked_add(next.cost_basis, notional);
        } else {
            const auto old_size = accounting_detail::magnitude(next.quantity);
            const auto closed = std::min(old_size, fill.quantity);
            const auto allocated = accounting_detail::allocate(next.cost_basis, closed, old_size);
            const auto closing_notional = checked_mul(closed, fill.price);
            const auto pnl = next.quantity > 0 ? checked_add(closing_notional, checked_negate(allocated))
                                               : checked_add(allocated, checked_negate(closing_notional));
            next.realized_pnl = checked_add(next.realized_pnl, pnl);
            next.cost_basis = checked_add(next.cost_basis, checked_negate(allocated));
            if (fill.quantity > old_size) next.cost_basis = checked_mul(fill.quantity - old_size, fill.price);
        }
        next.quantity = checked_add(next.quantity, signed_quantity);
        next.last_price = fill.price;
        positions_.insert_or_assign(fill.instrument, next);
        cash_ = next_cash;
        fees_ = next_fees;
    }
    void dividend(InstrumentId id, Price amount) {
        if (!id || amount < 0) throw std::invalid_argument("invalid dividend");
        const auto delta = checked_mul(quantity(id), amount);
        const auto next_cash = checked_add(cash_, delta);
        const auto next_income = checked_add(income_, delta);
        cash_ = next_cash;
        income_ = next_income;
    }
    void split(InstrumentId id, std::int64_t numerator, std::int64_t denominator) {
        if (!id || numerator <= 0 || denominator <= 0) throw std::invalid_argument("invalid split");
        const auto it = positions_.find(id);
        if (it == positions_.end() || !it->second.quantity) return;
        auto next = it->second;
        const auto quantity_numerator = checked_mul(next.quantity, numerator);
        const auto price_numerator = checked_mul(next.last_price, denominator);
        if (quantity_numerator % denominator || price_numerator % numerator)
            throw std::invalid_argument("fractional split quantity or mark unsupported");
        next.quantity = quantity_numerator / denominator;
        next.last_price = price_numerator / numerator;
        if (!next.quantity || next.last_price <= 0) throw std::invalid_argument("split rounds to zero");
        it->second = next;
    }
private:
    Money initial_cash_{};
    Money cash_{};
    Money fees_{};
    Money income_{};
    std::map<InstrumentId, Position> positions_;
};
} // namespace chronos
