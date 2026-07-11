// Built with -DNANOFIX_ASSERT_FAILFAST (see CMakeLists): a tripped NANOFIX_ASSERT
// aborts rather than counting and producing bogus timings.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "bench_parse_data.hpp"

namespace {

using namespace nanofix_bench_parse;

// The 20-tag sequential/random iterator sweeps — the overview table's
// like-for-like read rows. Identical body to BM_Parse_FindTags in
// bench_parse.cpp; duplicated here so this TU stays tiny.
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

}  // namespace

namespace nanofix_bench {

void register_parse_hot_iter() {
    auto const& datasets = load_datasets();
    for (auto const& ds : datasets) {
        std::string const& name = ds.name;
        benchmark::RegisterBenchmark(("BM_Parse_Sequential_Iter/" + name).c_str(),
                                     BM_Parse_FindTags<kSeqOrderTags, std::size(kSeqOrderTags)>,
                                     &ds);
        benchmark::RegisterBenchmark(("BM_Parse_Random_Iter/" + name).c_str(),
                                     BM_Parse_FindTags<kRandOrderTags, std::size(kRandOrderTags)>,
                                     &ds);
    }
}

}  // namespace nanofix_bench
