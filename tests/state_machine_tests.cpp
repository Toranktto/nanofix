// Corrupt and truncated frames must classify safely and never walk the iterator
// past the buffer.

#include <gtest/gtest.h>

#include <nanofix.hpp>

#include <cstddef>
#include <string>

using namespace nanofix;

namespace {
struct corruption_outcome {
    bool complete;
    bool valid;
    std::size_t fields_seen;
    parse_error error;
};

corruption_outcome probe(char const* buf, std::size_t n) {
    message_reader r(buf, buf + n);
    corruption_outcome out{r.is_complete(), r.is_valid(), 0, r.error()};
    if (!r.is_valid() || !r.is_complete())
        return out;
    for (auto it = r.begin(); it != r.end(); ++it) {
        if (++out.fields_seen > 4096) {
            ADD_FAILURE() << "iterator failed to terminate on corrupted message";
            break;
        }
    }
    return out;
}
}  // namespace

TEST(StateMachineCorruption, empty_buffer) {
    auto o = probe("", 0);
    EXPECT_FALSE(o.complete);
    EXPECT_EQ(o.fields_seen, 0u);
}

TEST(StateMachineCorruption, single_byte) {
    char const buf[] = "8";
    auto o = probe(buf, 1);
    EXPECT_FALSE(o.complete);
}

TEST(StateMachineCorruption, begin_string_truncated_no_soh) {
    char const buf[] = "8=FIX.4.2";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_FALSE(o.complete);
}

TEST(StateMachineCorruption, begin_string_only_then_soh_no_body_length) {
    char const buf[] = "8=FIX.4.2\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_FALSE(o.complete);
}

TEST(StateMachineCorruption, body_length_tag_without_value) {
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_FALSE(o.complete && o.valid);
}

TEST(StateMachineCorruption, body_length_non_numeric) {
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=abc\x01"
        "35=D\x01"
        "49=A\x01"
        "56=B\x01"
        "10=000\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_FALSE(o.valid);
    EXPECT_EQ(o.error, parse_error::body_length_not_numeric);
}

TEST(StateMachineCorruption, body_length_overflows_into_buffer_end) {
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=999999\x01"
        "35=D\x01"
        "49=A\x01"
        "10=000\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_FALSE(o.complete);
}

TEST(StateMachineCorruption, body_length_understated) {
    // BodyLength=5 but actual body is longer; CheckSum tag is not at the
    // location the header advertised.
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=5\x01"
        "35=D\x01"
        "49=ALICE\x01"
        "56=BOB\x01"
        "10=000\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_FALSE(o.valid && o.complete);
}

TEST(StateMachineCorruption, no_msg_type_after_body_length) {
    // Spec requires tag 35 immediately after tag 9.
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=12\x01"
        "49=A\x01"
        "56=B\x01"
        "10=000\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_FALSE(o.valid);
    EXPECT_EQ(o.error, parse_error::msg_type_tag_missing);
}

TEST(StateMachineCorruption, soh_only_buffer) {
    char const buf[] = "\x01\x01\x01\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_FALSE(o.valid && o.complete);
}

TEST(StateMachineCorruption, consecutive_sohs_inside_message) {
    // Empty field value mid-message (two SOHs in a row).
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=00000018\x01"
        "35=D\x01"
        "49=\x01"
        "56=B\x01"
        "10=000\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    if (o.valid && o.complete) {
        EXPECT_LE(o.fields_seen, 16u);
    }
}

TEST(StateMachineCorruption, tag_without_equals_sign) {
    // Digits then non-'=' before SOH.
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=00000020\x01"
        "35=D\x01"
        "49ALICE\x01"
        "56=B\x01"
        "10=000\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_LE(o.fields_seen, 16u);
}

TEST(StateMachineCorruption, non_digit_in_tag_number) {
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=00000018\x01"
        "35=D\x01"
        "4a=A\x01"
        "56=B\x01"
        "10=000\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_LE(o.fields_seen, 16u);
}

TEST(StateMachineCorruption, raw_data_length_overshoots_buffer) {
    // RawDataLength=999 but only a few bytes follow.
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=00000035\x01"
        "35=n\x01"
        "49=A\x01"
        "56=B\x01"
        "95=999\x01"
        "96=hi\x01"
        "10=000\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_LE(o.fields_seen, 32u);
}

TEST(StateMachineCorruption, raw_data_length_negative_like) {
    // Negative-looking length ('-' rejected by atou, becomes 0).
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=00000030\x01"
        "35=n\x01"
        "49=A\x01"
        "56=B\x01"
        "95=-1\x01"
        "96=x\x01"
        "10=000\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_LE(o.fields_seen, 32u);
}

TEST(StateMachineCorruption, raw_data_length_huge_value) {
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=00000060\x01"
        "35=n\x01"
        "49=A\x01"
        "56=B\x01"
        "95=18446744073709551610\x01"
        "96=hi\x01"
        "10=000\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_LE(o.fields_seen, 32u);
}

TEST(StateMachineCorruption, two_back_to_back_headers) {
    std::string blob;
    blob +=
        "8=FIX.4.2\x01"
        "9=000010\x01"
        "35=A\x01"
        "10=000\x01";
    blob +=
        "8=FIX.4.2\x01"
        "9=000010\x01"
        "35=A\x01"
        "10=000\x01";
    int seen = 0;
    for (auto const& r : messages(blob.data(), blob.size())) {
        (void)r;
        if (++seen > 8) {
            FAIL() << "messages() did not terminate";
        }
    }
    EXPECT_LE(seen, 8);
}

TEST(StateMachineCorruption, header_8_fix_with_no_following_bytes) {
    char const buf[] = "8=FIX.";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_FALSE(o.complete);
}

TEST(StateMachineCorruption, messages_range_over_only_garbage) {
    char const buf[] = "completely_random_bytes_no_fix_anywhere_here\x01\x02\x03";
    int seen = 0;
    for (auto const& r : messages(buf, sizeof(buf) - 1)) {
        (void)r;
        if (++seen > 4) {
            FAIL() << "resync over garbage did not terminate";
        }
    }
    EXPECT_EQ(seen, 0);
}

TEST(StateMachineCorruption, body_length_with_leading_zeros_overflow) {
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=000500\x01"
        "35=D\x01"
        "49=A\x01"
        "56=B\x01"
        "10=000\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_FALSE(o.complete);
}

TEST(StateMachineCorruption, indexed_path_on_corrupt_message_is_safe) {
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=abc\x01"
        "35=D\x01"
        "49=A\x01"
        "10=000\x01";
    message_reader r(buf, buf + sizeof(buf) - 1);
    ASSERT_FALSE(r.is_valid());
    nanofix::field_index_buffer<32> idx;
    auto im = nanofix::build_field_index(r, idx);
    // An invalid frame indexes to nothing rather than walking corrupt bytes.
    EXPECT_EQ(im.field_count(), 0u);
    EXPECT_FALSE(im.truncated());
}
