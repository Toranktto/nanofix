#include <cstddef>
#include <cstdint>
#include <string>

#include "bench_parse_data.hpp"

namespace {

using namespace nanofix_bench_parse;

// The 20-tag sequential/random indexed sweeps — the overview table's indexed
// read rows. Identical body to BM_Parse_FindTagsIndexed_Hint in
// bench_parse.cpp; duplicated here so this TU stays tiny.
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

}  // namespace

namespace nanofix_bench {

void register_parse_hot_indexed() {
    auto const& datasets = load_datasets();
    for (auto const& ds : datasets) {
        std::string const& name = ds.name;
        benchmark::RegisterBenchmark(
            ("BM_Parse_Sequential_Indexed/" + name).c_str(),
            BM_Parse_FindTagsIndexed_Hint<kSeqOrderTags, std::size(kSeqOrderTags)>,
            &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_Random_Indexed/" + name).c_str(),
            BM_Parse_FindTagsIndexed_Hint<kRandOrderTags, std::size(kRandOrderTags)>,
            &ds);
    }
}

}  // namespace nanofix_bench
