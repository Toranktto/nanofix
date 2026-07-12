#include <gtest/gtest.h>

#include <nanofix.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "test_common.hpp"

#ifndef NANOFIX_TEST_DATA_DIR
#error "NANOFIX_TEST_DATA_DIR must be set by the build system"
#endif

using nanofix_test::read_file;

namespace {

struct Message {
    std::string version;
    std::vector<std::pair<int, std::string>> fields;
};

std::vector<Message> parse_all(char const* begin, char const* end) {
    std::vector<Message> out;
    for (nanofix::message_reader r(begin, end); r.is_complete(); r = r.next_message_reader()) {
        if (!r.is_valid())
            continue;
        Message m;
        m.version.assign(r.prefix_begin(), r.prefix_end());
        for (auto it = r.begin(); it != r.end(); ++it) {
            m.fields.emplace_back(it->tag(), std::string(it->value().begin(), it->value().end()));
        }
        out.push_back(std::move(m));
    }
    return out;
}

// Spec convention data_tag = length_tag + 1, with Signature(89) the exception.
int length_tag_for_data_tag(int data_tag) {
    if (data_tag == nanofix::tag::Signature)
        return nanofix::tag::SignatureLength;
    int const candidate = data_tag - 1;
    if (nanofix::detail::is_tag_a_data_length(candidate))
        return candidate;
    return 0;
}

std::vector<char> reserialize(std::vector<Message> const& msgs) {
    std::vector<char> out;
    char buf[1 << 16];
    std::size_t count = 0;
    for (auto const& m : msgs) {
        nanofix::message_writer w(buf, buf + sizeof(buf));
        w.push_back_header(m.version);
        for (std::size_t i = 0; i < m.fields.size(); ++i) {
            int const tag = m.fields[i].first;
            std::string const& val = m.fields[i].second;
            if (tag == nanofix::tag::BeginString || tag == nanofix::tag::BodyLength ||
                tag == nanofix::tag::CheckSum) {
                continue;
            }
            if (int const length_tag = length_tag_for_data_tag(tag); length_tag != 0) {
                w.push_back_data(length_tag, tag, val.data(), val.data() + val.size());
                continue;
            }
            w.push_back_string(tag, val);
        }
        if (!w.push_back_trailer()) {
            ADD_FAILURE() << "writer buffer overflow at message N=" << count;
            return out;
        }
        out.insert(out.end(), buf, w.message_end());
        ++count;
    }
    return out;
}

}  // namespace

class SelfRoundTripTest : public ::testing::TestWithParam<char const*> {};

TEST_P(SelfRoundTripTest, AllMessagesValidWithMatchingChecksum) {
    std::filesystem::path path = std::filesystem::path(NANOFIX_TEST_DATA_DIR) / GetParam();
    auto data = read_file(path);
    ASSERT_FALSE(data.empty()) << path;

    std::size_t total = 0, valid = 0, bad_checksum = 0;
    for (nanofix::message_reader r(data.data(), data.data() + data.size()); r.is_complete();
         r = r.next_message_reader()) {
        ++total;
        if (!r.is_valid())
            continue;
        ++valid;
        unsigned char want = r.check_sum()->value().as_int_unchecked<unsigned char>();
        unsigned char got = r.calculate_check_sum();
        if (want != got)
            ++bad_checksum;
    }

    EXPECT_GT(total, 0u);
    EXPECT_EQ(valid, total) << path;
    EXPECT_EQ(bad_checksum, 0u) << path;
}

TEST_P(SelfRoundTripTest, RoundTripPreservesTagStream) {
    std::filesystem::path path = std::filesystem::path(NANOFIX_TEST_DATA_DIR) / GetParam();
    auto data = read_file(path);
    ASSERT_FALSE(data.empty()) << path;

    auto first = parse_all(data.data(), data.data() + data.size());
    auto wire2 = reserialize(first);
    auto second = parse_all(wire2.data(), wire2.data() + wire2.size());

    ASSERT_EQ(first.size(), second.size()) << path;
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first[i].version, second[i].version) << "msg #" << i << " in " << path;
        ASSERT_EQ(first[i].fields.size(), second[i].fields.size())
            << "msg #" << i << " field count in " << path;
        for (std::size_t j = 0; j < first[i].fields.size(); ++j) {
            EXPECT_EQ(first[i].fields[j].first, second[i].fields[j].first)
                << "msg #" << i << " field #" << j << " tag in " << path;
            EXPECT_EQ(first[i].fields[j].second, second[i].fields[j].second)
                << "msg #" << i << " field #" << j << " value in " << path;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(SampleData,
                         SelfRoundTripTest,
                         ::testing::Values("fix.4.1.set.1",
                                           "fix.5.0.set.1",
                                           "fix.5.0.set.2",
                                           "fix.binary.with.soh.set.1"));

TEST(SelfRoundTripBinary, EmbeddedSohPreserved) {
    std::filesystem::path path =
        std::filesystem::path(NANOFIX_TEST_DATA_DIR) / "fix.binary.with.soh.set.1";
    auto data = read_file(path);
    ASSERT_FALSE(data.empty()) << path;

    auto parsed = parse_all(data.data(), data.data() + data.size());
    ASSERT_EQ(parsed.size(), 1u);

    bool found = false;
    for (auto const& [tag, val] : parsed[0].fields) {
        if (tag == nanofix::tag::RawData) {
            found = true;
            EXPECT_NE(val.find('\x01'), std::string::npos)
                << "fixture must contain an embedded SOH in the binary payload";
        }
    }
    EXPECT_TRUE(found) << "RawData missing from fixture";

    auto wire2 = reserialize(parsed);
    auto parsed2 = parse_all(wire2.data(), wire2.data() + wire2.size());
    ASSERT_EQ(parsed2.size(), 1u);
    ASSERT_EQ(parsed[0].fields.size(), parsed2[0].fields.size());
    for (std::size_t j = 0; j < parsed[0].fields.size(); ++j) {
        EXPECT_EQ(parsed[0].fields[j].first, parsed2[0].fields[j].first);
        EXPECT_EQ(parsed[0].fields[j].second, parsed2[0].fields[j].second);
    }
}

TEST(Malformed, ClassifiesValidInvalidAndIncomplete) {
    std::filesystem::path path =
        std::filesystem::path(NANOFIX_TEST_DATA_DIR) / "fix.4.2.malformed.set.1";
    auto data = read_file(path);
    ASSERT_FALSE(data.empty()) << path;

    std::size_t total_complete = 0, valid = 0, invalid = 0, bad_checksum = 0;
    for (nanofix::message_reader r(data.data(), data.data() + data.size()); r.is_complete();
         r = r.next_message_reader()) {
        ++total_complete;
        if (!r.is_valid()) {
            ++invalid;
            continue;
        }
        ++valid;
        unsigned char want = r.check_sum()->value().as_int_unchecked<unsigned char>();
        unsigned char got = r.calculate_check_sum();
        if (want != got)
            ++bad_checksum;
    }

    EXPECT_EQ(valid, 7u) << path;
    EXPECT_EQ(invalid, 2u) << path;
    EXPECT_EQ(bad_checksum, 1u) << path;
    // Hand-counted: 9 complete frames (the 10th is truncated, stopping
    // iteration). Pins framing/resync beyond the per-bucket sums.
    EXPECT_EQ(total_complete, 9u) << path;
}
