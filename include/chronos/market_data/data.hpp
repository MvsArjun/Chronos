#pragma once
#include "chronos/core/types.hpp"
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace chronos::market_data {
inline constexpr std::size_t default_max_events = 10'000'000;
struct Dataset { std::string metadata; std::vector<Event> events; };
std::vector<Event> load_csv(const std::filesystem::path& path,
                            std::size_t max_events = default_max_events);
void save_csv(const std::filesystem::path& path, std::span<const Event> events);
Dataset load_binary(const std::filesystem::path& path,
                    std::size_t max_events = default_max_events);
void save_binary(const std::filesystem::path& path, std::span<const Event> events,
                 const std::string& metadata = "{}");
std::vector<Event> synthetic_trades(std::size_t count, std::uint64_t seed = 1,
                                    InstrumentId instrument = 1);
std::vector<Event> synthetic_bars(std::size_t count, std::uint64_t seed = 1,
                                  InstrumentId instrument = 1);
} // namespace chronos::market_data
