#include <benchmark/benchmark.h>

// Built with -DNANOFIX_ASSERT_FAILFAST (see CMakeLists): a tripped NANOFIX_ASSERT
// aborts rather than counting and producing bogus timings.
#include <nanofix.hpp>
#include <nanofix/detail/fields.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "bench_common.hpp"

#ifndef NANOFIX_BENCH_DATA_DIR
#define NANOFIX_BENCH_DATA_DIR ""
#endif

namespace {

// Sized for the largest synthetic MD-incremental (~1.1k fields).
// truncated() guards below SkipWithError if fixgen grows past this.
constexpr std::size_t kFieldIndexCapacity = 2048;

std::vector<char> load_file(std::filesystem::path const& p) {
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

std::vector<Dataset> const& load_datasets() {
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
constexpr int kCommonTags[] = {
    nanofix::tag::MsgType,
    nanofix::tag::SenderCompID,
    nanofix::tag::TargetCompID,
    nanofix::tag::MsgSeqNum,
    nanofix::tag::SendingTime,
};

// 5 headers + 5 MD body + 5 NewOrder/Exec tags. ~50% per-tag presence
// on fixgen's mix (MD body absent on order msgs and vice versa).
constexpr int kManyTags[] = {
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
constexpr int kWideTags[] = {
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
constexpr int kWideTagsHighHit[] = {
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
constexpr int kSeqOrderTags[] = {
    nanofix::tag::MsgType,     nanofix::tag::SenderCompID, nanofix::tag::TargetCompID,
    nanofix::tag::MsgSeqNum,   nanofix::tag::SendingTime,  nanofix::tag::Symbol,
    nanofix::tag::NoMDEntries, nanofix::tag::MDEntryType,  nanofix::tag::MDEntryPx,
    nanofix::tag::MDEntrySize, nanofix::tag::ClOrdID,      nanofix::tag::OrderQty,
    nanofix::tag::Price,       nanofix::tag::OrdType,      nanofix::tag::TimeInForce,
    nanofix::tag::Account,     nanofix::tag::ExpireTime,   nanofix::tag::Side,
    nanofix::tag::OrderID,     nanofix::tag::ExecID,
};
constexpr int kRandOrderTags[] = {
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

MessageOffsets const* message_offsets_for(Dataset const& ds) {
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

void BM_Parse_ScanMessages(benchmark::State& state, Dataset const* ds) {
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    std::size_t total_bytes = 0;
    std::size_t total_messages = 0;
    for (auto _ : state) {
        std::size_t messages = 0;
        nanofix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (r.is_valid()) {
                ++messages;
                benchmark::DoNotOptimize(messages);
            }
        }
        total_bytes += ds->data.size();
        total_messages += messages;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(static_cast<int64_t>(total_messages));
}

void BM_Parse_IterAllFields(benchmark::State& state, Dataset const* ds) {
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    std::size_t total_bytes = 0;
    std::size_t total_fields = 0;
    for (auto _ : state) {
        std::size_t fields = 0;
        nanofix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (!r.is_valid())
                continue;
            for (auto it = r.begin(); it != r.end(); ++it) {
                ++fields;
                benchmark::DoNotOptimize(it->tag());
            }
        }
        total_bytes += ds->data.size();
        total_fields += fields;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(static_cast<int64_t>(total_fields));
}

template <int const* Tags, std::size_t N>
void BM_Parse_FindTags(benchmark::State& state, Dataset const* ds) {
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    std::size_t total_bytes = 0;
    std::size_t total_messages = 0;
    for (auto _ : state) {
        std::size_t messages = 0;
        nanofix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (!r.is_valid())
                continue;
            ++messages;
            auto it = r.begin();
            for (std::size_t k = 0; k < N; ++k) {
                if (r.find_with_hint(Tags[k], it)) {
                    auto v = it->value();
                    benchmark::DoNotOptimize(v);
                }
            }
        }
        total_bytes += ds->data.size();
        total_messages += messages;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(static_cast<int64_t>(total_messages));
}

// One forward pass, dispatch each field against the wanted set: O(message
// length), independent of N and of how many tags are absent. The realistic
// counterpart to BM_Parse_FindTags, whose per-tag find_with_hint loop runs
// O(N * length) and collapses on the absent-heavy kWideTags set.
template <int const* Tags, std::size_t N>
void BM_Parse_DispatchTags(benchmark::State& state, Dataset const* ds) {
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    std::array<int, N> wanted{};
    for (std::size_t i = 0; i < N; ++i)
        wanted[i] = Tags[i];
    std::sort(wanted.begin(), wanted.end());
    std::size_t total_bytes = 0;
    std::size_t total_messages = 0;
    for (auto _ : state) {
        std::size_t messages = 0;
        nanofix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (!r.is_valid())
                continue;
            ++messages;
            for (auto it = r.begin(); it != r.end(); ++it) {
                if (std::binary_search(wanted.begin(), wanted.end(), it->tag())) {
                    auto v = it->value();
                    benchmark::DoNotOptimize(v);
                }
            }
        }
        total_bytes += ds->data.size();
        total_messages += messages;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(static_cast<int64_t>(total_messages));
}

// Hint hoisted: lookup order matters. The non-Hint variant resets per call.
template <int const* Tags, std::size_t N>
void BM_Parse_FindTagsIndexed_Hint(benchmark::State& state, Dataset const* ds) {
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    nanofix::field_index_buffer<kFieldIndexCapacity> idx_buffer;
    std::size_t total_bytes = 0;
    std::size_t total_messages = 0;
    for (auto _ : state) {
        std::size_t messages = 0;
        nanofix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (!r.is_valid())
                continue;
            ++messages;
            auto idx = nanofix::build_field_index(r, idx_buffer);
            std::size_t h = 0;
            for (std::size_t k = 0; k < N; ++k) {
                auto v = idx.find_with_hint(Tags[k], h);
                benchmark::DoNotOptimize(v);
            }
            if (idx.truncated()) [[unlikely]] {
                state.SkipWithError("index truncated; bump kFieldIndexCapacity");
                return;
            }
        }
        total_bytes += ds->data.size();
        total_messages += messages;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(static_cast<int64_t>(total_messages));
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

template <int const* Tags, std::size_t N>
void BM_Parse_FindTagsIndexed(benchmark::State& state, Dataset const* ds) {
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    nanofix::field_index_buffer<kFieldIndexCapacity> idx_buffer;
    std::size_t total_bytes = 0;
    std::size_t total_messages = 0;
    for (auto _ : state) {
        std::size_t messages = 0;
        nanofix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (!r.is_valid())
                continue;
            ++messages;
            auto idx = nanofix::build_field_index(r, idx_buffer);
            for (std::size_t k = 0; k < N; ++k) {
                std::size_t h = 0;
                auto v = idx.find_with_hint(Tags[k], h);
                benchmark::DoNotOptimize(v);
            }
            if (idx.truncated()) [[unlikely]] {
                state.SkipWithError("index truncated; bump kFieldIndexCapacity");
                return;
            }
        }
        total_bytes += ds->data.size();
        total_messages += messages;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(static_cast<int64_t>(total_messages));
}

void BM_Parse_BuildFieldIndex(benchmark::State& state, Dataset const* ds) {
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    nanofix::field_index_buffer<kFieldIndexCapacity> idx_buffer;
    std::size_t total_bytes = 0;
    std::size_t total_messages = 0;
    for (auto _ : state) {
        std::size_t messages = 0;
        nanofix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (!r.is_valid())
                continue;
            ++messages;
            auto idx = nanofix::build_field_index(r, idx_buffer);
            benchmark::DoNotOptimize(idx);
            if (idx.truncated()) [[unlikely]] {
                state.SkipWithError("index truncated; bump kFieldIndexCapacity");
                return;
            }
        }
        total_bytes += ds->data.size();
        total_messages += messages;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(static_cast<int64_t>(total_messages));
}

// Mixed-probe set defeats branch prediction caching on the bitmap lookup.
// Delta vs BM_Parse_IterAllFields = whitelist overhead in realistic workload.
void BM_Parse_IterAllFields_Whitelist(benchmark::State& state, Dataset const* ds) {
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    std::size_t total_bytes = 0;
    std::size_t total_fields = 0;
    std::size_t rejected = 0;
    for (auto _ : state) {
        std::size_t fields = 0;
        std::size_t rej = 0;
        nanofix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (!r.is_valid())
                continue;
            for (auto it = r.begin(); it != r.end(); ++it) {
                ++fields;
                int tag = it->tag();
                if (!nanofix::is_known_tag(tag)) [[unlikely]] {
                    ++rej;
                    continue;
                }
                benchmark::DoNotOptimize(tag);
            }
        }
        total_bytes += ds->data.size();
        total_fields += fields;
        rejected += rej;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(static_cast<int64_t>(total_fields));
    state.counters["rejected"] = static_cast<double>(rejected);
}

// Random per-iter msg pick across the >> L3 dataset; forces LLC misses.
void BM_Parse_RandomAccess_Iter(benchmark::State& state, Dataset const* ds, MessageOffsets const* mo) {
    if (mo->offsets.empty()) {
        state.SkipWithError("dataset has no valid messages");
        return;
    }
    std::mt19937_64 rng(0xC0DEC0DEC0DEC0DEULL);
    std::uniform_int_distribution<std::size_t> dist(0, mo->offsets.size() - 1);
    char const* base = ds->data.data();
    std::size_t total = 0;
    std::size_t messages = 0;
    for (auto _ : state) {
        std::size_t const i = dist(rng);
        char const* mb = base + mo->offsets[i];
        char const* me = mb + mo->sizes[i];
        nanofix::message_reader r(mb, me);
        if (r.is_valid()) [[likely]] {
            for (auto it = r.begin(); it != r.end(); ++it) {
                benchmark::DoNotOptimize(it->tag());
                auto v = it->value();
                benchmark::DoNotOptimize(v);
            }
            total += mo->sizes[i];
            ++messages;
        }
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
    state.SetItemsProcessed(static_cast<int64_t>(messages));
}

// 32 MB eviction sweep between samples (>= M4 SLC, >= typical x86 L3 partition)
// flushes message bytes out of cache; PauseTiming excludes sweep from timing.
void BM_Parse_CacheCold_Single(benchmark::State& state, Dataset const* ds, MessageOffsets const* mo) {
    if (mo->offsets.empty()) {
        state.SkipWithError("dataset has no valid messages");
        return;
    }
    constexpr std::size_t kEvictBytes = std::size_t{32} * 1024 * 1024;
    std::vector<char> evict(kEvictBytes);
    for (std::size_t i = 0; i < evict.size(); ++i)
        evict[i] = static_cast<char>(i & 0xFFu);

    std::mt19937_64 rng(0xDECAFC0FFEEBABEULL);
    std::uniform_int_distribution<std::size_t> dist(0, mo->offsets.size() - 1);
    char const* base = ds->data.data();
    std::size_t total = 0;
    std::size_t messages = 0;

    for (auto _ : state) {
        state.PauseTiming();
        std::uint64_t acc = 0;
        for (std::size_t i = 0; i < evict.size(); i += 64) {
            acc += static_cast<std::uint8_t>(evict[i]);
        }
        benchmark::DoNotOptimize(acc);
        std::size_t const idx = dist(rng);
        char const* mb = base + mo->offsets[idx];
        char const* me = mb + mo->sizes[idx];
        state.ResumeTiming();

        nanofix::message_reader r(mb, me);
        if (r.is_valid()) [[likely]] {
            for (auto it = r.begin(); it != r.end(); ++it) {
                benchmark::DoNotOptimize(it->tag());
                auto v = it->value();
                benchmark::DoNotOptimize(v);
            }
            total += mo->sizes[idx];
            ++messages;
        }
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
    state.SetItemsProcessed(static_cast<int64_t>(messages));
}

// Cold-cache counterpart to BM_Parse_CacheCold_Single for the indexed path:
// the 32 MB sweep evicts the picked message, then build_field_index + the
// kSeqOrderTags lookups run on cold bytes. Isolates how indexing's extra pass
// fares when memory, not branch prediction, is the bottleneck.
void BM_Parse_CacheCold_Indexed(benchmark::State& state, Dataset const* ds, MessageOffsets const* mo) {
    if (mo->offsets.empty()) {
        state.SkipWithError("dataset has no valid messages");
        return;
    }
    constexpr std::size_t kEvictBytes = std::size_t{32} * 1024 * 1024;
    std::vector<char> evict(kEvictBytes);
    for (std::size_t i = 0; i < evict.size(); ++i)
        evict[i] = static_cast<char>(i & 0xFFu);

    nanofix::field_index_buffer<kFieldIndexCapacity> idx_buffer;
    std::mt19937_64 rng(0xDECAFC0FFEEBABEULL);
    std::uniform_int_distribution<std::size_t> dist(0, mo->offsets.size() - 1);
    char const* base = ds->data.data();
    std::size_t messages = 0;

    for (auto _ : state) {
        state.PauseTiming();
        std::uint64_t acc = 0;
        for (std::size_t i = 0; i < evict.size(); i += 64)
            acc += static_cast<std::uint8_t>(evict[i]);
        benchmark::DoNotOptimize(acc);
        std::size_t const idx = dist(rng);
        char const* mb = base + mo->offsets[idx];
        char const* me = mb + mo->sizes[idx];
        state.ResumeTiming();

        nanofix::message_reader r(mb, me);
        if (r.is_valid()) [[likely]] {
            auto fi = nanofix::build_field_index(r, idx_buffer);
            std::size_t h = 0;
            for (int t : kSeqOrderTags) {
                auto v = fi.find_with_hint(t, h);
                benchmark::DoNotOptimize(v);
            }
            ++messages;
        }
    }
    state.SetItemsProcessed(static_cast<int64_t>(messages));
}

void BM_Parse_FindN_Iter(benchmark::State& state, Dataset const* ds, int n) {
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    std::size_t total_bytes = 0;
    std::size_t total_messages = 0;
    for (auto _ : state) {
        std::size_t messages = 0;
        nanofix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (!r.is_valid())
                continue;
            ++messages;
            auto it = r.begin();
            for (int i = 0; i < n; ++i) {
                if (r.find_with_hint(kWideTags[i], it)) {
                    auto v = it->value();
                    benchmark::DoNotOptimize(v);
                }
            }
        }
        total_bytes += ds->data.size();
        total_messages += messages;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(static_cast<int64_t>(total_messages));
}

void BM_Parse_FindN_Indexed(benchmark::State& state, Dataset const* ds, int n) {
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    nanofix::field_index_buffer<kFieldIndexCapacity> idx_buffer;
    std::size_t total_bytes = 0;
    std::size_t total_messages = 0;
    for (auto _ : state) {
        std::size_t messages = 0;
        nanofix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (!r.is_valid())
                continue;
            ++messages;
            auto idx = nanofix::build_field_index(r, idx_buffer);
            for (int i = 0; i < n; ++i) {
                std::size_t h = 0;
                auto v = idx.find_with_hint(kWideTags[i], h);
                benchmark::DoNotOptimize(v);
            }
            if (idx.truncated()) [[unlikely]] {
                state.SkipWithError("index truncated; bump kFieldIndexCapacity");
                return;
            }
        }
        total_bytes += ds->data.size();
        total_messages += messages;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(static_cast<int64_t>(total_messages));
}

// reader.group(count,delim).for_each(fn) sugar vs hand-rolled delimiter scan.
// Same lookups (MDEntryPx, MDEntrySize per entry). Throughput must match.
void BM_Parse_GroupAccess_Sugared(benchmark::State& state, Dataset const* ds) {
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    std::size_t total_bytes = 0;
    std::size_t total_messages = 0;
    for (auto _ : state) {
        std::size_t messages = 0;
        nanofix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (!r.is_valid())
                continue;
            ++messages;
            r.group(nanofix::tag::NoMDEntries, nanofix::tag::MDEntryType)
                .for_each([&](nanofix::group_entry const& entry) {
                    auto eit = entry.begin();
                    if (entry.find_with_hint(nanofix::tag::MDEntryPx, eit)) {
                        auto v = eit->value();
                        benchmark::DoNotOptimize(v);
                    }
                    eit = entry.begin();
                    if (entry.find_with_hint(nanofix::tag::MDEntrySize, eit)) {
                        auto v = eit->value();
                        benchmark::DoNotOptimize(v);
                    }
                });
        }
        total_bytes += ds->data.size();
        total_messages += messages;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(static_cast<int64_t>(total_messages));
}

void BM_Parse_GroupAccess_Manual(benchmark::State& state, Dataset const* ds) {
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    constexpr int kDelimiter = nanofix::tag::MDEntryType;
    std::size_t total_bytes = 0;
    std::size_t total_messages = 0;
    for (auto _ : state) {
        std::size_t messages = 0;
        nanofix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (!r.is_valid())
                continue;
            ++messages;
            auto it = r.begin();
            auto const end_it = r.end();
            if (!r.find_with_hint(nanofix::tag::NoMDEntries, it))
                continue;
            std::size_t count = 0;
            if (!it->value().try_as_int<std::size_t>(count))
                continue;
            ++it;
            while (it != end_it && it->tag() != kDelimiter)
                ++it;
            std::size_t seen = 0;
            while (seen < count && it != end_it) {
                auto entry_begin = it;
                ++it;
                while (it != end_it && it->tag() != kDelimiter)
                    ++it;
                auto entry_end = it;
                auto eit = entry_begin;
                if (nanofix::find_with_hint(
                        entry_begin, entry_end, nanofix::tag_equal(nanofix::tag::MDEntryPx), eit)) {
                    auto v = eit->value();
                    benchmark::DoNotOptimize(v);
                }
                eit = entry_begin;
                if (nanofix::find_with_hint(
                        entry_begin, entry_end, nanofix::tag_equal(nanofix::tag::MDEntrySize), eit)) {
                    auto v = eit->value();
                    benchmark::DoNotOptimize(v);
                }
                ++seen;
            }
        }
        total_bytes += ds->data.size();
        total_messages += messages;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(static_cast<int64_t>(total_messages));
}

constexpr int kAmortizeN[] = {3, 5, 7, 8, 10, 13, 15, 20, 30};

void register_parse() {
    auto const& datasets = load_datasets();
    if (datasets.empty()) {
        benchmark::RegisterBenchmark("BM_Parse/<no-dataset>", [](benchmark::State& s) {
            s.SkipWithError("no dataset; run cmake --build <build> --target bench_data");
        });
        return;
    }
    for (auto const& ds : datasets) {
        std::string const& name = ds.name;
        benchmark::RegisterBenchmark(
            ("BM_Parse_ScanMessages/" + name).c_str(), BM_Parse_ScanMessages, &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_IterAllFields/" + name).c_str(), BM_Parse_IterAllFields, &ds);
        benchmark::RegisterBenchmark(("BM_Parse_IterAllFields_Whitelist/" + name).c_str(),
                                     BM_Parse_IterAllFields_Whitelist,
                                     &ds);
        benchmark::RegisterBenchmark(("BM_Parse_FindCommonFields/" + name).c_str(),
                                     BM_Parse_FindTags<kCommonTags, std::size(kCommonTags)>,
                                     &ds);
        benchmark::RegisterBenchmark(("BM_Parse_FindManyFields_Iter/" + name).c_str(),
                                     BM_Parse_FindTags<kManyTags, std::size(kManyTags)>,
                                     &ds);
        benchmark::RegisterBenchmark(("BM_Parse_FindLotsFields_Iter/" + name).c_str(),
                                     BM_Parse_FindTags<kWideTags, std::size(kWideTags)>,
                                     &ds);
        // Single-pass dispatch: counterpart to the repeated-find rows above.
        benchmark::RegisterBenchmark(("BM_Parse_FindManyFields_Dispatch/" + name).c_str(),
                                     BM_Parse_DispatchTags<kManyTags, std::size(kManyTags)>,
                                     &ds);
        benchmark::RegisterBenchmark(("BM_Parse_FindLotsFields_Dispatch/" + name).c_str(),
                                     BM_Parse_DispatchTags<kWideTags, std::size(kWideTags)>,
                                     &ds);
        benchmark::RegisterBenchmark(("BM_Parse_FindCommonFields_Indexed/" + name).c_str(),
                                     BM_Parse_FindTagsIndexed<kCommonTags, std::size(kCommonTags)>,
                                     &ds);
        benchmark::RegisterBenchmark(("BM_Parse_FindManyFields_Indexed/" + name).c_str(),
                                     BM_Parse_FindTagsIndexed<kManyTags, std::size(kManyTags)>,
                                     &ds);
        benchmark::RegisterBenchmark(("BM_Parse_FindLotsFields_Indexed/" + name).c_str(),
                                     BM_Parse_FindTagsIndexed<kWideTags, std::size(kWideTags)>,
                                     &ds);
        // HighHit: 5 always-present headers + 10 partial ExecReport body tags
        // (~55-65% per-tag hit rate on fixgen's mix); not an all-present baseline.
        benchmark::RegisterBenchmark(("BM_Parse_FindManyFields_Iter_HighHit/" + name).c_str(),
                                     BM_Parse_FindTags<kWideTagsHighHit, std::size(kWideTagsHighHit)>,
                                     &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_FindManyFields_Indexed_HighHit/" + name).c_str(),
            BM_Parse_FindTagsIndexed<kWideTagsHighHit, std::size(kWideTagsHighHit)>,
            &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_BuildFieldIndex/" + name).c_str(), BM_Parse_BuildFieldIndex, &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_GroupAccess_Sugared/" + name).c_str(), BM_Parse_GroupAccess_Sugared, &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_GroupAccess_Manual/" + name).c_str(), BM_Parse_GroupAccess_Manual, &ds);

        benchmark::RegisterBenchmark(("BM_Parse_Sequential_Iter/" + name).c_str(),
                                     BM_Parse_FindTags<kSeqOrderTags, std::size(kSeqOrderTags)>,
                                     &ds);
        benchmark::RegisterBenchmark(("BM_Parse_Random_Iter/" + name).c_str(),
                                     BM_Parse_FindTags<kRandOrderTags, std::size(kRandOrderTags)>,
                                     &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_Sequential_Indexed/" + name).c_str(),
            BM_Parse_FindTagsIndexed_Hint<kSeqOrderTags, std::size(kSeqOrderTags)>,
            &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_Random_Indexed/" + name).c_str(),
            BM_Parse_FindTagsIndexed_Hint<kRandOrderTags, std::size(kRandOrderTags)>,
            &ds);

        benchmark::RegisterBenchmark(
            ("BM_Parse_TailLatency_Sequential_Iter/" + name).c_str(),
            BM_Parse_TailLatency<kSeqOrderTags, std::size(kSeqOrderTags), false>,
            &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_TailLatency_Random_Iter/" + name).c_str(),
            BM_Parse_TailLatency<kRandOrderTags, std::size(kRandOrderTags), false>,
            &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_TailLatency_Sequential_Indexed/" + name).c_str(),
            BM_Parse_TailLatency<kSeqOrderTags, std::size(kSeqOrderTags), true>,
            &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_TailLatency_Random_Indexed/" + name).c_str(),
            BM_Parse_TailLatency<kRandOrderTags, std::size(kRandOrderTags), true>,
            &ds);

        auto const* mo = message_offsets_for(ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_RandomAccess_Iter/" + name).c_str(), BM_Parse_RandomAccess_Iter, &ds, mo);
        benchmark::RegisterBenchmark(
            ("BM_Parse_CacheCold_Single/" + name).c_str(), BM_Parse_CacheCold_Single, &ds, mo)
            ->Iterations(128)
            ->Unit(benchmark::kNanosecond);
        benchmark::RegisterBenchmark(
            ("BM_Parse_CacheCold_Indexed/" + name).c_str(), BM_Parse_CacheCold_Indexed, &ds, mo)
            ->Iterations(128)
            ->Unit(benchmark::kNanosecond);

        for (int n : kAmortizeN) {
            std::string n_str = std::to_string(n);
            benchmark::RegisterBenchmark(
                ("BM_Parse_FindN_Iter/" + n_str + "/" + name).c_str(), BM_Parse_FindN_Iter, &ds, n);
            benchmark::RegisterBenchmark(("BM_Parse_FindN_Indexed/" + n_str + "/" + name).c_str(),
                                         BM_Parse_FindN_Indexed,
                                         &ds,
                                         n);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    nanofix_bench::register_write();
    nanofix_bench::register_primitives();
    register_parse();
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv))
        return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
