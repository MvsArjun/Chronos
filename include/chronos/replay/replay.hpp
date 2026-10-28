#pragma once
#include "chronos/core/types.hpp"
#include <algorithm>
#include <functional>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace chronos::replay {
inline auto event_key(const Event& event) {
    return std::tie(event.header.available_time, event.header.event_time,
                    event.header.sequence);
}
inline std::vector<Event> ordered_events(std::vector<Event> events) {
    std::unordered_set<std::uint64_t> sequences;
    sequences.reserve(events.size());
    for (const auto& event : events) {
        validate_event(event);
        if (!sequences.insert(event.header.sequence).second)
            throw std::invalid_argument("duplicate global event sequence");
    }
    std::sort(events.begin(), events.end(), [](const Event& left, const Event& right) {
        return event_key(left) < event_key(right);
    });
    return events;
}
class Replay {
public:
    explicit Replay(std::vector<Event> events) : events_(ordered_events(std::move(events))) {}
    template<class Handler> void run(Handler&& handler) {
        now_ = 0;
        for (const auto& event : events_) {
            now_ = event.header.available_time;
            std::invoke(handler, event);
        }
    }
    [[nodiscard]] Timestamp now() const noexcept { return now_; }
    [[nodiscard]] const std::vector<Event>& events() const noexcept { return events_; }
private:
    std::vector<Event> events_;
    Timestamp now_{};
};
} // namespace chronos::replay
