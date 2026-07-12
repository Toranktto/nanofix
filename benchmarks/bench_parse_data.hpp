#pragma once

// Dataset loading, tag sets, and templates shared by bench_parse.cpp and
// bench_parse_hot_iter.cpp. The hot iterator sweeps live in their own non-LTO
// TU so the indexed benches' always-inline AVX2 bodies don't exhaust GCC's
// inline-unit-growth budget and pessimize them (~18-19% on
// BM_Parse_{Sequential,Random}_Iter; see benchmarks/CMakeLists.txt).

#include <benchmark/benchmark.h>

#include <nanofix.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "bench_common.hpp"

#ifndef NANOFIX_BENCH_DATA_DIR
#define NANOFIX_BENCH_DATA_DIR ""
#endif

namespace nanofix_bench_parse {

// Sized for the largest synthetic MD-incremental (~1.1k fields).
// truncated() guards below SkipWithError if fixgen grows past this.
inline constexpr std::size_t kFieldIndexCapacity = 2048;

inline std::vector<char> load_file(std::filesystem::path const& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in)
        return {};
    auto size = in.tellg();
    if (size <= 0)
        return {};
    std::vector<char> buf(static_cast<std::size_t>(size));
    in.seekg(0);
    in.read(buf.data(), size);
    return buf;
}

struct Dataset {
    std::string name;
    std::vector<char> data;
};

inline std::vector<Dataset> const& load_datasets() {
    static std::vector<Dataset> const datasets = []() {
        std::vector<Dataset> out;
        std::filesystem::path dir{NANOFIX_BENCH_DATA_DIR};
        if (dir.empty() || !std::filesystem::exists(dir))
            return out;
        // Sort for stable bench names across hosts.
        std::vector<std::filesystem::path> paths;
        for (auto const& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.path().extension() == ".fix")
                paths.push_back(entry.path());
        }
        std::sort(paths.begin(), paths.end());
        for (auto const& p : paths) {
            auto buf = load_file(p);
            if (buf.empty())
                continue;
            out.push_back({p.stem().string(), std::move(buf)});
        }
        return out;
    }();
    return datasets;
}

// 5 session headers — all present on every MsgType, header-resident.
// Wire-order, so find_with_hint's carry kicks in after the first hit.
inline constexpr int kCommonTags[] = {
    nanofix::tag::MsgType,
    nanofix::tag::SenderCompID,
    nanofix::tag::TargetCompID,
    nanofix::tag::MsgSeqNum,
    nanofix::tag::SendingTime,
};

// 5 headers + 5 MD body + 5 NewOrder/Exec tags. ~50% per-tag presence
// on fixgen's mix (MD body absent on order msgs and vice versa).
inline constexpr int kManyTags[] = {
    nanofix::tag::MsgType,
    nanofix::tag::SenderCompID,
    nanofix::tag::TargetCompID,
    nanofix::tag::MsgSeqNum,
    nanofix::tag::SendingTime,
    nanofix::tag::Symbol,
    nanofix::tag::NoMDEntries,
    nanofix::tag::MDEntryType,
    nanofix::tag::MDEntryPx,
    nanofix::tag::MDEntrySize,
    nanofix::tag::ClOrdID,
    nanofix::tag::OrderQty,
    nanofix::tag::Price,
    nanofix::tag::OrdType,
    nanofix::tag::TimeInForce,
};

// Superset of kManyTags + 15 more ExecReport / order tags. Most are
// absent on any single MD or NewOrder msg → every miss forces a full
// scan to end of msg. Iterator worst case.
inline constexpr int kWideTags[] = {
    nanofix::tag::MsgType,      nanofix::tag::SenderCompID, nanofix::tag::TargetCompID,
    nanofix::tag::MsgSeqNum,    nanofix::tag::SendingTime,  nanofix::tag::Symbol,
    nanofix::tag::NoMDEntries,  nanofix::tag::MDEntryType,  nanofix::tag::MDEntryPx,
    nanofix::tag::MDEntrySize,  nanofix::tag::ClOrdID,      nanofix::tag::OrderQty,
    nanofix::tag::Price,        nanofix::tag::OrdType,      nanofix::tag::TimeInForce,
    nanofix::tag::Account,      nanofix::tag::ExpireTime,   nanofix::tag::Side,
    nanofix::tag::OrderID,      nanofix::tag::ExecID,       nanofix::tag::HandlInst,
    nanofix::tag::TransactTime, nanofix::tag::AvgPx,        nanofix::tag::LastPx,
    nanofix::tag::LastQty,      nanofix::tag::SecurityID,   nanofix::tag::CumQty,
    nanofix::tag::LeavesQty,    nanofix::tag::OrdStatus,    nanofix::tag::ExecType,
};

// 5 session headers + 10 ExecReport body tags; ~55-65% per-tag hit against
// fixgen's default mix, not an all-present baseline.
inline constexpr int kWideTagsHighHit[] = {
    nanofix::tag::MsgType,
    nanofix::tag::SenderCompID,
    nanofix::tag::TargetCompID,
    nanofix::tag::MsgSeqNum,
    nanofix::tag::SendingTime,
    nanofix::tag::Symbol,
    nanofix::tag::Side,
    nanofix::tag::OrderID,
    nanofix::tag::ExecID,
    nanofix::tag::OrdStatus,
    nanofix::tag::ExecType,
    nanofix::tag::LeavesQty,
    nanofix::tag::CumQty,
    nanofix::tag::AvgPx,
    nanofix::tag::LastPx,
};

// 20 tags read per message. kSeqOrderTags matches wire order
// (find_with_hint's hint carries cleanly); kRandOrderTags reverses it
// so every lookup wraps from end-of-message back to begin.
inline constexpr int kSeqOrderTags[] = {
    nanofix::tag::MsgType,     nanofix::tag::SenderCompID, nanofix::tag::TargetCompID,
    nanofix::tag::MsgSeqNum,   nanofix::tag::SendingTime,  nanofix::tag::Symbol,
    nanofix::tag::NoMDEntries, nanofix::tag::MDEntryType,  nanofix::tag::MDEntryPx,
    nanofix::tag::MDEntrySize, nanofix::tag::ClOrdID,      nanofix::tag::OrderQty,
    nanofix::tag::Price,       nanofix::tag::OrdType,      nanofix::tag::TimeInForce,
    nanofix::tag::Account,     nanofix::tag::ExpireTime,   nanofix::tag::Side,
    nanofix::tag::OrderID,     nanofix::tag::ExecID,
};
inline constexpr int kRandOrderTags[] = {
    nanofix::tag::ExecID,       nanofix::tag::OrderID,     nanofix::tag::Side,
    nanofix::tag::ExpireTime,   nanofix::tag::Account,     nanofix::tag::TimeInForce,
    nanofix::tag::OrdType,      nanofix::tag::Price,       nanofix::tag::OrderQty,
    nanofix::tag::ClOrdID,      nanofix::tag::MDEntrySize, nanofix::tag::MDEntryPx,
    nanofix::tag::MDEntryType,  nanofix::tag::NoMDEntries, nanofix::tag::Symbol,
    nanofix::tag::SendingTime,  nanofix::tag::MsgSeqNum,   nanofix::tag::TargetCompID,
    nanofix::tag::SenderCompID, nanofix::tag::MsgType,
};

struct MessageOffsets {
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint32_t> sizes;
};

inline MessageOffsets const* message_offsets_for(Dataset const& ds) {
    static std::vector<std::unique_ptr<MessageOffsets>> cache;
    static std::vector<Dataset const*> keys;
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == &ds)
            return cache[i].get();
    auto mo = std::make_unique<MessageOffsets>();
    char const* begin = ds.data.data();
    char const* end = begin + ds.data.size();
    nanofix::message_reader r(begin, end);
    for (; r.is_complete(); r = r.next_message_reader()) {
        if (!r.is_valid())
            continue;
        mo->offsets.push_back(static_cast<std::uint32_t>(r.message_begin() - begin));
        mo->sizes.push_back(static_cast<std::uint32_t>(r.message_size()));
    }
    auto* raw = mo.get();
    cache.push_back(std::move(mo));
    keys.push_back(&ds);
    return raw;
}

// Counters include the latency_probe cost (RDTSCP ~5-10 cycles on x86-64,
// steady_clock ~20-30 ns elsewhere); same overhead in every variant, so
// relative deltas are comparable.
template <int const* Tags, std::size_t N, bool UseIndex>
void BM_Parse_TailLatency(benchmark::State& state, Dataset const* ds) {
    namespace probe = nanofix_bench::latency_probe;
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    nanofix::field_index_buffer<kFieldIndexCapacity> idx_buffer;
    std::vector<std::uint64_t> samples;
    samples.reserve(1u << 20);
    probe::calibrate();
    for (auto _ : state) {
        state.PauseTiming();
        samples.clear();
        state.ResumeTiming();
        nanofix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (!r.is_valid())
                continue;
            std::uint64_t const t0 = probe::now();
            if constexpr (UseIndex) {
                auto idx = nanofix::build_field_index(r, idx_buffer);
                std::size_t h = 0;
                for (std::size_t k = 0; k < N; ++k) {
                    auto v = idx.find_with_hint(Tags[k], h);
                    benchmark::DoNotOptimize(v);
                }
            } else {
                auto it = r.begin();
                for (std::size_t k = 0; k < N; ++k) {
                    if (r.find_with_hint(Tags[k], it)) {
                        auto v = it->value();
                        benchmark::DoNotOptimize(v);
                    }
                }
            }
            std::uint64_t const t1 = probe::now();
            samples.push_back(t1 - t0);
        }
    }
    state.PauseTiming();
    if (samples.empty()) {
        state.SkipWithError("no messages measured");
        return;
    }
    std::sort(samples.begin(), samples.end());
    auto pct = [&](double q) -> double {
        std::size_t i = static_cast<std::size_t>(q * static_cast<double>(samples.size() - 1));
        return probe::to_ns(samples[i]);
    };
    state.counters["p95_ns"] = pct(0.95);
    state.counters["p99_ns"] = pct(0.99);
    state.counters["p999_ns"] = pct(0.999);
    state.counters["max_ns"] = probe::to_ns(samples.back());
    state.counters["samples"] = static_cast<double>(samples.size());
}

inline constexpr int kAmortizeN[] = {3, 5, 7, 8, 10, 13, 15, 20, 30};

}  // namespace nanofix_bench_parse
