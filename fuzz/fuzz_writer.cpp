#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include <nanofix.hpp>

namespace {

// Little-endian pulls from the fuzz input; zero once the input runs dry.
struct puller {
    std::uint8_t const* p;
    std::size_t n;
    std::size_t i = 0;

    template <typename T>
    T pull() noexcept {
        T v{};
        if (i + sizeof(T) <= n) {
            std::memcpy(&v, p + i, sizeof(T));
            i += sizeof(T);
        }
        return v;
    }
};

// Drive every validating writer with arbitrary values: each call either emits
// a well-formed field or sets the sticky error — a writer-accepted message
// must then frame valid with a matching checksum.
void exercise_numeric_writers(std::uint8_t const* data, std::size_t size) noexcept {
    using namespace std::chrono;
    puller in{data, size};
    char buf[512];
    nanofix::message_writer w(buf, sizeof(buf));
    w.push_back_header("FIX.4.2");
    w.push_back_string(nanofix::tag::MsgType, "0");

    w.push_back_int(in.pull<int>(), in.pull<std::int64_t>());  // arbitrary tag incl. <= 0
    w.push_back_decimal(nanofix::tag::Price, in.pull<std::int64_t>(), in.pull<std::int64_t>());
    w.push_back_char(nanofix::tag::Side, static_cast<char>(in.pull<std::uint8_t>()));
    w.push_back_date(nanofix::tag::MaturityDate, in.pull<int>(), in.pull<int>(), in.pull<int>());
    w.push_back_monthyear(nanofix::tag::MaturityMonthYear, in.pull<int>(), in.pull<int>());
    w.push_back_timeonly(
        nanofix::tag::MDEntryTime, in.pull<int>(), in.pull<int>(), in.pull<int>(), in.pull<int>());
    w.push_back_timeonly_nano(
        nanofix::tag::MDEntryTime, in.pull<int>(), in.pull<int>(), in.pull<int>(), in.pull<int>());
    w.push_back_timestamp(nanofix::tag::SendingTime,
                          in.pull<int>(),
                          in.pull<int>(),
                          in.pull<int>(),
                          in.pull<int>(),
                          in.pull<int>(),
                          in.pull<int>(),
                          in.pull<int>());
    w.push_back_timestamp_epoch_millis(nanofix::tag::SendingTime, in.pull<std::int64_t>());
    w.push_back_timestamp_epoch_nanos(nanofix::tag::SendingTime, in.pull<std::int64_t>());
    w.push_back_timestamp(nanofix::tag::SendingTime,
                          sys_time<seconds>{seconds{in.pull<std::int64_t>()}});
    w.push_back_timestamp_nano(nanofix::tag::SendingTime,
                               sys_time<milliseconds>{milliseconds{in.pull<std::int64_t>()}});

    if (!w.push_back_trailer())
        return;
    nanofix::message_reader r(w);
    if (!r.is_complete() || !r.is_valid())
        std::abort();
    unsigned char wire_sum = 0;
    if (!r.check_sum()->value().try_as_int(wire_sum) || wire_sum != r.calculate_check_sum())
        std::abort();
}

// Differential oracle: parse -> re-serialize the field stream -> re-parse.
// Frames the reader accepted must survive a write round-trip byte-for-byte at
// the (tag, value) level.
void round_trip(std::span<char const> wire) noexcept {
    nanofix::message_reader r(wire);
    if (!r.is_complete() || !r.is_valid())
        return;
    for (auto it = r.begin(); it != r.end(); ++it) {
        // Round-trip holds only for canonical `tag=value` fields: wrapped tags
        // (<= 0) can't be written back; values with SOH need their length-tag
        // pairing; a field yielded without `=` (bare `95\x01`) or a yielded
        // data-length tag reframes on the second parse.
        char const* vb = it->value().begin();
        if (it->tag() <= 0 || nanofix::detail::is_tag_a_data_length(it->tag()) || vb == wire.data() ||
            vb[-1] != '=' || std::memchr(vb, '\x01', it->value().size()) != nullptr)
            return;
    }

    std::vector<char> out(wire.size() + 64);
    nanofix::message_writer w(out.data(), out.size());
    w.push_back_header(std::string_view(r.prefix_begin(), r.prefix_size()));
    for (auto it = r.begin(); it != r.end(); ++it)
        w.push_back_string(it->tag(), it->value().as_string_view());
    if (!w.push_back_trailer())
        std::abort();  // ample buffer: a parsed frame must re-serialize

    nanofix::message_reader r2(w);
    if (!r2.is_complete() || !r2.is_valid())
        std::abort();
    auto it2 = r2.begin();
    for (auto it = r.begin(); it != r.end(); ++it, ++it2) {
        if (it2 == r2.end())
            std::abort();
        if (it->tag() != it2->tag())
            std::abort();
        if (it->value().as_string_view() != it2->value().as_string_view())
            std::abort();
    }
    if (it2 != r2.end())
        std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size) {
    round_trip(std::span<char const>(reinterpret_cast<char const*>(data), size));
    exercise_numeric_writers(data, size);
    return 0;
}
