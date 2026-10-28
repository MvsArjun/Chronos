#include "chronos/market_data/data.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <fstream>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>

namespace chronos::market_data {
namespace {
constexpr std::string_view csv_header = "type,event_time,receive_time,available_time,sequence,instrument,p1,p2,p3,p4,quantity,quantity2,aux";
constexpr std::array<std::string_view, 6> type_names = {"trade", "quote", "book", "bar", "dividend", "split"};
constexpr std::uint32_t record_size = 89;
constexpr std::size_t max_metadata = 65'536;
struct Fields {
    std::uint8_t type{};
    std::int64_t p1{}, p2{}, p3{}, p4{}, quantity{}, quantity2{};
    std::uint32_t aux{};
    bool operator==(const Fields&) const = default;
};
Fields flatten(const Payload& payload) {
    Fields fields;
    fields.type = static_cast<std::uint8_t>(payload.index());
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Trade>) { fields.p1 = value.price; fields.quantity = value.quantity; }
        if constexpr (std::is_same_v<T, Quote>) { fields.p1 = value.bid; fields.p2 = value.ask; fields.quantity = value.bid_size; fields.quantity2 = value.ask_size; }
        if constexpr (std::is_same_v<T, BookUpdate>) { fields.p1 = value.price; fields.p2 = static_cast<std::int64_t>(value.side); fields.quantity = value.quantity; fields.aux = value.level; }
        if constexpr (std::is_same_v<T, Bar>) { fields.p1 = value.open; fields.p2 = value.high; fields.p3 = value.low; fields.p4 = value.close; fields.quantity = value.volume; }
        if constexpr (std::is_same_v<T, Dividend>) fields.p1 = value.amount_per_share;
        if constexpr (std::is_same_v<T, Split>) { fields.p1 = value.numerator; fields.p2 = value.denominator; }
    }, payload);
    return fields;
}
Payload expand(const Fields& fields) {
    Payload result;
    switch (fields.type) {
    case 0: result = Trade{fields.p1, fields.quantity}; break;
    case 1: result = Quote{fields.p1, fields.p2, fields.quantity, fields.quantity2}; break;
    case 2:
        if (fields.p2 != 1 && fields.p2 != 2) throw std::invalid_argument("invalid book side");
        result = BookUpdate{static_cast<Side>(fields.p2), fields.p1, fields.quantity, fields.aux}; break;
    case 3: result = Bar{fields.p1, fields.p2, fields.p3, fields.p4, fields.quantity}; break;
    case 4: result = Dividend{fields.p1}; break;
    case 5: result = Split{fields.p1, fields.p2}; break;
    default: throw std::invalid_argument("unknown event type");
    }
    if (!(flatten(result) == fields)) throw std::invalid_argument("nonzero inactive event fields");
    return result;
}
template<class T> T integer(std::string_view text) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || text.empty())
        throw std::invalid_argument("malformed integer field");
    return value;
}
bool bounded_line(std::istream& input, std::string& line) {
    // At most 1024 content bytes, an optional CR, and the terminating NUL.
    std::array<char, 1026> buffer{};
    input.getline(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (input.bad()) throw std::runtime_error("CSV read failed");
    if (input.fail() && !input.eof()) throw std::invalid_argument("CSV line too long");
    if (count == 0 && input.eof()) return false;
    const auto length = static_cast<std::size_t>(count - (input.eof() ? 0 : 1));
    line.assign(buffer.data(), length);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() > 1024) throw std::invalid_argument("CSV line too long");
    return true;
}
std::array<std::string_view, 13> columns(const std::string& line) {
    std::array<std::string_view, 13> result;
    std::size_t start = 0;
    for (std::size_t column = 0; column < result.size(); ++column) {
        const auto comma = line.find(',', start);
        if (column + 1 == result.size()) {
            if (comma != std::string::npos) throw std::invalid_argument("too many CSV columns");
            result[column] = std::string_view(line).substr(start);
        } else {
            if (comma == std::string::npos) throw std::invalid_argument("too few CSV columns");
            result[column] = std::string_view(line).substr(start, comma - start);
            start = comma + 1;
        }
    }
    return result;
}
class Crc32 {
public:
    void update(std::span<const char> bytes) {
#ifdef CHRONOS_CRC_BITWISE_BASELINE
        for (const char byte : bytes) {
            crc_ ^= static_cast<std::uint8_t>(byte);
            for (int bit = 0; bit < 8; ++bit)
                crc_ = (crc_ >> 1) ^ (0xedb88320U & (0U - (crc_ & 1U)));
        }
#else
        for (const char byte : bytes)
            crc_ = (crc_ >> 8) ^ table_[(crc_ ^ static_cast<std::uint8_t>(byte)) & 0xffU];
#endif
    }
    std::uint32_t value() const { return ~crc_; }
private:
    // The same reflected IEEE polynomial as the bitwise reference. The table is
    // built at compile time and consumes 1 KiB; update performs one lookup/byte.
    static constexpr auto table_ = [] {
        std::array<std::uint32_t, 256> table{};
        for (std::size_t index = 0; index < table.size(); ++index) {
            auto remainder = static_cast<std::uint32_t>(index);
            for (int bit = 0; bit < 8; ++bit)
                remainder = (remainder >> 1) ^ (0xedb88320U & (0U - (remainder & 1U)));
            table[index] = remainder;
        }
        return table;
    }();
    std::uint32_t crc_ = 0xffffffffU;
};
void put(std::span<char> bytes, std::size_t& offset, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) bytes[offset++] = static_cast<char>((value >> (8 * i)) & 0xffU);
}
std::uint64_t get(std::span<const char> bytes, std::size_t& offset, std::size_t width) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(bytes[offset++])) << (8 * i);
    return value;
}
void put_signed(std::span<char> bytes, std::size_t& offset, std::int64_t value) {
    put(bytes, offset, std::bit_cast<std::uint64_t>(value), 8);
}
std::int64_t get_signed(std::span<const char> bytes, std::size_t& offset) {
    return std::bit_cast<std::int64_t>(get(bytes, offset, 8));
}
void read_exact(std::istream& input, std::span<char> bytes) {
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input) throw std::runtime_error("truncated Chronos dataset");
}
void write_exact(std::ostream& output, std::span<const char> bytes) {
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("dataset write failed");
}
std::array<char, record_size> encode(const Event& event) {
    validate_event(event);
    std::array<char, record_size> bytes{};
    std::size_t offset = 0;
    put_signed(bytes, offset, event.header.event_time);
    put_signed(bytes, offset, event.header.receive_time);
    put_signed(bytes, offset, event.header.available_time);
    put(bytes, offset, event.header.sequence, 8);
    put(bytes, offset, event.header.instrument, 4);
    const auto fields = flatten(event.payload);
    put(bytes, offset, fields.type, 1);
    for (const auto value : {fields.p1, fields.p2, fields.p3, fields.p4, fields.quantity, fields.quantity2}) put_signed(bytes, offset, value);
    put(bytes, offset, fields.aux, 4);
    return bytes;
}
Event decode(std::span<const char> bytes) {
    std::size_t offset = 0;
    Event event;
    event.header.event_time = get_signed(bytes, offset);
    event.header.receive_time = get_signed(bytes, offset);
    event.header.available_time = get_signed(bytes, offset);
    event.header.sequence = get(bytes, offset, 8);
    event.header.instrument = static_cast<InstrumentId>(get(bytes, offset, 4));
    Fields fields;
    fields.type = static_cast<std::uint8_t>(get(bytes, offset, 1));
    fields.p1 = get_signed(bytes, offset); fields.p2 = get_signed(bytes, offset);
    fields.p3 = get_signed(bytes, offset); fields.p4 = get_signed(bytes, offset);
    fields.quantity = get_signed(bytes, offset); fields.quantity2 = get_signed(bytes, offset);
    fields.aux = static_cast<std::uint32_t>(get(bytes, offset, 4));
    event.payload = expand(fields);
    validate_event(event);
    return event;
}
std::uint64_t random_next(std::uint64_t& state) {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    return state;
}
void check_synthetic(std::size_t count, std::uint64_t seed, InstrumentId instrument) {
    if (count > default_max_events || seed == 0 || instrument == 0)
        throw std::invalid_argument("synthetic count exceeds cap, or zero seed/instrument");
}
} // namespace

std::vector<Event> load_csv(const std::filesystem::path& path, std::size_t max_events) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open CSV dataset");
    std::string line;
    if (!bounded_line(input, line)) throw std::invalid_argument("missing CSV header");
    if (line != csv_header) throw std::invalid_argument("unsupported CSV schema");
    std::vector<Event> events;
    std::size_t line_number = 1;
    while (bounded_line(input, line)) {
        ++line_number;
        if (events.size() >= std::min(max_events, default_max_events)) throw std::length_error("CSV event count exceeds cap");
        try {
            const auto field = columns(line);
            const auto type = std::find(type_names.begin(), type_names.end(), field[0]);
            if (type == type_names.end()) throw std::invalid_argument("unknown event type");
            Event event;
            event.header = {integer<Timestamp>(field[1]), integer<Timestamp>(field[2]), integer<Timestamp>(field[3]), integer<std::uint64_t>(field[4]), integer<InstrumentId>(field[5])};
            const Fields values{static_cast<std::uint8_t>(type - type_names.begin()), integer<std::int64_t>(field[6]), integer<std::int64_t>(field[7]), integer<std::int64_t>(field[8]), integer<std::int64_t>(field[9]), integer<Quantity>(field[10]), integer<Quantity>(field[11]), integer<std::uint32_t>(field[12])};
            event.payload = expand(values);
            validate_event(event);
            events.push_back(event);
        } catch (const std::exception& error) {
            throw std::invalid_argument("CSV line " + std::to_string(line_number) + ": " + error.what());
        }
    }
    if (!input.eof()) throw std::runtime_error("CSV read failed");
    return events;
}
void save_csv(const std::filesystem::path& path, std::span<const Event> events) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create CSV dataset");
    output.imbue(std::locale::classic());
    output << csv_header << '\n';
    for (const auto& event : events) {
        validate_event(event);
        const auto fields = flatten(event.payload);
        output << type_names.at(fields.type) << ',' << event.header.event_time << ',' << event.header.receive_time << ',' << event.header.available_time << ',' << event.header.sequence << ',' << event.header.instrument << ',' << fields.p1 << ',' << fields.p2 << ',' << fields.p3 << ',' << fields.p4 << ',' << fields.quantity << ',' << fields.quantity2 << ',' << fields.aux << '\n';
    }
    output.flush();
    if (!output) throw std::runtime_error("CSV write failed");
}
void save_binary(const std::filesystem::path& path, std::span<const Event> events, const std::string& metadata) {
    if (metadata.size() > max_metadata) throw std::length_error("dataset metadata exceeds cap");
    if (events.size() > default_max_events) throw std::length_error("dataset event count exceeds cap");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create binary dataset");
    std::array<char, 28> header{'C','H','R','O','N','O','S','\0'};
    std::size_t offset = 8;
    put(header, offset, schema_version, 2);
    put(header, offset, 0x454c, 2);
    put(header, offset, record_size, 4);
    put(header, offset, metadata.size(), 4);
    put(header, offset, events.size(), 8);
    Crc32 crc;
    const auto write_checked = [&](std::span<const char> bytes) { write_exact(output, bytes); crc.update(bytes); };
    write_checked(header);
    write_checked(std::span<const char>(metadata.data(), metadata.size()));
    for (const auto& event : events) write_checked(encode(event));
    std::array<char, 4> checksum{}; offset = 0;
    put(checksum, offset, crc.value(), 4);
    write_exact(output, checksum);
    output.flush();
    if (!output) throw std::runtime_error("binary dataset flush failed");
}
Dataset load_binary(const std::filesystem::path& path, std::size_t max_events) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open binary dataset");
    Crc32 crc;
    const auto read_checked = [&](std::span<char> bytes) { read_exact(input, bytes); crc.update(bytes); };
    std::array<char, 28> header{};
    read_checked(header);
    if (std::string_view(header.data(), 8) != std::string_view("CHRONOS\0", 8)) throw std::invalid_argument("invalid dataset magic");
    std::size_t offset = 8;
    if (get(header, offset, 2) != schema_version) throw std::invalid_argument("unsupported dataset version");
    if (get(header, offset, 2) != 0x454c) throw std::invalid_argument("unsupported dataset endianness");
    if (get(header, offset, 4) != record_size) throw std::invalid_argument("unsupported dataset record size");
    const auto metadata_size = get(header, offset, 4);
    const auto count = get(header, offset, 8);
    if (metadata_size > max_metadata || count > std::min(max_events, default_max_events) || count > std::numeric_limits<std::size_t>::max()) throw std::length_error("dataset resource cap exceeded");
    // Check the physical size before allocating from untrusted count fields.
    const auto expected_size = 28 + metadata_size + count * record_size + 4;
    input.seekg(0, std::ios::end);
    const auto physical_size = input.tellg();
    if (physical_size < 0 || static_cast<std::uint64_t>(physical_size) != expected_size) throw std::runtime_error("dataset length mismatch");
    input.seekg(28, std::ios::beg);
    Dataset dataset;
    dataset.metadata.resize(static_cast<std::size_t>(metadata_size));
    read_checked(std::span<char>(dataset.metadata.data(), dataset.metadata.size()));
    dataset.events.reserve(static_cast<std::size_t>(count));
    std::array<char, record_size> bytes{};
    for (std::uint64_t i = 0; i < count; ++i) { read_checked(bytes); dataset.events.push_back(decode(bytes)); }
    std::array<char, 4> checksum{};
    read_exact(input, checksum); offset = 0;
    if (get(checksum, offset, 4) != crc.value()) throw std::runtime_error("dataset CRC32 mismatch");
    if (input.peek() != std::char_traits<char>::eof()) throw std::runtime_error("trailing dataset bytes");
    return dataset;
}
std::vector<Event> synthetic_trades(std::size_t count, std::uint64_t seed, InstrumentId instrument) {
    check_synthetic(count, seed, instrument);
    std::vector<Event> events;
    events.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto time = 1'700'000'000'000'000'000LL + static_cast<Timestamp>(i) * 1'000'000;
        const auto price = 1'000'000 + static_cast<Price>(random_next(seed) % 2001) - 1000;
        const auto quantity = 1 + static_cast<Quantity>(random_next(seed) % 100);
        events.push_back(Event{{time, time + 100, time + 100, static_cast<std::uint64_t>(i) + 1, instrument}, Trade{price, quantity}});
    }
    return events;
}
std::vector<Event> synthetic_bars(std::size_t count, std::uint64_t seed, InstrumentId instrument) {
    check_synthetic(count, seed, instrument);
    std::vector<Event> events;
    events.reserve(count);
    Price previous = 1'000'000;
    for (std::size_t i = 0; i < count; ++i) {
        const auto time = 1'700'000'000'000'000'000LL + static_cast<Timestamp>(i) * 60'000'000'000LL;
        const auto close = 1'000'000 + static_cast<Price>(random_next(seed) % 20'001) - 10'000;
        events.push_back(Event{{time, time, time, static_cast<std::uint64_t>(i) + 1, instrument}, Bar{previous, std::max(previous, close) + 100, std::min(previous, close) - 100, close, 1000}});
        previous = close;
    }
    return events;
}
} // namespace chronos::market_data
