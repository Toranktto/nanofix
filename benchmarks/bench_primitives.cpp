// SIMD primitives, timestamp parse, and the single-message read/scan paths.

#include <benchmark/benchmark.h>

#include <nanofix.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "bench_common.hpp"

using namespace nanofix_bench;

namespace {

constexpr std::size_t kBenchTimestampPool = 1024;

constexpr int kSingleMsgTags[] = {
    nanofix::tag::MsgType,
    nanofix::tag::SenderCompID,
    nanofix::tag::TargetCompID,
    nanofix::tag::MsgSeqNum,
    nanofix::tag::SendingTime,
    nanofix::tag::ClOrdID,
    nanofix::tag::HandlInst,
    nanofix::tag::Symbol,
    nanofix::tag::Side,
    nanofix::tag::OrderQty,
    nanofix::tag::OrdType,
    nanofix::tag::Price,
    nanofix::tag::TimeInForce,
    nanofix::tag::TransactTime,
    nanofix::tag::Account,
};

// Raw checksum_bytes() over a synthetic buffer of state.range(0) bytes.
// Bypasses the reader so it isolates the SIMD primitive itself.
void BM_ChecksumBytes(benchmark::State& state) {
    std::size_t const len = static_cast<std::size_t>(state.range(0));
    std::vector<char> buf(len);
    std::mt19937 rng(0xC1234567u);
    for (auto& c : buf)
        c = static_cast<char>(rng() & 0xFF);
    std::size_t total = 0;
    for (auto _ : state) {
        std::uint8_t cs = nanofix::detail::checksum_bytes(buf.data(), buf.data() + len);
        benchmark::DoNotOptimize(cs);
        total += len;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

// FIX-like byte buffer: SOH every 6-14 bytes, mimicking the field-length
// distribution, no SOH inside values.
std::vector<char> make_fixlike(std::size_t len) {
    std::vector<char> buf(len);
    std::mt19937 rng(0xBEEF'1234u);
    std::size_t next = 0;
    for (std::size_t i = 0; i < len; ++i) {
        if (i >= next) {
            buf[i] = '\x01';
            next = i + 6 + (rng() % 9);
        } else {
            char c;
            do {
                c = static_cast<char>(rng() & 0xFF);
            } while (c == '\x01');
            buf[i] = c;
        }
    }
    return buf;
}

// Framing: repeated per-field find_soh, walking every boundary.
void BM_FramingPerField(benchmark::State& state) {
    auto buf = make_fixlike(static_cast<std::size_t>(state.range(0)));
    char const* const b = buf.data();
    char const* const e = b + buf.size();
    std::size_t total = 0;
    for (auto _ : state) {
        char const* p = b;
        std::size_t count = 0;
        while (p < e) {
            p = nanofix::detail::find_soh(p, e);
            if (p < e) {
                ++count;
                ++p;
            }
        }
        benchmark::DoNotOptimize(count);
        total += buf.size();
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

// One bulk find_all_soh sweep, output sized to the whole buffer — so the
// capacity-exhaustion branch is never hit here (BM_FramingBulkCapped covers it).
void BM_FramingBulk(benchmark::State& state) {
    auto buf = make_fixlike(static_cast<std::size_t>(state.range(0)));
    std::vector<std::uint32_t> offs(buf.size());
    std::size_t total = 0;
    for (auto _ : state) {
        std::size_t count = nanofix::detail::find_all_soh(
            buf.data(), buf.data() + buf.size(), offs.data(), offs.size());
        benchmark::DoNotOptimize(count);
        total += buf.size();
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

// Bounded look-ahead refilled across the buffer — the shape build_field_index
// drives, exercising find_all_soh's cap/short-write branch. Whole buffer covered.
void BM_FramingBulkCapped(benchmark::State& state) {
    auto buf = make_fixlike(static_cast<std::size_t>(state.range(0)));
    constexpr std::size_t kCap = 256;  // matches build_field_index's window
    std::uint32_t offs[kCap];
    char const* const e = buf.data() + buf.size();
    std::size_t total = 0;
    for (auto _ : state) {
        std::size_t soh = 0;
        for (char const* p = buf.data(); p < e;) {
            std::size_t n = nanofix::detail::find_all_soh(p, e, offs, kCap);
            soh += n;
            if (n < kCap)
                break;             // reached the end inside this window
            p += offs[n - 1] + 1;  // refill past the last SOH found
        }
        benchmark::DoNotOptimize(soh);
        total += buf.size();
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

// find_tag_in_index() over an int[N] tag table. Worst case: target tag
// is the last entry, so the SIMD loop scans the whole table.
void BM_FindTagInIndex(benchmark::State& state) {
    std::size_t const n = static_cast<std::size_t>(state.range(0));
    std::vector<int> tags(n);
    for (std::size_t i = 0; i < n; ++i)
        tags[i] = static_cast<int>(1000 + i);
    int const target = tags.back();
    for (auto _ : state) {
        std::size_t r = nanofix::detail::find_tag_in_index(tags.data(), n, target);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(n));
}

// Pure checksum throughput on a large MD incremental refresh. Reader is built
// once outside the loop so each iteration measures only calculate_check_sum().
void BM_Checksum_LargeMDIncremental(benchmark::State& state) {
    static char buffer[1 << 16];
    auto tsend = live_timestamp();
    int const n_entries = static_cast<int>(state.range(0));
    std::size_t const len = write_md_incremental_large(buffer, sizeof(buffer), 1, tsend, n_entries);
    nanofix::message_reader r(buffer, buffer + len);
    if (!r.is_complete() || !r.is_valid()) {
        state.SkipWithError("could not construct test message");
        return;
    }
    std::size_t total = 0;
    for (auto _ : state) {
        unsigned char cs = r.calculate_check_sum();
        benchmark::DoNotOptimize(cs);
        total += len;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
    state.counters["msg_bytes"] = static_cast<double>(len);
}

void BM_ReadMessageScan(benchmark::State& state) {
    char buffer[kBufSize];
    auto tsend = live_timestamp();
    benchmark::DoNotOptimize(tsend);
    std::size_t len = write_new_order(buffer, sizeof(buffer), 1, tsend);
    std::size_t total = 0;
    for (auto _ : state) {
        nanofix::message_reader r(buffer, buffer + len);
        if (r.is_complete() && r.is_valid()) {
            for (auto it = r.begin(); it != r.end(); ++it) {
                benchmark::DoNotOptimize(it->tag());
            }
        }
        total += len;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

void BM_ReadMessageFindFields(benchmark::State& state) {
    char buffer[kBufSize];
    auto tsend = live_timestamp();
    benchmark::DoNotOptimize(tsend);
    std::size_t len = write_new_order(buffer, sizeof(buffer), 1, tsend);
    std::size_t total = 0;
    for (auto _ : state) {
        nanofix::message_reader r(buffer, buffer + len);
        if (r.is_complete() && r.is_valid()) {
            auto it = r.begin();
            if (r.find_with_hint(nanofix::tag::ClOrdID, it)) {
                auto v = it->value();
                benchmark::DoNotOptimize(v);
            }
            if (r.find_with_hint(nanofix::tag::Symbol, it)) {
                auto v = it->value();
                benchmark::DoNotOptimize(v);
            }
            if (r.find_with_hint(nanofix::tag::OrderQty, it)) {
                auto v = it->value().as_int_unchecked<int>();
                benchmark::DoNotOptimize(v);
            }
            if (r.find_with_hint(nanofix::tag::Price, it)) {
                auto v = it->value();
                benchmark::DoNotOptimize(v);
            }
        }
        total += len;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

void BM_RoundTrip(benchmark::State& state) {
    char buffer[kBufSize];
    auto tsend = live_timestamp();
    benchmark::DoNotOptimize(tsend);
    std::size_t total = 0;
    for (auto _ : state) {
        std::size_t len = write_new_order(buffer, sizeof(buffer), 1, tsend);
        nanofix::message_reader r(buffer, buffer + len);
        for (auto it = r.begin(); it != r.end(); ++it) {
            auto v = it->value();
            benchmark::DoNotOptimize(v);
        }
        total += len;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

std::vector<std::string> make_timestamp_pool_millis() {
    std::vector<std::string> out;
    out.reserve(kBenchTimestampPool);
    char buf[32];
    for (std::size_t i = 0; i < kBenchTimestampPool; ++i) {
        int year = 2000 + static_cast<int>((i * 13) % 99);
        int month = 1 + static_cast<int>((i * 7) % 12);
        int day = 1 + static_cast<int>((i * 11) % 28);
        int hour = static_cast<int>((i * 5) % 24);
        int minute = static_cast<int>((i * 17) % 60);
        int second = static_cast<int>((i * 31) % 60);
        int millis = static_cast<int>((i * 137) % 1000);
        std::snprintf(buf,
                      sizeof(buf),
                      "%04d%02d%02d-%02d:%02d:%02d.%03d",
                      year,
                      month,
                      day,
                      hour,
                      minute,
                      second,
                      millis);
        out.emplace_back(buf);
    }
    return out;
}

std::vector<std::string> make_timestamp_pool_nanos() {
    std::vector<std::string> out;
    out.reserve(kBenchTimestampPool);
    char buf[40];
    for (std::size_t i = 0; i < kBenchTimestampPool; ++i) {
        int year = 2000 + static_cast<int>((i * 13) % 99);
        int month = 1 + static_cast<int>((i * 7) % 12);
        int day = 1 + static_cast<int>((i * 11) % 28);
        int hour = static_cast<int>((i * 5) % 24);
        int minute = static_cast<int>((i * 17) % 60);
        int second = static_cast<int>((i * 31) % 60);
        std::uint64_t nanos = (static_cast<std::uint64_t>(i) * 0x9E3779B97F4A7C15ULL) % 1000000000ULL;
        std::snprintf(buf,
                      sizeof(buf),
                      "%04d%02d%02d-%02d:%02d:%02d.%09llu",
                      year,
                      month,
                      day,
                      hour,
                      minute,
                      second,
                      static_cast<unsigned long long>(nanos));
        out.emplace_back(buf);
    }
    return out;
}

void BM_ParseTimestampMillis(benchmark::State& state) {
    static auto const pool = make_timestamp_pool_millis();
    using TimePoint = std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds>;
    std::size_t i = 0;
    std::size_t total = 0;
    for (auto _ : state) {
        auto const& s = pool[i++ % pool.size()];
        TimePoint tp;
        bool ok = nanofix::detail::atotimepoint(s.data(), s.data() + s.size(), tp);
        benchmark::DoNotOptimize(ok);
        benchmark::DoNotOptimize(tp);
        total += s.size();
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

void BM_ParseTimestampNanos(benchmark::State& state) {
    static auto const pool = make_timestamp_pool_nanos();
    using TimePoint = std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds>;
    std::size_t i = 0;
    std::size_t total = 0;
    for (auto _ : state) {
        auto const& s = pool[i++ % pool.size()];
        TimePoint tp;
        bool ok = nanofix::detail::atotimepoint_nano(s.data(), s.data() + s.size(), tp);
        benchmark::DoNotOptimize(ok);
        benchmark::DoNotOptimize(tp);
        total += s.size();
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

void BM_FindManyFields_Iterator(benchmark::State& state) {
    char buffer[kBufSize];
    auto tsend = live_timestamp();
    benchmark::DoNotOptimize(tsend);
    std::size_t len = write_new_order(buffer, sizeof(buffer), 1, tsend);
    int n_lookups = static_cast<int>(state.range(0));
    std::size_t total = 0;
    for (auto _ : state) {
        nanofix::message_reader r(buffer, buffer + len);
        if (!r.is_valid())
            continue;
        for (int i = 0; i < n_lookups; ++i) {
            auto it = r.begin();
            if (r.find_with_hint(kSingleMsgTags[i], it)) {
                auto v = it->value();
                benchmark::DoNotOptimize(v);
            }
        }
        total += len;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

void BM_FindManyFields_Indexed(benchmark::State& state) {
    char buffer[kBufSize];
    auto tsend = live_timestamp();
    benchmark::DoNotOptimize(tsend);
    std::size_t len = write_new_order(buffer, sizeof(buffer), 1, tsend);
    nanofix::field_index_buffer<32> idx_buffer;
    int n_lookups = static_cast<int>(state.range(0));
    std::size_t total = 0;
    for (auto _ : state) {
        nanofix::message_reader r(buffer, buffer + len);
        if (!r.is_valid())
            continue;
        auto idx = nanofix::build_field_index(r, idx_buffer);
        for (int i = 0; i < n_lookups; ++i) {
            std::size_t h = 0;
            auto v = idx.find_with_hint(kSingleMsgTags[i], h);
            benchmark::DoNotOptimize(v);
        }
        total += len;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

void BM_IsKnownTag_HotLoop(benchmark::State& state) {
    static constexpr int kProbes[] = {
        1,
        8,
        35,
        49,
        56,
        100,
        268,
        269,
        270,
        271,
        272,
        273,
        274,
        275,
        700,
        1500,
        4321,
        9999,
        12345,
        23000,
        42171,
        50000,
        50002,
        50003,
        -1,
        0,
        100000,
        1000000,
        std::numeric_limits<int>::max(),
        std::numeric_limits<int>::min(),
        60000,
        999,
    };
    std::size_t i = 0;
    std::size_t hits = 0;
    for (auto _ : state) {
        bool b = nanofix::is_known_tag(kProbes[i & 31u]);
        benchmark::DoNotOptimize(b);
        hits += b ? 1u : 0u;
        ++i;
    }
    benchmark::DoNotOptimize(hits);
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}

}  // namespace

namespace nanofix_bench {
void register_primitives() {
    benchmark::RegisterBenchmark("BM_ReadMessageScan", BM_ReadMessageScan);
    benchmark::RegisterBenchmark("BM_IsKnownTag_HotLoop", BM_IsKnownTag_HotLoop);
    auto* cs_b = benchmark::RegisterBenchmark("BM_Checksum_LargeMDIncremental",
                                              BM_Checksum_LargeMDIncremental);
    for (int n : {1, 5, 20, 50, 200, 500}) {
        cs_b->Arg(n);
    }

    auto* csbytes = benchmark::RegisterBenchmark("BM_ChecksumBytes", BM_ChecksumBytes);
    for (int n : {16, 64, 256, 1024, 4096, 16384}) {
        csbytes->Arg(n);
    }
    auto* fti = benchmark::RegisterBenchmark("BM_FindTagInIndex", BM_FindTagInIndex);
    for (int n : {4, 8, 16, 32, 64, 128, 256, 512}) {
        fti->Arg(n);
    }
    auto* fpf = benchmark::RegisterBenchmark("BM_FramingPerField", BM_FramingPerField);
    auto* fbk = benchmark::RegisterBenchmark("BM_FramingBulk", BM_FramingBulk);
    auto* fbc = benchmark::RegisterBenchmark("BM_FramingBulkCapped", BM_FramingBulkCapped);
    for (int n : {256, 1024, 4096, 16384}) {
        fpf->Arg(n);
        fbk->Arg(n);
        fbc->Arg(n);
    }
    benchmark::RegisterBenchmark("BM_ReadMessageFindFields", BM_ReadMessageFindFields);
    benchmark::RegisterBenchmark("BM_RoundTrip", BM_RoundTrip);
    benchmark::RegisterBenchmark("BM_ParseTimestampMillis", BM_ParseTimestampMillis);
    benchmark::RegisterBenchmark("BM_ParseTimestampNanos", BM_ParseTimestampNanos);

    auto* iter_b =
        benchmark::RegisterBenchmark("BM_FindManyFields_Iterator", BM_FindManyFields_Iterator);
    auto* idx_b =
        benchmark::RegisterBenchmark("BM_FindManyFields_Indexed", BM_FindManyFields_Indexed);
    for (int n : {3, 5, 8, 13, 15}) {
        iter_b->Arg(n);
        idx_b->Arg(n);
    }
}
}  // namespace nanofix_bench
