#include "chronos/market_data/data.hpp"
#include "chronos/replay/replay.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <locale>

namespace {
class TempFile {
public:
    TempFile() {
        static std::atomic<unsigned> counter{0};
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("chronos-data-" + std::to_string(now) + "-" + std::to_string(counter++));
    }
    ~TempFile() { std::error_code error; std::filesystem::remove(path, error); }
    std::filesystem::path path;
};
std::string bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void write_bytes(const std::filesystem::path& path, const std::string& content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
}
std::vector<chronos::Event> all_types() {
    using namespace chronos;
    return {{{1, 2, 3, 1, 7}, Trade{100'001, 10}},
            {{2, 3, 4, 2, 7}, Quote{100'000, 100'010, 9, 11}},
            {{3, 4, 5, 3, 7}, BookUpdate{Side::Sell, 100'010, 0, 2}},
            {{4, 5, 6, 4, 7}, Bar{100'000, 100'200, 99'900, 100'100, 20}},
            {{5, 6, 7, 5, 7}, Dividend{500}},
            {{6, 7, 8, 6, 7}, Split{3, 2}}};
}
constexpr auto header = "type,event_time,receive_time,available_time,sequence,instrument,p1,p2,p3,p4,quantity,quantity2,aux\n";
} // namespace

TEST(Data, CsvAllPayloadsCanonicalRoundTrip) {
    TempFile first, second;
    const auto events = all_types();
    chronos::market_data::save_csv(first.path, events);
    const auto loaded = chronos::market_data::load_csv(first.path);
    ASSERT_EQ(loaded.size(), 6U);
    EXPECT_EQ(std::get<chronos::Quote>(loaded[1].payload).ask_size, 11);
    EXPECT_EQ(std::get<chronos::BookUpdate>(loaded[2].payload).side, chronos::Side::Sell);
    EXPECT_EQ(std::get<chronos::Split>(loaded[5].payload).denominator, 2);
    chronos::market_data::save_csv(second.path, loaded);
    EXPECT_EQ(bytes(first.path), bytes(second.path));
}
TEST(Data, CsvSerializationIsIndependentOfGlobalNumericLocale) {
    struct Grouping : std::numpunct<char> {
        char do_thousands_sep() const override { return ','; }
        std::string do_grouping() const override { return "\3"; }
    };
    struct RestoreLocale {
        std::locale saved{std::locale()};
        ~RestoreLocale() { std::locale::global(saved); }
    } restore;
    std::locale::global(std::locale(std::locale::classic(), new Grouping));
    TempFile file;
    chronos::market_data::save_csv(file.path, all_types());
    EXPECT_EQ(chronos::market_data::load_csv(file.path).size(), 6U);
}
TEST(Data, CsvRejectsInvalidAndAmbiguousRows) {
    TempFile file;
    for (const auto row : {"trade,1,1,1,1,1,100,0,0,0,1,0,0,extra\n",
                           "trade,1,1,1,1,1,100,0,0,0,1,0\n",
                           "trade,1,1,1,-1,1,100,0,0,0,1,0,0\n",
                           "trade,1,1,1,1,1,100x,0,0,0,1,0,0\n",
                           "trade,1,1,1,1,1,100,1,0,0,1,0,0\n",
                           "trade,1,1,1,1,1,100,0,0,0,0,0,0\n",
                           "trade,1,0,1,1,1,100,0,0,0,1,0,0\n",
                           "bar,1,1,1,1,1,100,90,80,100,1,0,0\n",
                           "unknown,1,1,1,1,1,100,0,0,0,1,0,0\n",
                           "\n"}) {
        write_bytes(file.path, std::string(header) + row);
        EXPECT_THROW(chronos::market_data::load_csv(file.path), std::invalid_argument) << row;
    }
}
TEST(Data, ResourceLimitsAndEmptyDatasets) {
    TempFile file;
    chronos::market_data::save_csv(file.path, all_types());
    EXPECT_THROW(chronos::market_data::load_csv(file.path, 1), std::length_error);
    chronos::market_data::save_binary(file.path, all_types());
    EXPECT_THROW(chronos::market_data::load_binary(file.path, 1), std::length_error);
    EXPECT_THROW(chronos::market_data::save_binary(file.path, {}, std::string(65'537, 'x')), std::length_error);
    chronos::market_data::save_binary(file.path, {}, "empty");
    EXPECT_TRUE(chronos::market_data::load_binary(file.path).events.empty());
    chronos::market_data::save_csv(file.path, {});
    EXPECT_TRUE(chronos::market_data::load_csv(file.path).empty());
}
TEST(Data, CsvLineEndingsFinalRowAndBoundedInput) {
    TempFile file;
    const std::string row = "trade,1,1,1,1,1,100,0,0,0,1,0,0";
    write_bytes(file.path, std::string(header) + row);
    EXPECT_EQ(chronos::market_data::load_csv(file.path).size(), 1U);
    auto crlf_header = std::string(header); crlf_header.pop_back(); crlf_header += "\r\n";
    write_bytes(file.path, crlf_header + row + "\r\n");
    EXPECT_EQ(chronos::market_data::load_csv(file.path).size(), 1U);
    write_bytes(file.path, std::string(header) + std::string(100'000, 'x'));
    EXPECT_THROW(chronos::market_data::load_csv(file.path), std::invalid_argument);
}
TEST(Data, BinaryDeterministicExplicitEndianAndRoundTrip) {
    TempFile file, second;
    chronos::market_data::save_binary(file.path, all_types(), "fixture-v1");
    const auto saved = bytes(file.path);
    ASSERT_EQ(saved.size(), 28U + 10U + 6U * 89U + 4U);
    EXPECT_EQ(saved.substr(0, 8), std::string("CHRONOS\0", 8));
    EXPECT_EQ(static_cast<unsigned char>(saved[8]), 1U);
    EXPECT_EQ(static_cast<unsigned char>(saved[9]), 0U);
    EXPECT_EQ(saved.substr(10, 2), "LE");
    EXPECT_EQ(static_cast<unsigned char>(saved[12]), 89U);
    EXPECT_EQ(static_cast<unsigned char>(saved[20]), 6U);
    EXPECT_EQ(static_cast<unsigned char>(saved[38]), 1U); // first timestamp
    const auto loaded = chronos::market_data::load_binary(file.path);
    EXPECT_EQ(loaded.metadata, "fixture-v1");
    ASSERT_EQ(loaded.events.size(), 6U);
    chronos::market_data::save_binary(second.path, loaded.events, loaded.metadata);
    EXPECT_EQ(saved, bytes(second.path));
}
TEST(Data, BinaryRejectsCorruptionTruncationTrailingAndVersion) {
    TempFile file;
    chronos::market_data::save_binary(file.path, all_types(), "metadata");
    const auto good = bytes(file.path);
    auto bad = good; bad[28] ^= 1; write_bytes(file.path, bad);
    EXPECT_THROW(chronos::market_data::load_binary(file.path), std::runtime_error);
    bad = good.substr(0, good.size() - 1); write_bytes(file.path, bad);
    EXPECT_THROW(chronos::market_data::load_binary(file.path), std::runtime_error);
    bad = good + "x"; write_bytes(file.path, bad);
    EXPECT_THROW(chronos::market_data::load_binary(file.path), std::runtime_error);
    bad = good; bad[8] = 2; write_bytes(file.path, bad);
    EXPECT_THROW(chronos::market_data::load_binary(file.path), std::invalid_argument);
    bad = good; bad[12] = 88; write_bytes(file.path, bad);
    EXPECT_THROW(chronos::market_data::load_binary(file.path), std::invalid_argument);
    bad = good; bad[20] = static_cast<char>(0xff); bad[27] = static_cast<char>(0xff); write_bytes(file.path, bad);
    EXPECT_THROW(chronos::market_data::load_binary(file.path), std::length_error);
}
TEST(Data, SyntheticFixturesAreDeterministicAndValidated) {
    const auto one = chronos::market_data::synthetic_trades(1000, 42);
    const auto two = chronos::market_data::synthetic_trades(1000, 42);
    ASSERT_EQ(one.size(), two.size());
    for (std::size_t i = 0; i < one.size(); ++i) {
        chronos::validate_event(one[i]);
        EXPECT_EQ(std::get<chronos::Trade>(one[i].payload).price, std::get<chronos::Trade>(two[i].payload).price);
        EXPECT_EQ(one[i].header.sequence, i + 1);
    }
    for (const auto& event : chronos::market_data::synthetic_bars(1000)) EXPECT_NO_THROW(chronos::validate_event(event));
    EXPECT_THROW(chronos::market_data::synthetic_trades(1, 0), std::invalid_argument);
}
TEST(Replay, AvailabilityOrderingTieBreakingAndClock) {
    auto events = chronos::market_data::synthetic_trades(4);
    for (auto& event : events) event.header = {10, 10, 20, event.header.sequence, 1};
    events[0].header.available_time = 30;
    events[1].header.event_time = 11;
    events[1].header.receive_time = 11;
    std::reverse(events.begin(), events.end());
    chronos::replay::Replay replay(events);
    std::vector<std::uint64_t> first, second;
    replay.run([&](const chronos::Event& event) {
        EXPECT_EQ(replay.now(), event.header.available_time);
        first.push_back(event.header.sequence);
    });
    replay.run([&](const chronos::Event& event) { second.push_back(event.header.sequence); });
    EXPECT_EQ(first, (std::vector<std::uint64_t>{3, 4, 2, 1}));
    EXPECT_EQ(first, second);
    EXPECT_EQ(replay.now(), 30);
}
TEST(Replay, DuplicateSequencesAndInvalidHeadersRejected) {
    auto events = chronos::market_data::synthetic_trades(2);
    events[1].header.sequence = 1;
    EXPECT_THROW(chronos::replay::ordered_events(events), std::invalid_argument);
    events[1].header.sequence = 2;
    events[1].header.available_time = 0;
    EXPECT_THROW(chronos::replay::ordered_events(events), std::invalid_argument);
}
