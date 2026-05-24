// Single-message write latency and the write tail distribution.

#include <benchmark/benchmark.h>

#include <nanofix.hpp>
#include <nanofix/detail/fields.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "bench_common.hpp"

using namespace nanofix_bench;

namespace {

void BM_WriteLogon(benchmark::State& state) {
    char buffer[kBufSize];
    auto tsend = live_timestamp();
    benchmark::DoNotOptimize(tsend);
    std::size_t total = 0;
    for (auto _ : state) {
        std::size_t n = write_logon(buffer, sizeof(buffer), 1, tsend);
        benchmark::DoNotOptimize(buffer);
        benchmark::ClobberMemory();
        total += n;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

void BM_WriteNewOrder(benchmark::State& state) {
    char buffer[kBufSize];
    auto tsend = live_timestamp();
    benchmark::DoNotOptimize(tsend);
    std::size_t total = 0;
    for (auto _ : state) {
        std::size_t n = write_new_order(buffer, sizeof(buffer), 1, tsend);
        benchmark::DoNotOptimize(buffer);
        benchmark::ClobberMemory();
        total += n;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

void BM_WriteNewOrder_EpochNanos(benchmark::State& state) {
    char buffer[kBufSize];
    auto epoch_nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(live_timestamp().time_since_epoch()).count();
    benchmark::DoNotOptimize(epoch_nanos);
    std::size_t total = 0;
    for (auto _ : state) {
        std::size_t n = write_new_order_epoch_nanos(buffer, sizeof(buffer), 1, epoch_nanos);
        benchmark::DoNotOptimize(buffer);
        benchmark::ClobberMemory();
        total += n;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

// Closure form via try_write_message: exercises the noexcept-overflow path
// and reports the same shape as BM_WriteNewOrder.
void BM_WriteNewOrder_Closure(benchmark::State& state) {
    char buffer[kBufSize];
    auto tsend = live_timestamp();
    benchmark::DoNotOptimize(tsend);
    std::size_t total = 0;
    for (auto _ : state) {
        char* end_out = nullptr;
        bool ok = nanofix::try_write_message(
            buffer, buffer + sizeof(buffer), end_out, [&](nanofix::message_writer& w) {
                w.push_back_header("FIXT.1.1");
                w.push_back_string(nanofix::tag::MsgType, "D");
                w.push_back_string(nanofix::tag::SenderCompID, "AAAA");
                w.push_back_string(nanofix::tag::TargetCompID, "BBBB");
                w.push_back_int(nanofix::tag::MsgSeqNum, 1);
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
            });
        benchmark::DoNotOptimize(ok);
        benchmark::DoNotOptimize(buffer);
        benchmark::ClobberMemory();
        total += static_cast<std::size_t>(end_out - buffer);
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

// Per-message write latency distribution: serialize one NewOrderSingle, sample
// the wall time, repeat. Counters carry the ~20-30 ns clock::now() probe, so
// absolute p* are inflated by that fixed cost; the shape and relative spread
// are what matter. Mirrors BM_Parse_TailLatency on the read side.
void BM_Write_TailLatency(benchmark::State& state) {
    using clk = std::chrono::steady_clock;
    char buffer[kBufSize];
    auto tsend = live_timestamp();
    benchmark::DoNotOptimize(tsend);
    constexpr int kBatch = 4096;
    std::vector<std::uint64_t> samples;
    samples.reserve(1u << 20);
    for (auto _ : state) {
        state.PauseTiming();
        samples.clear();
        state.ResumeTiming();
        for (int i = 0; i < kBatch; ++i) {
            auto t0 = clk::now();
            std::size_t n = write_new_order(buffer, sizeof(buffer), i, tsend);
            auto t1 = clk::now();
            benchmark::DoNotOptimize(buffer);
            benchmark::DoNotOptimize(n);
            samples.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
        }
    }
    state.PauseTiming();
    std::sort(samples.begin(), samples.end());
    auto pct = [&](double q) -> double {
        std::size_t i = static_cast<std::size_t>(q * static_cast<double>(samples.size() - 1));
        return static_cast<double>(samples[i]);
    };
    state.counters["p50_ns"] = pct(0.50);
    state.counters["p95_ns"] = pct(0.95);
    state.counters["p99_ns"] = pct(0.99);
    state.counters["p999_ns"] = pct(0.999);
    state.counters["max_ns"] = static_cast<double>(samples.back());
    state.counters["samples"] = static_cast<double>(samples.size());
}

}  // namespace

namespace nanofix_bench {
void register_write() {
    benchmark::RegisterBenchmark("BM_WriteLogon", BM_WriteLogon);
    benchmark::RegisterBenchmark("BM_WriteNewOrder", BM_WriteNewOrder);
    benchmark::RegisterBenchmark("BM_WriteNewOrder_EpochNanos", BM_WriteNewOrder_EpochNanos);
    benchmark::RegisterBenchmark("BM_WriteNewOrder_Closure", BM_WriteNewOrder_Closure);
    benchmark::RegisterBenchmark("BM_Write_TailLatency", BM_Write_TailLatency);
}
}  // namespace nanofix_bench
