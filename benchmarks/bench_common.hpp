#pragma once

#include <nanofix.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#if defined(_MSC_VER)
#include <intrin.h>  // __rdtscp; MSVC has no <x86intrin.h>
#else
#include <x86intrin.h>
#endif
#define NANOFIX_BENCH_HAS_RDTSC 1
#else
#define NANOFIX_BENCH_HAS_RDTSC 0
#endif

namespace nanofix_bench {

inline constexpr std::size_t kBufSize = 1 << 13;

namespace latency_probe {

#if NANOFIX_BENCH_HAS_RDTSC
inline std::uint64_t now() noexcept {
    unsigned aux;
    return __rdtscp(&aux);
}

inline double ticks_per_ns() noexcept {
    static double const k = []() {
        using namespace std::chrono;
        unsigned aux;
        auto t0 = steady_clock::now();
        std::uint64_t const c0 = __rdtscp(&aux);
        std::this_thread::sleep_for(milliseconds(80));
        std::uint64_t const c1 = __rdtscp(&aux);
        auto t1 = steady_clock::now();
        auto const ns = duration_cast<nanoseconds>(t1 - t0).count();
        return ns > 0 ? static_cast<double>(c1 - c0) / static_cast<double>(ns) : 1.0;
    }();
    return k;
}
#else
inline std::uint64_t now() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

inline double ticks_per_ns() noexcept {
    return 1.0;
}
#endif

inline double to_ns(std::uint64_t ticks) noexcept {
    return static_cast<double>(ticks) / ticks_per_ns();
}

inline void calibrate() noexcept {
    (void)ticks_per_ns();
}

}  // namespace latency_probe

using SysTime = std::chrono::system_clock::time_point;

inline SysTime live_timestamp() noexcept {
    return std::chrono::system_clock::now();
}

inline std::size_t write_logon(char* buffer, std::size_t buffer_size, int seq, SysTime tsend) {
    nanofix::message_writer w(buffer, buffer + buffer_size);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(nanofix::tag::MsgType, "A");
    w.push_back_string(nanofix::tag::SenderCompID, "AAAA");
    w.push_back_string(nanofix::tag::TargetCompID, "BBBB");
    w.push_back_int(nanofix::tag::MsgSeqNum, seq);
    w.push_back_timestamp(nanofix::tag::SendingTime, tsend);
    w.push_back_int(nanofix::tag::EncryptMethod, 0);
    w.push_back_int(nanofix::tag::HeartBtInt, 10);
    (void)w.push_back_trailer();
    return static_cast<std::size_t>(w.message_end() - buffer);
}

inline std::size_t write_new_order(char* buffer, std::size_t buffer_size, int seq, SysTime tsend) {
    nanofix::message_writer w(buffer, buffer + buffer_size);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(nanofix::tag::MsgType, "D");
    w.push_back_string(nanofix::tag::SenderCompID, "AAAA");
    w.push_back_string(nanofix::tag::TargetCompID, "BBBB");
    w.push_back_int(nanofix::tag::MsgSeqNum, seq);
    w.push_back_timestamp(nanofix::tag::SendingTime, tsend);
    w.push_back_string(nanofix::tag::ClOrdID, "A1");
    w.push_back_char(nanofix::tag::HandlInst, '1');
    w.push_back_string(nanofix::tag::Symbol, "OIH");
    w.push_back_char(nanofix::tag::Side, '1');
    w.push_back_timestamp(nanofix::tag::TransactTime, tsend);
    w.push_back_int(nanofix::tag::OrderQty, 100);
    w.push_back_char(nanofix::tag::OrdType, '2');
    w.push_back_decimal(nanofix::tag::Price, 50001, -2);
    w.push_back_char(nanofix::tag::TimeInForce, '1');
    (void)w.push_back_trailer();
    return static_cast<std::size_t>(w.message_end() - buffer);
}

// write_new_order with push_back_timestamp_epoch_nanos instead of the
// chrono time_point overload on SendingTime/TransactTime.
inline std::size_t write_new_order_epoch_nanos(char* buffer,
                                               std::size_t buffer_size,
                                               int seq,
                                               std::int64_t epoch_nanos) {
    nanofix::message_writer w(buffer, buffer + buffer_size);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(nanofix::tag::MsgType, "D");
    w.push_back_string(nanofix::tag::SenderCompID, "AAAA");
    w.push_back_string(nanofix::tag::TargetCompID, "BBBB");
    w.push_back_int(nanofix::tag::MsgSeqNum, seq);
    w.push_back_timestamp_epoch_nanos(nanofix::tag::SendingTime, epoch_nanos);
    w.push_back_string(nanofix::tag::ClOrdID, "A1");
    w.push_back_char(nanofix::tag::HandlInst, '1');
    w.push_back_string(nanofix::tag::Symbol, "OIH");
    w.push_back_char(nanofix::tag::Side, '1');
    w.push_back_timestamp_epoch_nanos(nanofix::tag::TransactTime, epoch_nanos);
    w.push_back_int(nanofix::tag::OrderQty, 100);
    w.push_back_char(nanofix::tag::OrdType, '2');
    w.push_back_decimal(nanofix::tag::Price, 50001, -2);
    w.push_back_char(nanofix::tag::TimeInForce, '1');
    (void)w.push_back_trailer();
    return static_cast<std::size_t>(w.message_end() - buffer);
}

// Large MD incremental refresh: header + N MD entries. Pushes per-message size
// into the few-kB range so checksum scanning dominates.
inline std::size_t write_md_incremental_large(
    char* buffer, std::size_t buffer_size, int seq, SysTime tsend, int n_entries) {
    nanofix::message_writer w(buffer, buffer + buffer_size);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(nanofix::tag::MsgType, "X");
    w.push_back_string(nanofix::tag::SenderCompID, "AAAA");
    w.push_back_string(nanofix::tag::TargetCompID, "BBBB");
    w.push_back_int(nanofix::tag::MsgSeqNum, seq);
    w.push_back_timestamp(nanofix::tag::SendingTime, tsend);
    w.push_back_int(nanofix::tag::NoMDEntries, n_entries);
    for (int i = 0; i < n_entries; ++i) {
        w.push_back_char(nanofix::tag::MDUpdateAction, char('0' + (i % 3)));
        w.push_back_char(nanofix::tag::MDEntryType, (i & 1) ? '1' : '0');
        w.push_back_string(nanofix::tag::Symbol, "BTC-USD-PERP");
        w.push_back_decimal(
            nanofix::tag::MDEntryPx, std::int64_t{6'543'212'345'678LL + i * 100LL}, std::int64_t{-8});
        w.push_back_decimal(
            nanofix::tag::MDEntrySize, std::int64_t{12'345'678LL + i * 7LL}, std::int64_t{-8});
    }
    (void)w.push_back_trailer();
    return static_cast<std::size_t>(w.message_end() - buffer);
}

// Defined in the matching bench_*.cpp, called from main() in bench_parse.cpp
// (which registers its own parse group locally).
void register_write();
void register_primitives();

}  // namespace nanofix_bench
