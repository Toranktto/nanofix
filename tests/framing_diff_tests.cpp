// Differential gate: bulk-SIMD build_field_index must match a per-field reference.

#include <gtest/gtest.h>

#include <nanofix.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "test_common.hpp"

#ifndef NANOFIX_TEST_DATA_DIR
#error "NANOFIX_TEST_DATA_DIR must be set by the build system"
#endif

using nanofix_test::read_file;

// ref_build_field_index is an independent per-field find_soh() loop (no bulk
// find_all_soh), kept in lockstep with the iterator's data-length semantics.
// The production nanofix::build_field_index must produce byte-identical
// (tag, pos, len, truncated) output over every fixture, including the
// embedded-SOH binary fixture that exercises the data-length jump. If this
// diverges, the bulk rewrite is wrong by default.
namespace {

struct RefIndex {
    std::vector<int> tags;
    std::vector<std::uint64_t> pos;
    std::vector<std::uint64_t> len;
    bool truncated = false;
};

template <std::size_t N>
RefIndex ref_build_field_index(nanofix::message_reader const& r) {
    namespace d = nanofix::detail;
    RefIndex out;
    if (!r.is_valid())
        return out;
    if (r.message_size() > nanofix::indexed_message::kMaxIndexableMessageBytes) {
        out.truncated = true;
        return out;
    }
    char const* const msg_begin = r.message_begin();
    auto const begin_field = *r.begin();
    char const* const stop = r.end().buffer_begin();
    std::size_t count = 0;
    auto write_slot = [&](int tag, char const* vb, char const* ve) {
        out.tags.push_back(tag);
        out.pos.push_back(static_cast<std::uint64_t>(vb - msg_begin));
        out.len.push_back(static_cast<std::uint64_t>(ve - vb));
        ++count;
    };
    auto truncate = [&] {
        out.truncated = true;
        out.tags.clear();
        out.pos.clear();
        out.len.clear();
    };

    write_slot(begin_field.tag(), begin_field.value().begin(), begin_field.value().end());
    char const* p = begin_field.value().end() + 1;

    while (p < stop) {
        if (count == N) {
            truncate();
            return out;
        }
        unsigned utag = 0;
        while (p < stop && *p != '=' && *p != '\x01') {
            utag = utag * 10u + static_cast<unsigned>(*p - '0');
            ++p;
        }
        if (p >= stop)
            break;
        int const tag = static_cast<int>(utag);
        if (*p == '\x01') {
            write_slot(tag, p, p);
            ++p;
            continue;
        }
        ++p;
        char const* const vb = p;
        char const* const ve = d::find_soh(p, stop);
        if (d::is_tag_a_data_length(tag)) {
            std::size_t data_len = 0;
            if (!d::try_atou<std::size_t>(vb, ve, data_len))
                break;
            p = ve + 1;
            unsigned unext = 0;
            while (p < stop && *p != '=') {
                unext = unext * 10u + static_cast<unsigned>(*p - '0');
                ++p;
            }
            if (p >= stop)
                break;
            ++p;
            // Iterator semantics: the length must fit inside the body and be
            // SOH-terminated, else the offset cannot be trusted.
            if (data_len >= static_cast<std::size_t>(stop - p) || p[data_len] != '\x01')
                break;
            char const* const dvb = p;
            char const* const dve = p + data_len;
            write_slot(static_cast<int>(unext), dvb, dve);
            p = dve + 1;
            continue;
        }
        write_slot(tag, vb, ve);
        p = ve + 1;
    }
    return out;
}

template <std::size_t N>
void expect_index_matches_reference(nanofix::message_reader const& r, char const* where) {
    RefIndex const ref = ref_build_field_index<N>(r);
    nanofix::field_index_buffer<N> ib;
    auto const idx = nanofix::build_field_index(r, ib);

    ASSERT_EQ(idx.truncated(), ref.truncated) << where << " N=" << N;
    if (ref.truncated)
        return;
    ASSERT_EQ(idx.field_count(), ref.tags.size()) << where << " N=" << N;
    for (std::size_t i = 0; i < ref.tags.size(); ++i) {
        EXPECT_EQ(idx.tag_at(i), ref.tags[i]) << where << " field#" << i << " N=" << N;
        auto const v = idx.value_at(i);
        auto const pos = static_cast<std::uint64_t>(v.begin() - idx.message_begin());
        auto const len = static_cast<std::uint64_t>(v.end() - v.begin());
        EXPECT_EQ(pos, ref.pos[i]) << where << " field#" << i << " N=" << N;
        EXPECT_EQ(len, ref.len[i]) << where << " field#" << i << " N=" << N;
    }
}

}  // namespace

class BulkFramingDiffTest : public ::testing::TestWithParam<char const*> {};

TEST_P(BulkFramingDiffTest, MatchesPerFieldReference) {
    std::filesystem::path path = std::filesystem::path(NANOFIX_TEST_DATA_DIR) / GetParam();
    auto data = read_file(path);
    ASSERT_FALSE(data.empty()) << path;

    std::size_t complete = 0;
    for (nanofix::message_reader r(data.data(), data.data() + data.size()); r.is_complete();
         r = r.next_message_reader()) {
        ++complete;
        // Large N: no truncation, full (tag,pos,len) compare.
        expect_index_matches_reference<2048>(r, GetParam());
        // Small N: force the count==N truncation boundary, both paths must trip
        // truncated() at the same field and return the empty index. (The size
        // guard, message_size() > ~4 GiB, is mirrored in the reference but never
        // hit: no fixture is that large.)
        expect_index_matches_reference<4>(r, GetParam());
        expect_index_matches_reference<7>(r, GetParam());
        expect_index_matches_reference<8>(r, GetParam());
    }
    EXPECT_GT(complete, 0u) << path;
}

TEST(BulkFramingDiff, LyingDataLengthMatchesReference) {
    // Declared data length lands mid-value (not SOH-terminated): both the
    // reference and the bulk path must stop at the same field.
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=23\x01"
        "35=A\x01"
        "93=2\x01"
        "89=abcd\x01"
        "58=x\x01"
        "10=000\x01";
    nanofix::message_reader r(buf, buf + sizeof(buf) - 1);
    ASSERT_TRUE(r.is_complete() && r.is_valid());
    expect_index_matches_reference<2048>(r, "inline:lying-data-length");
    expect_index_matches_reference<4>(r, "inline:lying-data-length");
}

INSTANTIATE_TEST_SUITE_P(SampleData,
                         BulkFramingDiffTest,
                         ::testing::Values("fix.4.1.set.1",
                                           "fix.4.2.malformed.set.1",
                                           "fix.5.0.set.1",
                                           "fix.5.0.set.2",
                                           "fix.binary.with.soh.set.1"));
