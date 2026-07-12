#include <gtest/gtest.h>

#include <nanofix.hpp>
#include <nanofix/names.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace std::literals::string_view_literals;
using namespace nanofix;

TEST(NanofixTest, basic) {
    char b[1024];
    message_writer w(b);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "A");
    ASSERT_TRUE(w.push_back_trailer());
    message_reader r(b);

    EXPECT_EQ(w.message_size(), r.message_size());

    message_reader::const_iterator i = r.begin();
    EXPECT_TRUE(r.find_with_hint(tag::MsgType, i));
    EXPECT_TRUE(i->value() == "A");
    EXPECT_TRUE(i->value() == (char const*)"A");
    EXPECT_TRUE(i->value() != "B");
    EXPECT_TRUE(i->value() == "A"sv);
    EXPECT_TRUE(i->value() != "B"sv);

    {
        message_writer w_err(b);
        EXPECT_FALSE(w_err.push_back_trailer<false>());
        EXPECT_TRUE(w_err.has_error());
    }
    {
        message_writer w_err(b);
        w_err.push_back_header("FIXT.1.1");
        w_err.push_back_header("FIXT.1.1");
        EXPECT_TRUE(w_err.has_error());
    }
}

template <typename F>
int test_bound_checking(F f) {
    char buffer[101];
    int minimum_size = 101;
    for (int i = 100; i >= 0; --i) {
        message_writer writer(buffer, buffer + i);
        buffer[i] = '\x55';
        f(writer);
        if (writer.ok()) {
            EXPECT_EQ(minimum_size, i + 1);
            minimum_size = i;
        }
        EXPECT_EQ(buffer[i], '\x55');
    }
    return minimum_size;
}

TEST(NanofixTest, push_back_decimal_negative_exponent_reserves_fraction) {
    using namespace nanofix;
    // A deep negative exponent makes dtoa emit ~|exponent| digits. Tight buffer
    // must error, not overflow; a wide one writes it and round-trips.
    char tight[48];
    message_writer wt(tight, sizeof(tight));
    wt.push_back_header("FIXT.1.1");
    wt.push_back_string(tag::MsgType, "D");
    wt.push_back_decimal(tag::Price, 1, -1000);  // ~1000 fractional bytes: cannot fit
    EXPECT_FALSE(wt.ok());
    EXPECT_FALSE(wt.push_back_trailer());

    // Ample buffer: the same shape fits and round-trips.
    char wide[2048];
    message_writer ww(wide, sizeof(wide));
    ww.push_back_header("FIXT.1.1");
    ww.push_back_string(tag::MsgType, "D");
    ww.push_back_decimal(tag::Price, 1, -100);
    ASSERT_TRUE(ww.ok());
    ASSERT_TRUE(ww.push_back_trailer());

    message_reader r(ww);
    ASSERT_TRUE(r.is_valid());
    auto it = r.begin();
    ASSERT_TRUE(r.find_with_hint(tag::Price, it));
    std::int64_t m = 0, e = 0;
    ASSERT_TRUE(it->value().try_as_decimal<std::int64_t>(m, e));
    EXPECT_EQ(m, 1);
    EXPECT_EQ(e, -100);
}

TEST(NanofixTest, message_writer_bounds) {
    using W = message_writer;
    EXPECT_EQ(test_bound_checking([](W& w) { w.push_back_header("FIXT.1.1"); }), 20);
    EXPECT_EQ(test_bound_checking([](W& w) {
                  w.push_back_header("FIXT.1.1");
                  (void)w.push_back_trailer<false>();
              }),
              27);
    EXPECT_EQ(test_bound_checking([](W& w) {
                  w.push_back_header("FIXT.1.1");
                  (void)w.push_back_trailer<true>();
              }),
              27);

    auto const test_string = "string literal";
    EXPECT_EQ(
        test_bound_checking([&](W& w) { w.push_back_string(58, test_string, test_string + 14); }),
        27);
    EXPECT_EQ(test_bound_checking([&](W& w) { w.push_back_string(58, std::string(test_string)); }),
              27);
    EXPECT_EQ(
        test_bound_checking([&](W& w) { w.push_back_string(58, std::string_view(test_string)); }),
        27);
    EXPECT_EQ(test_bound_checking([&](W& w) { w.push_back_char(58, 'a'); }), 14);
    EXPECT_EQ(test_bound_checking([&](W& w) { w.push_back_bool(58, true); }), 14);
    EXPECT_EQ(test_bound_checking([&](W& w) { w.push_back_int(58, 55); }), 24);
    EXPECT_EQ(test_bound_checking([&](W& w) { w.push_back_decimal(58, 123456, -3); }), 25);
    EXPECT_EQ(test_bound_checking([&](W& w) { w.push_back_decimal(58, 123456, 0); }), 25);
    EXPECT_EQ(test_bound_checking([&](W& w) { w.push_back_date(58, 1970, 1, 1); }), 21);
    EXPECT_EQ(test_bound_checking([&](W& w) { w.push_back_monthyear(58, 1970, 1); }), 19);
    EXPECT_EQ(test_bound_checking([&](W& w) { w.push_back_timeonly(58, 23, 59, 59, 999); }), 25);
    EXPECT_EQ(
        test_bound_checking([&](W& w) { w.push_back_timeonly_nano(58, 23, 59, 59, 999999999); }), 31);
    EXPECT_EQ(test_bound_checking([&](W& w) { w.push_back_timestamp(58, 1970, 1, 1, 23, 59, 59); }),
              30);
    EXPECT_EQ(
        test_bound_checking([&](W& w) { w.push_back_timestamp(58, 1970, 1, 1, 23, 59, 59, 999); }),
        34);
    EXPECT_EQ(test_bound_checking(
                  [&](W& w) { w.push_back_timestamp_nano(58, 1970, 1, 1, 23, 59, 59, 999999999); }),
              40);
    EXPECT_EQ(
        test_bound_checking([&](W& w) { w.push_back_data(58, 59, test_string, test_string + 14); }),
        51);
}

static void test_checksum(message_writer& mw, char const (&expected)[4]) {
    ASSERT_TRUE(mw.push_back_trailer());
    char const* end = mw.message_end();

    EXPECT_EQ(end[-4], expected[0]);
    EXPECT_EQ(end[-3], expected[1]);
    EXPECT_EQ(end[-2], expected[2]);
}

TEST(NanofixTest, checksum_empty) {
    char buffer[50] = {};
    message_writer writer(buffer);
    writer.push_back_header("FIXT.1.1");
    test_checksum(writer, "006");
}

TEST(NanofixTest, checksum) {
    char buffer[50] = {};
    message_writer writer(buffer);
    writer.push_back_header("FIXT.1.1");
    writer.push_back_decimal(58, 123, 0);
    test_checksum(writer, "078");
}

TEST(NanofixTest, checksum_negative) {
    char buffer[50] = {};
    message_writer writer(buffer);
    writer.push_back_header("FIXT.1.1");
    writer.push_back_char(58, '\x80');
    test_checksum(writer, "054");
}

TEST(NanofixTest, checksum_calc) {
    char buffer[50] = {};
    message_writer writer(buffer);
    writer.push_back_header("FIXT.1.1");
    writer.push_back_string(tag::MsgType, "A");
    ASSERT_TRUE(writer.push_back_trailer());

    message_reader mr(writer);
    EXPECT_EQ(mr.calculate_check_sum(), mr.check_sum()->value().as_int_unchecked<unsigned char>());
}

TEST(NanofixTest, null_field_value) {
    char buffer[50] = {};
    message_writer writer(buffer);
    writer.push_back_header("FIXT.1.1");
    writer.push_back_string(nanofix::tag::MsgType, "A");
    writer.push_back_string(37, "");
    writer.push_back_string(38, "whatever");
    ASSERT_TRUE(writer.push_back_trailer());

    message_reader reader(writer);
    message_reader::const_iterator i = reader.begin();
    EXPECT_TRUE(reader.find_with_hint(38, i));
}

TEST(NanofixTest, data_length) {
    char buffer[100] = {};
    message_writer writer(buffer);
    std::string datum("datum");
    writer.push_back_header("FIXT.1.1");
    writer.push_back_string(tag::MsgType, "A");
    writer.push_back_data(tag::RawDataLength, tag::RawData, &*datum.begin(), &*datum.end());
    writer.push_back_data(tag::EncodedUnderlyingProvisionTextLen,
                          tag::EncodedUnderlyingProvisionText,
                          &*datum.begin(),
                          &*datum.end());
    ASSERT_TRUE(writer.push_back_trailer());

    message_reader reader(writer);
    message_reader::const_iterator i;

    i = reader.begin();
    ASSERT_FALSE(reader.find_with_hint(tag::RawDataLength, i));

    i = reader.begin();
    ASSERT_FALSE(reader.find_with_hint(tag::EncodedUnderlyingProvisionTextLen, i));

    i = reader.begin();
    ASSERT_TRUE(reader.find_with_hint(tag::RawData, i));
    ASSERT_TRUE(i != reader.end());
    EXPECT_EQ(i->value().as_string_view(), datum);

    ASSERT_TRUE(reader.find_with_hint(tag::EncodedUnderlyingProvisionText, i));
    ASSERT_TRUE(i != reader.end());
    EXPECT_EQ(i->value().as_string_view(), datum);
}

TEST(NanofixTest, field_value_bytes_span) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_string(tag::SenderCompID, "ALICE");
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    auto it = r.begin();
    ASSERT_TRUE(r.find_with_hint(tag::SenderCompID, it));

    std::span<char const> b = it->value().bytes();
    EXPECT_EQ(b.size(), it->value().size());
    EXPECT_EQ(b.data(), it->value().begin());
    EXPECT_EQ(std::string_view(b.data(), b.size()), "ALICE");
}

TEST(NanofixTest, field_value_try_as_char) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_string(tag::Side, "1");      // char field, 1 byte
    w.push_back_string(tag::Symbol, "ABC");  // 3 bytes, invalid char
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    auto it = r.begin();

    // valid single byte.
    ASSERT_TRUE(r.find_with_hint(tag::Side, it));
    char side = 0;
    ASSERT_TRUE(it->value().try_as_char(side));
    EXPECT_EQ(side, '1');

    // multi-byte rejected (FIX char is exactly one byte), out untouched.
    ASSERT_TRUE(r.find_with_hint(tag::Symbol, it));
    char bad = 'Z';
    EXPECT_FALSE(it->value().try_as_char(bad));
    EXPECT_EQ(bad, 'Z');

    // empty (a miss) rejected, no UB.
    char miss = 'Q';
    EXPECT_FALSE(r.find(tag::ClOrdID).value().try_as_char(miss));
    EXPECT_EQ(miss, 'Q');

    // generic try_as<char> delegates -> also rejects multi-byte.
    ASSERT_TRUE(r.find_with_hint(tag::Symbol, it));
    char gc = 'Z';
    EXPECT_FALSE(it->value().try_as(gc));

    // typed_value gated to character category.
    char tv = 0;
    ASSERT_TRUE(r.find(tag::Side).try_as_char(tv));
    EXPECT_EQ(tv, '1');
}

TEST(NanofixTest, field_value_generic_try_as) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_int(tag::MsgSeqNum, 4242);
    w.push_back_decimal(tag::Price, 4925604, -2);  // 49256.04
    w.push_back_string(tag::Symbol, "X");          // single char
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    auto it = r.begin();

    // integral: dispatches to try_as_int, deduced from out.
    ASSERT_TRUE(r.find_with_hint(tag::MsgSeqNum, it));
    std::uint32_t seq = 0;
    ASSERT_TRUE(it->value().try_as(seq));
    EXPECT_EQ(seq, 4242u);

    // string_view: never fails, copies the view.
    ASSERT_TRUE(r.find_with_hint(tag::MsgType, it));
    std::string_view sv;
    ASSERT_TRUE(it->value().try_as(sv));
    EXPECT_EQ(sv, "D");

    // char: delegates to try_as_char; exactly one byte.
    ASSERT_TRUE(r.find_with_hint(tag::Symbol, it));
    char c = 0;
    ASSERT_TRUE(it->value().try_as(c));
    EXPECT_EQ(c, 'X');

    // decimal_parts<>: default int64_t mantissa/exponent.
    ASSERT_TRUE(r.find_with_hint(tag::Price, it));
    decimal_parts<> d{};
    ASSERT_TRUE(it->value().try_as(d));
    EXPECT_EQ(d.mantissa, 4925604);
    EXPECT_EQ(d.exponent, -2);

    // decimal_parts<int32_t>: generic over Int.
    decimal_parts<std::int32_t> d32{};
    ASSERT_TRUE(it->value().try_as(d32));
    EXPECT_EQ(d32.mantissa, 4925604);
    EXPECT_EQ(d32.exponent, -2);

    // non-numeric integral / decimal reject; out untouched.
    ASSERT_TRUE(r.find_with_hint(tag::MsgType, it));
    std::uint32_t bad = 7;
    EXPECT_FALSE(it->value().try_as(bad));
    EXPECT_EQ(bad, 7u);
    decimal_parts<> dbad{99, 99};
    EXPECT_FALSE(it->value().try_as(dbad));
    EXPECT_EQ(dbad.mantissa, 99);
}

TEST(NanofixTest, field_value_bool) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_string(tag::PossDupFlag, "Y");  // Boolean true
    w.push_back_string(tag::PossResend, "N");   // Boolean false
    w.push_back_string(tag::Symbol, "X");       // not Y/N
    w.push_back_string(tag::Account, "YY");     // 2 bytes, invalid bool
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    auto it = r.begin();

    // field_value: validating Y/N.
    ASSERT_TRUE(r.find_with_hint(tag::PossDupFlag, it));
    bool y = false;
    ASSERT_TRUE(it->value().try_as_bool(y));
    EXPECT_TRUE(y);
    EXPECT_TRUE(it->value().as_bool_unchecked());

    ASSERT_TRUE(r.find_with_hint(tag::PossResend, it));
    bool n = true;
    ASSERT_TRUE(it->value().try_as_bool(n));
    EXPECT_FALSE(n);
    EXPECT_FALSE(it->value().as_bool_unchecked());

    // Invalid: not Y/N, and wrong length -> reject, out untouched.
    ASSERT_TRUE(r.find_with_hint(tag::Symbol, it));
    bool bad = true;
    EXPECT_FALSE(it->value().try_as_bool(bad));
    EXPECT_TRUE(bad);
    ASSERT_TRUE(r.find_with_hint(tag::Account, it));
    EXPECT_FALSE(it->value().try_as_bool(bad));

    // generic try_as<bool> / as_unchecked<bool> route to Y/N, not int parse.
    ASSERT_TRUE(r.find_with_hint(tag::PossDupFlag, it));
    bool gb = false;
    ASSERT_TRUE(it->value().try_as(gb));
    EXPECT_TRUE(gb);
    EXPECT_TRUE(it->value().as_unchecked<bool>());

    // typed_value: gated to character category.
    nanofix::field_index_buffer<32> ib;
    auto idx = nanofix::build_field_index(r, ib);
    bool tv = false;
    ASSERT_TRUE(idx.find(tag::PossDupFlag).try_as_bool(tv));
    EXPECT_TRUE(tv);
}

TEST(NanofixTest, field_value_generic_as_unchecked) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_int(tag::MsgSeqNum, 4242);
    w.push_back_decimal(tag::Price, 4925604, -2);
    w.push_back_string(tag::Symbol, "X");
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    auto it = r.begin();

    ASSERT_TRUE(r.find_with_hint(tag::MsgSeqNum, it));
    EXPECT_EQ(it->value().as_unchecked<std::uint32_t>(), 4242u);

    ASSERT_TRUE(r.find_with_hint(tag::MsgType, it));
    EXPECT_EQ(it->value().as_unchecked<std::string_view>(), "D");

    ASSERT_TRUE(r.find_with_hint(tag::Symbol, it));
    EXPECT_EQ(it->value().as_unchecked<char>(), 'X');

    ASSERT_TRUE(r.find_with_hint(tag::Price, it));
    auto d = it->value().as_unchecked<decimal_parts<>>();
    EXPECT_EQ(d.mantissa, 4925604);
    EXPECT_EQ(d.exponent, -2);

    auto d32 = it->value().as_unchecked<decimal_parts<std::int32_t>>();
    EXPECT_EQ(d32.mantissa, 4925604);
    EXPECT_EQ(d32.exponent, -2);
}

TEST(NanofixTest, field_value_date_and_monthyear) {
    using namespace nanofix;
    char buf[256];
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_date(tag::SettlDate, 2024, 1, 15);           // YYYYMMDD
    w.push_back_monthyear(tag::MaturityMonthYear, 2024, 3);  // YYYYMM
    w.push_back_string(tag::Symbol, "2024");                 // wrong length for both
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    auto it = r.begin();

    ASSERT_TRUE(r.find_with_hint(tag::SettlDate, it));
    int y = 0, m = 0, d = 0;
    ASSERT_TRUE(it->value().as_date(y, m, d));
    EXPECT_EQ(y, 2024);
    EXPECT_EQ(m, 1);
    EXPECT_EQ(d, 15);

    ASSERT_TRUE(r.find_with_hint(tag::MaturityMonthYear, it));
    int my = 0, mm = 0;
    ASSERT_TRUE(it->value().as_monthyear(my, mm));
    EXPECT_EQ(my, 2024);
    EXPECT_EQ(mm, 3);

    // Wrong length: both reject, out-params untouched.
    ASSERT_TRUE(r.find_with_hint(tag::Symbol, it));
    int yy = -1, mo = -1, dd = -1;
    EXPECT_FALSE(it->value().as_date(yy, mo, dd));
    EXPECT_FALSE(it->value().as_monthyear(yy, mo));
    EXPECT_EQ(yy, -1);  // doc: out-params unmodified on false
}

TEST(NanofixTest, find_all_soh_matches_scalar) {
    std::mt19937 rng(123);
    for (int trial = 0; trial < 200; ++trial) {
        std::size_t len = rng() % 200;
        std::vector<char> buf(len);
        for (auto& c : buf)
            c = (rng() % 5 == 0) ? '\x01' : static_cast<char>('A' + (rng() % 26));
        std::vector<std::uint32_t> expected;
        for (std::size_t i = 0; i < len; ++i)
            if (buf[i] == '\x01')
                expected.push_back(static_cast<std::uint32_t>(i));
        std::vector<std::uint32_t> got(len ? len : 1);
        std::size_t n =
            nanofix::detail::find_all_soh(buf.data(), buf.data() + len, got.data(), got.size());
        got.resize(n);
        EXPECT_EQ(got, expected) << "trial " << trial << " len " << len;

        // Above compares the dispatched impl; pin the scalar fallback directly
        // so a scalar bug can't hide on a SIMD build.
        std::vector<std::uint32_t> got_scalar(len ? len : 1);
        std::size_t ns = nanofix::detail::find_all_soh_scalar(
            buf.data(), buf.data() + len, got_scalar.data(), got_scalar.size());
        got_scalar.resize(ns);
        EXPECT_EQ(got, got_scalar) << "dispatched vs scalar, trial " << trial;

        // cap < match count: capped offsets and count must match scalar exactly.
        if (!expected.empty()) {
            std::size_t const cap = expected.size() / 2;
            std::vector<std::uint32_t> got_cap(cap ? cap : 1);
            std::vector<std::uint32_t> got_cap_scalar(cap ? cap : 1);
            std::size_t const nc =
                nanofix::detail::find_all_soh(buf.data(), buf.data() + len, got_cap.data(), cap);
            std::size_t const ncs = nanofix::detail::find_all_soh_scalar(
                buf.data(), buf.data() + len, got_cap_scalar.data(), cap);
            ASSERT_EQ(nc, ncs) << "capped count, trial " << trial;
            got_cap.resize(nc);
            got_cap_scalar.resize(ncs);
            EXPECT_EQ(got_cap, got_cap_scalar) << "capped offsets, trial " << trial;
            EXPECT_TRUE(std::equal(got_cap.begin(), got_cap.end(), expected.begin()))
                << "capped prefix, trial " << trial;
        }
    }
}

// Sizes cross the 8-wide and 4-wide vector loops plus the scalar tail; targets
// mix hit-first / hit-repeated / miss. First-occurrence semantics must match
// the scalar reference exactly.
TEST(NanofixTest, find_tag_in_index_matches_scalar) {
    std::mt19937 rng(456);
    for (int trial = 0; trial < 400; ++trial) {
        std::size_t const n = rng() % 70;
        std::vector<int> tags(n);
        for (auto& t : tags)
            t = 1 + static_cast<int>(rng() % 12);             // small range forces duplicates
        int const target = 1 + static_cast<int>(rng() % 14);  // sometimes absent
        std::size_t expected = n;
        for (std::size_t i = 0; i < n; ++i)
            if (tags[i] == target) {
                expected = i;
                break;
            }
        EXPECT_EQ(nanofix::detail::find_tag_in_index(tags.data(), n, target), expected)
            << "trial " << trial << " n " << n << " target " << target;
        EXPECT_EQ(nanofix::detail::find_tag_in_index_scalar(tags.data(), n, target), expected)
            << "scalar reference, trial " << trial;
    }
}

// Sizes straddle every vector threshold: the AVX2 128/32-byte strides and the
// NEON 1024-byte scalar cutoff (below it the NEON build never runs vector
// code, so >= 1024 is the only size that exercises it).
TEST(NanofixTest, checksum_bytes_matches_scalar) {
    std::mt19937 rng(789);
    constexpr std::size_t kSizes[] = {0,
                                      1,
                                      15,
                                      16,
                                      17,
                                      63,
                                      64,
                                      65,
                                      127,
                                      128,
                                      129,
                                      255,
                                      256,
                                      257,
                                      287,
                                      288,  // AVX2 256-byte scalar cutoff +
                                            // 128-byte stride epilogue
                                      1023,
                                      1024,
                                      1025,
                                      4096,
                                      4099};
    for (std::size_t const len : kSizes) {
        std::vector<char> buf(len ? len : 1);
        std::uint8_t expected = 0;
        for (std::size_t i = 0; i < len; ++i) {
            buf[i] = static_cast<char>(rng());
            expected = static_cast<std::uint8_t>(expected + static_cast<std::uint8_t>(buf[i]));
        }
        EXPECT_EQ(nanofix::detail::checksum_bytes(buf.data(), buf.data() + len), expected)
            << "len " << len;
        EXPECT_EQ(nanofix::detail::checksum_bytes_scalar(buf.data(), buf.data() + len), expected)
            << "scalar reference, len " << len;
    }
}

namespace {
char const* g_last_assert_msg = nullptr;
}

// The one assert path (no NDEBUG branch): a failed NANOFIX_ASSERT bumps the
// counter and runs the installed handler. Skip under NANOFIX_ASSERT_FAILFAST,
// where the same path also traps.
#ifndef NANOFIX_ASSERT_FAILFAST
TEST(NanofixAssert, counter_handler_and_passthrough) {
    using namespace nanofix;
    reset_assert_failure_count();
    set_assert_handler(nullptr);
    g_last_assert_msg = nullptr;

    bool volatile false_cond = false;  // read each use, so the branch isn't folded
    NANOFIX_ASSERT(false_cond, "first");
    EXPECT_EQ(assert_failure_count(), 1u);

    set_assert_handler([](char const* m) noexcept { g_last_assert_msg = m; });
    NANOFIX_ASSERT(false_cond, "second");
    EXPECT_EQ(assert_failure_count(), 2u);
    EXPECT_STREQ(g_last_assert_msg, "second");

    bool volatile true_cond = true;
    NANOFIX_ASSERT(true_cond, "noop");  // passing condition: no count, no handler
    EXPECT_EQ(assert_failure_count(), 2u);

    set_assert_handler(nullptr);
    reset_assert_failure_count();
}
#endif

TEST(NanofixTest, parse_error_classification) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "A");
    w.push_back_string(tag::SenderCompID, "S");
    ASSERT_TRUE(w.push_back_trailer());
    char* const end = w.message_end();

    {
        message_reader r(buf, end);
        ASSERT_TRUE(r.is_valid());
        EXPECT_EQ(r.error(), parse_error::none);
    }

    // Corrupt the MsgType framing tag "35" -> "36"; the frame stays complete
    // (length and trailer SOHs unchanged) but fails the tag-35 check.
    std::string_view full(buf, static_cast<std::size_t>(end - buf));
    auto pos = full.find(
        "\x01"
        "35=");
    ASSERT_NE(pos, std::string_view::npos);
    buf[pos + 2] = '6';
    {
        message_reader r(buf, end);
        EXPECT_TRUE(r.is_complete());
        EXPECT_FALSE(r.is_valid());
        EXPECT_EQ(r.error(), parse_error::msg_type_tag_missing);
    }
}

TEST(NanofixTest, parse_error_framing_variants) {
    auto error_of = [](std::string_view b) {
        message_reader r(b.data(), b.data() + b.size());
        return r.error();
    };

    // No SOH within the BeginString span.
    EXPECT_EQ(error_of("8=FIX.4.4XXXXXXX"), parse_error::begin_string_unterminated);
    // BeginString not followed by BodyLength (tag 9).
    EXPECT_EQ(error_of("8=FIX.4.2\x01"
                       "35=D\x01"
                       "10=000\x01"),
              parse_error::body_length_tag_missing);
    // BodyLength past the 9-digit cap.
    EXPECT_EQ(error_of("8=FIX.4.2\x01"
                       "9=9999999999\x01"
                       "35=D\x01"
                       "10=000\x01"),
              parse_error::body_length_overflow);
    // BodyLength understated by one: byte before CheckSum is not SOH.
    EXPECT_EQ(error_of("8=FIX.4.2\x01"
                       "9=4\x01"
                       "35=D\x01"
                       "10=000\x01"),
              parse_error::checksum_soh_missing);
    // CheckSum field has an extra digit: no SOH where the trailer must end it.
    EXPECT_EQ(error_of("8=FIX.4.2\x01"
                       "9=5\x01"
                       "35=D\x01"
                       "10=0000\x01"),
              parse_error::trailer_soh_missing);
}

// A declared data length that doesn't land on a SOH must stop iteration, not
// resume mid-value as a garbage field. The data field is deliberately not last
// so init()'s BodyLength/checksum framing still passes and the branch is hit.
TEST(NanofixTest, data_length_bad_terminator_stops) {
    char buffer[160] = {};
    message_writer writer(buffer);
    std::string datum("datum");
    writer.push_back_header("FIXT.1.1");
    writer.push_back_string(tag::MsgType, "A");
    writer.push_back_data(tag::RawDataLength, tag::RawData, &*datum.begin(), &*datum.end());
    writer.push_back_string(tag::Account, "ACC1");  // keeps a SOH after RawData
    ASSERT_TRUE(writer.push_back_trailer());

    char const* const msg_end = writer.message_end();

    // Control: intact message is valid and yields RawData.
    {
        message_reader reader(buffer, msg_end);
        ASSERT_TRUE(reader.is_valid());
        bool saw_raw = false;
        for (auto it = reader.begin(); it != reader.end(); ++it)
            saw_raw |= (it->tag() == tag::RawData);
        EXPECT_TRUE(saw_raw);
    }

    // Corrupt the interior SOH right after "datum".
    auto* soh = std::search(buffer, const_cast<char*>(msg_end), datum.begin(), datum.end());
    ASSERT_NE(soh, msg_end);
    char* term = soh + datum.size();
    ASSERT_LT(term, const_cast<char*>(msg_end));
    ASSERT_EQ(*term, '\x01');
    *term = 'X';

    message_reader reader(buffer, msg_end);
    ASSERT_TRUE(reader.is_valid());
    bool saw_raw = false;
    std::size_t fields = 0;
    for (auto it = reader.begin(); it != reader.end(); ++it) {
        ++fields;
        saw_raw |= (it->tag() == tag::RawData);
        ASSERT_LT(fields, 64u);  // no runaway loop
    }
    EXPECT_FALSE(saw_raw) << "bad terminator must not yield RawData";
}

// data_len == 0: the bounds check is `data_len >= (end - p) || p[data_len] !=
// SOH`. With len 0 it must accept the empty value (p already points at the SOH)
// and resume at the next field, not stall or skip one.
TEST(NanofixTest, data_length_zero_yields_empty_value) {
    char buffer[160] = {};
    message_writer writer(buffer);
    writer.push_back_header("FIXT.1.1");
    writer.push_back_string(tag::MsgType, "A");
    char const* const empty = buffer;  // any pointer; begin == end => length 0
    writer.push_back_data(tag::RawDataLength, tag::RawData, empty, empty);
    writer.push_back_string(tag::Account, "ACC1");  // must still be reachable
    ASSERT_TRUE(writer.push_back_trailer());

    message_reader reader(buffer, writer.message_end());
    ASSERT_TRUE(reader.is_valid());

    bool saw_empty_raw = false, saw_account = false;
    for (auto it = reader.begin(); it != reader.end(); ++it) {
        if (it->tag() == tag::RawData) {
            saw_empty_raw = true;
            EXPECT_TRUE(it->value().as_string_view().empty());
        }
        if (it->tag() == tag::Account)
            saw_account = true;
    }
    EXPECT_TRUE(saw_empty_raw) << "zero-length RawData must be yielded";
    EXPECT_TRUE(saw_account) << "field after zero-length data must remain reachable";

    // Indexed path frames the same jump; it must agree.
    nanofix::field_index_buffer<64> idx_buf;
    auto idx = nanofix::build_field_index(reader, idx_buf);
    ASSERT_FALSE(idx.truncated());
    ASSERT_TRUE(idx.has(tag::RawData));
    std::size_t h = 0;
    EXPECT_TRUE(idx.find_with_hint(tag::RawData, h).as_string_view().empty());
}

TEST(NanofixTest, iterating) {
    char b[1024];
    char* ptr = b;
    unsigned num = 0;
    for (size_t i = 0; i < 10; i++) {
        message_writer w(ptr, 1024 - (ptr - b));
        w.push_back_header("FIXT.1.1");
        w.push_back_string(tag::MsgType, "A");
        ASSERT_TRUE(w.push_back_trailer());
        ptr += w.message_size();
    }

    for (message_reader reader(b); reader.is_complete(); reader = reader.next_message_reader()) {
        if (reader.is_complete() && reader.is_valid()) {
            EXPECT_EQ(std::distance(reader.begin(), reader.end()), 1);
            num++;
        }
    }
    EXPECT_EQ(num, 10u);
}

TEST(NanofixTest, iterator_category_is_forward) {
    using It = nanofix::message_reader::const_iterator;
    using Tr = std::iterator_traits<It>;
    static_assert(std::is_same_v<Tr::iterator_category, std::forward_iterator_tag>,
                  "field iterator is multipass over an immutable buffer; must be forward");
    static_assert(std::is_same_v<Tr::reference, nanofix::field const&>, "reference must be const");
    static_assert(std::is_same_v<Tr::pointer, nanofix::field const*>, "pointer must be const");
    static_assert(std::is_same_v<Tr::value_type, nanofix::field>);
}

TEST(NanofixTest, msg_type_string) {
    EXPECT_EQ(std::string(msg_type::AccountSummaryReport), std::string("CQ"));
}

TEST(Names, FieldMsgValueLookups) {
    static_assert(std::is_same_v<decltype(nanofix::field_name(0)), std::string_view>);
    EXPECT_EQ(nanofix::field_name(tag::Side), "Side");
    EXPECT_EQ(nanofix::field_name(tag::BeginString), "BeginString");
    EXPECT_TRUE(nanofix::field_name(999999).empty());

    EXPECT_EQ(nanofix::msg_type_name("D"), "NewOrderSingle");
    EXPECT_EQ(nanofix::msg_type_name("8"), "ExecutionReport");
    EXPECT_TRUE(nanofix::msg_type_name("ZZZZ").empty());

    EXPECT_EQ(nanofix::value_name(tag::Side, "1"), "BUY");
    EXPECT_EQ(nanofix::value_name(tag::Side, "8"), "CROSS");
    EXPECT_TRUE(nanofix::value_name(tag::Side, "ZZ").empty());
    EXPECT_TRUE(nanofix::value_name(tag::ClOrdID, "1").empty());  // ClOrdID has no enum
}

// dictionary_init_* loop over the constexpr tables; verify they fill a caller
// map equivalently to the named lookups, and skip unknown tags.
TEST(Names, DictionaryInitBuilders) {
    std::map<int, std::string> fd;
    nanofix::dictionary_init_field(fd);
    EXPECT_EQ(fd[tag::Side], "Side");
    EXPECT_EQ(fd[tag::Price], "Price");
    EXPECT_EQ(fd[tag::BeginString], "BeginString");
    EXPECT_EQ(fd.count(20), 0u);      // tag 20 absent from FIX5.0 spec -> not inserted
    EXPECT_EQ(fd.count(999999), 0u);  // unknown -> not inserted

    std::map<std::string, std::string> md;
    nanofix::dictionary_init_message(md);
    EXPECT_EQ(md["D"], "NewOrderSingle");
    EXPECT_EQ(md["8"], "ExecutionReport");
    EXPECT_EQ(md.count("ZZZZ"), 0u);
}

TEST(NanofixTest, field_value_empty_and_or_else) {
    char buf[256];
    message_writer w(buf, sizeof(buf));
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_int(tag::OrderQty, 100);
    ASSERT_TRUE(w.push_back_trailer());
    message_reader r(w);
    ASSERT_TRUE(r.is_valid());

    field_index_buffer<64> idxbuf;
    with_fields(r, idxbuf, [&](auto& f) {
        long qty_m = 0, qty_e = 0;  // OrderQty is a Qty (decimal) field
        ASSERT_TRUE(f.find(tag::OrderQty).try_as_decimal(qty_m, qty_e));
        EXPECT_EQ(qty_m, 100);
        EXPECT_EQ(qty_e, 0);
        EXPECT_TRUE(f.find(tag::Price).empty());  // genuinely absent

        // or_else is a field_value combinator (reached via value()): present
        // value short-circuits (fallback not run).
        bool ran = false;
        field_value got = f.find(tag::OrderQty).value().or_else([&] {
            ran = true;
            return field_value{};
        });
        EXPECT_FALSE(ran);
        EXPECT_EQ(got.as_string_view(), "100");

        // or_else: empty value runs the fallback.
        field_value alt =
            f.find(tag::Price).value().or_else([&] { return f.find(tag::OrderQty).value(); });
        EXPECT_EQ(alt.as_string_view(), "100");
    });
}

TEST(NanofixTest, with_fields_truncation_dispatches_to_iterator) {
    char buf[512];
    message_writer w(buf, sizeof(buf));
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_int(tag::MsgSeqNum, 1);
    w.push_back_string(tag::SenderCompID, "S");
    w.push_back_string(tag::TargetCompID, "T");
    w.push_back_int(tag::OrderQty, 7);
    ASSERT_TRUE(w.push_back_trailer());
    message_reader r(w);
    ASSERT_TRUE(r.is_valid());

    // N=1 cannot hold the message: with_fields must dispatch to the iterator
    // accessor, and lookups still succeed.
    field_index_buffer<1> tiny;
    with_fields(r, tiny, [&](auto& f) {
        long qty_m = 0, qty_e = 0;  // OrderQty is a Qty (decimal) field
        ASSERT_TRUE(f.find(tag::OrderQty).try_as_decimal(qty_m, qty_e));  // via iterator path
        EXPECT_EQ(qty_m, 7);
        EXPECT_EQ(qty_e, 0);
        EXPECT_EQ(f.find(tag::SenderCompID).as_string_view(), "S");
        EXPECT_TRUE(f.find(tag::Price).empty());  // genuinely absent
    });
}

TEST(NanofixTest, indexed_message_lookup) {
    char buffer[256];
    message_writer w(buffer);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_string(tag::SenderCompID, "ALICE");
    w.push_back_string(tag::TargetCompID, "BOB");
    w.push_back_int(tag::MsgSeqNum, 42);
    w.push_back_int(tag::OrderQty, 100);
    w.push_back_string(tag::Symbol, "AAPL");
    w.push_back_decimal(tag::Price, 50001, -2);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(w);
    ASSERT_TRUE(r.is_valid());

    nanofix::field_index_buffer<32> idx_buffer;
    auto idx = nanofix::build_field_index(r, idx_buffer);

    EXPECT_FALSE(idx.truncated());
    EXPECT_GE(idx.field_count(), 7u);

    std::size_t h1 = 0;
    EXPECT_EQ(idx.find_with_hint(tag::Symbol, h1).as_string_view(), "AAPL");
    std::size_t h2 = 0;
    EXPECT_EQ(idx.find_with_hint(tag::SenderCompID, h2).as_string_view(), "ALICE");
    std::size_t h3 = 0;
    long oq_m = 0, oq_e = 0;  // OrderQty is a Qty (decimal) field
    ASSERT_TRUE(idx.find_with_hint(tag::OrderQty, h3).try_as_decimal(oq_m, oq_e));
    EXPECT_EQ(oq_m, 100);
    EXPECT_EQ(oq_e, 0);
    std::size_t h4 = 0;
    int seq = 0;  // MsgSeqNum is a SeqNum (integer) field
    ASSERT_TRUE(idx.find_with_hint(tag::MsgSeqNum, h4).try_as_int(seq));
    EXPECT_EQ(seq, 42);
    EXPECT_FALSE(idx.has(tag::ClOrdID));

    char buffer2[256];
    message_writer w2(buffer2);
    w2.push_back_header("FIXT.1.1");
    w2.push_back_string(tag::MsgType, "0");
    w2.push_back_string(tag::SenderCompID, "X");
    ASSERT_TRUE(w2.push_back_trailer());
    message_reader r2(w2);
    ASSERT_TRUE(r2.is_valid());

    auto idx2 = nanofix::build_field_index(r2, idx_buffer);
    std::size_t h5 = 0;
    EXPECT_EQ(idx2.find_with_hint(tag::SenderCompID, h5).as_string_view(), "X");
    std::size_t h6 = 0;
    EXPECT_EQ(idx2.find_with_hint(tag::MsgType, h6).as_string_view(), "0");
    EXPECT_FALSE(idx2.has(tag::OrderQty));
}

// Indirectly exercises the SoA-pack path for the data-length tag handling
// (length tag absorbed, data tag surfaced).
TEST(NanofixTest, build_field_index_data_length) {
    using namespace nanofix;
    char buffer[256];
    message_writer w(buffer);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "n");
    w.push_back_int(tag::MsgSeqNum, 1);
    char const raw[] =
        "abc\x01"
        "def\x01ghi";
    w.push_back_data(tag::RawDataLength, tag::RawData, raw, raw + sizeof(raw) - 1);
    w.push_back_string(tag::Symbol, "AAPL");
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(w);
    ASSERT_TRUE(r.is_valid());

    field_index_buffer<32> ib;
    auto idx = build_field_index(r, ib);

    std::size_t h = 0;
    field_value raw_v = idx.find_with_hint(tag::RawData, h).value();  // raw bytes
    ASSERT_EQ(static_cast<std::size_t>(raw_v.end() - raw_v.begin()), sizeof(raw) - 1);
    EXPECT_EQ(std::memcmp(raw_v.begin(), raw, sizeof(raw) - 1), 0);
    EXPECT_FALSE(idx.has(tag::RawDataLength));
    h = 0;
    EXPECT_EQ(idx.find_with_hint(tag::Symbol, h).as_string_view(), "AAPL");
}

TEST(NanofixTest, index_matches_iterator_on_lying_data_length) {
    // SignatureLength (93) declares 2 but the Signature (89) value is 4 bytes:
    // the declared offset lands mid-value and is not SOH-terminated. The
    // iterator stops; the index must not resume mid-value and fabricate fields.
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=23\x01"
        "35=A\x01"
        "93=2\x01"
        "89=abcd\x01"
        "58=x\x01"
        "10=000\x01";
    message_reader r(buf, buf + sizeof(buf) - 1);
    ASSERT_TRUE(r.is_complete() && r.is_valid());

    std::vector<int> iter_tags;
    for (auto it = r.begin(); it != r.end(); ++it)
        iter_tags.push_back(it->tag());

    field_index_buffer<32> ib;
    auto idx = build_field_index(r, ib);
    ASSERT_EQ(idx.field_count(), iter_tags.size());
    for (std::size_t i = 0; i < iter_tags.size(); ++i)
        EXPECT_EQ(idx.tag_at(i), iter_tags[i]);
}

TEST(NanofixTest, data_field_must_terminate_before_trailer) {
    // 93 declares 9: the SOH probe byte p[9] is the message's final SOH,
    // inside the trailer. A data value crossing the trailer must stop
    // iteration, not yield a field spanning "10=...".
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=16\x01"
        "35=A\x01"
        "93=9\x01"
        "89=ab\x01"
        "10=000\x01";
    message_reader r(buf, buf + sizeof(buf) - 1);
    ASSERT_TRUE(r.is_complete() && r.is_valid());
    char const* const trailer = r.end().buffer_begin();
    std::size_t n = 0;
    for (auto it = r.begin(); it != r.end(); ++it, ++n)
        EXPECT_LE(it->value().end(), trailer) << "field value crosses into the trailer";
    EXPECT_EQ(n, 1u);  // MsgType only; the lying data field stops iteration
}

TEST(NanofixTest, push_back_decimal_positive_exponent_scales) {
    auto emit = [](long mantissa, long exponent) {
        char buf[64];
        message_writer w(buf, sizeof(buf));
        w.push_back_decimal(44, mantissa, exponent);
        EXPECT_TRUE(w.ok());
        return std::string(w.message_begin(), w.message_end());
    };
    EXPECT_EQ(emit(5, 2), "44=500\x01");
    EXPECT_EQ(emit(-5, 2), "44=-500\x01");
    EXPECT_EQ(emit(0, 3), "44=0\x01");
    EXPECT_EQ(emit(12345, 1), "44=123450\x01");
    EXPECT_EQ(emit(5, 0), "44=5\x01");
    EXPECT_EQ(emit(50001, -2), "44=500.01\x01");
}

TEST(NanofixTest, push_back_decimal_positive_exponent_overflow_errors) {
    char buf[32];
    message_writer w(buf, sizeof(buf));
    w.push_back_decimal(44, 5L, 1000000L);  // 1e6 zeros cannot fit
    EXPECT_FALSE(w.ok());
    EXPECT_EQ(w.message_size(), 0u);
}

TEST(NanofixTest, generic_try_as_time_types) {
    using namespace std::chrono;
    auto v = [](char const* s) { return field_value(s, s + std::strlen(s)); };

    sys_time<milliseconds> tp;
    ASSERT_TRUE(v("20260712-12:30:05.250").try_as(tp));
    EXPECT_EQ(
        tp, sys_days{year{2026} / 7 / 12} + hours{12} + minutes{30} + seconds{5} + milliseconds{250});

    sys_time<nanoseconds> tpn;
    ASSERT_TRUE(v("20260712-12:30:05.123456789").try_as(tpn));
    EXPECT_EQ(tpn.time_since_epoch() % seconds{1}, nanoseconds{123456789});

    // Policy: sub-ms wire into an ms-precision target rejects, never truncates.
    EXPECT_FALSE(v("20260712-12:30:05.123456789").try_as(tp));
    // try_as is fully validating: digits, separators, calendar ranges.
    EXPECT_FALSE(v("2026071X-12:30:05").try_as(tp));
    EXPECT_FALSE(v("20260712T12:30:05").try_as(tp));
    EXPECT_FALSE(v("20260712-25:30:05").try_as(tp));
    EXPECT_FALSE(v("20260712-12:61:05").try_as(tp));
    EXPECT_FALSE(v("20261312-12:30:05").try_as(tp));

    nanoseconds dur{};
    ASSERT_TRUE(v("12:30:05.250").try_as(dur));
    EXPECT_EQ(dur, hours{12} + minutes{30} + seconds{5} + milliseconds{250});
    EXPECT_FALSE(v("12-30-05").try_as(dur));
    EXPECT_FALSE(v("12:3X:05").try_as(dur));
    milliseconds dur_ms{};
    ASSERT_TRUE(v("23:59:60").try_as(dur_ms));  // leap second passes
    EXPECT_FALSE(v("24:00:00").try_as(dur_ms));

    year_month_day ymd{};
    ASSERT_TRUE(v("20260712").try_as(ymd));
    EXPECT_EQ(ymd, year{2026} / 7 / 12);
    EXPECT_FALSE(v("20261312").try_as(ymd));  // month 13
    EXPECT_FALSE(v("2026071").try_as(ymd));   // short
    EXPECT_FALSE(v("2026071X").try_as(ymd));  // non-digit

    year_month ym{};
    ASSERT_TRUE(v("202607").try_as(ym));
    EXPECT_EQ(ym, year{2026} / 7);
    EXPECT_FALSE(v("202613").try_as(ym));
}

TEST(NanofixTest, named_chrono_try_as_methods) {
    using namespace std::chrono;
    auto v = [](char const* s) { return field_value(s, s + std::strlen(s)); };

    sys_time<milliseconds> tp;
    ASSERT_TRUE(v("20260712-12:30:05.250").try_as_timestamp(tp));
    EXPECT_EQ(
        tp, sys_days{year{2026} / 7 / 12} + hours{12} + minutes{30} + seconds{5} + milliseconds{250});
    EXPECT_FALSE(v("2026071X-12:30:05").try_as_timestamp(tp));

    nanoseconds dur{};
    ASSERT_TRUE(v("12:30:05.250").try_as_timeonly(dur));
    EXPECT_EQ(dur, hours{12} + minutes{30} + seconds{5} + milliseconds{250});
    EXPECT_FALSE(v("12:3X:05").try_as_timeonly(dur));

    year_month_day ymd{};
    ASSERT_TRUE(v("20260712").try_as_date(ymd));
    EXPECT_EQ(ymd, year{2026} / 7 / 12);
    EXPECT_FALSE(v("20261312").try_as_date(ymd));

    year_month ym{};
    ASSERT_TRUE(v("202607").try_as_monthyear(ym));
    EXPECT_EQ(ym, year{2026} / 7);
    EXPECT_FALSE(v("202613").try_as_monthyear(ym));
}

TEST(NanofixTest, generic_as_unchecked_time_types) {
    using namespace std::chrono;
    auto v = [](char const* s) { return field_value(s, s + std::strlen(s)); };

    auto tp = v("20260712-12:30:05.250").as_unchecked<sys_time<milliseconds>>();
    EXPECT_EQ(
        tp, sys_days{year{2026} / 7 / 12} + hours{12} + minutes{30} + seconds{5} + milliseconds{250});

    auto dur = v("12:30:05").as_unchecked<nanoseconds>();
    EXPECT_EQ(dur, hours{12} + minutes{30} + seconds{5});

    auto ymd = v("20260712").as_unchecked<year_month_day>();
    EXPECT_EQ(ymd, year{2026} / 7 / 12);

    auto ym = v("202607").as_unchecked<year_month>();
    EXPECT_EQ(ym, year{2026} / 7);
}

TEST(NanofixTest, group_for_each_returns_visited_entry_count) {
    char buf[256];
    message_writer w(buf, sizeof(buf));
    w.push_back_header("FIX.4.2");
    w.push_back_string(tag::MsgType, "W");
    w.push_back_int(tag::NoMDEntries, 3);  // declared 3, only 2 on the wire
    w.push_back_char(tag::MDEntryType, '0');
    w.push_back_decimal(tag::MDEntryPx, 10000L, -2L);
    w.push_back_char(tag::MDEntryType, '1');
    w.push_back_decimal(tag::MDEntryPx, 10050L, -2L);
    ASSERT_TRUE(w.push_back_trailer());
    message_reader r(w);
    ASSERT_TRUE(r.is_complete() && r.is_valid());

    auto g = r.group(tag::NoMDEntries, tag::MDEntryType);
    EXPECT_EQ(g.size(), 3u);  // declared count
    std::size_t const visited = g.for_each([](group_entry const&) {});
    EXPECT_EQ(visited, 2u);  // actual — count mismatch is now detectable
}

TEST(NanofixTest, push_back_bool) {
    char buf[64];
    message_writer w(buf, sizeof(buf));
    w.push_back_bool(tag::PossDupFlag, true);
    w.push_back_bool(tag::PossDupFlag, false);
    ASSERT_TRUE(w.ok());
    EXPECT_EQ(std::string(w.message_begin(), w.message_end()),
              "43=Y\x01"
              "43=N\x01");
}

TEST(NanofixTest, push_back_data_string_view) {
    char buf[64];
    message_writer w(buf, sizeof(buf));
    w.push_back_data(tag::SignatureLength,
                     tag::Signature,
                     std::string_view("ab\x01"
                                      "cd",
                                      5));
    ASSERT_TRUE(w.ok());
    EXPECT_EQ(std::string(w.message_begin(), w.message_end()),
              "93=5\x01"
              "89=ab\x01"
              "cd\x01");
}

TEST(NanofixTest, writer_rejects_out_of_range_date_time_parts) {
    auto emits_error = [](auto&& fn) {
        char buf[64];
        message_writer w(buf, sizeof(buf));
        fn(w);
        return !w.ok() && w.message_size() == 0;
    };
    EXPECT_TRUE(emits_error([](message_writer& w) { w.push_back_date(75, 10000, 1, 2); }));
    EXPECT_TRUE(emits_error([](message_writer& w) { w.push_back_date(75, -1, 1, 2); }));
    EXPECT_TRUE(emits_error([](message_writer& w) { w.push_back_date(75, 2026, 13, 2); }));
    EXPECT_TRUE(emits_error([](message_writer& w) { w.push_back_date(75, 2026, 0, 2); }));
    EXPECT_TRUE(emits_error([](message_writer& w) { w.push_back_date(75, 2026, 1, 32); }));
    EXPECT_TRUE(emits_error([](message_writer& w) { w.push_back_date(75, 2026, 2, 30); }));
    EXPECT_TRUE(emits_error([](message_writer& w) { w.push_back_date(75, 2023, 2, 29); }));
    // INT_MIN: int `m - 1` before the unsigned cast was signed-overflow UB.
    EXPECT_TRUE(emits_error([](message_writer& w) { w.push_back_date(75, 2026, INT_MIN, INT_MIN); }));
    EXPECT_TRUE(emits_error([](message_writer& w) { w.push_back_monthyear(200, 2026, INT_MIN); }));
    EXPECT_TRUE(
        emits_error([](message_writer& w) { w.push_back_timestamp(52, 2026, 4, 31, 3, 4, 5); }));
    EXPECT_TRUE(emits_error([](message_writer& w) { w.push_back_monthyear(200, 2026, 13); }));
    EXPECT_TRUE(emits_error([](message_writer& w) { w.push_back_timeonly(273, 24, 0, 0); }));
    EXPECT_TRUE(emits_error([](message_writer& w) { w.push_back_timeonly(273, 12, 60, 0); }));
    EXPECT_TRUE(emits_error([](message_writer& w) { w.push_back_timeonly(273, 12, 0, 61); }));
    EXPECT_TRUE(emits_error([](message_writer& w) { w.push_back_timeonly(273, 12, 0, 0, 1000); }));
    EXPECT_TRUE(
        emits_error([](message_writer& w) { w.push_back_timestamp(52, 10000, 1, 2, 3, 4, 5); }));
    EXPECT_TRUE(emits_error([](message_writer& w) {
        w.push_back_timestamp_nano(60, 2026, 1, 2, 3, 4, 5, 1000000000);
    }));
}

TEST(NanofixTest, writer_accepts_leap_second_and_valid_parts) {
    char buf[64];
    message_writer w(buf, sizeof(buf));
    w.push_back_timeonly(273, 23, 59, 60);  // leap second is legal FIX
    ASSERT_TRUE(w.ok());
    EXPECT_EQ(std::string(w.message_begin(), w.message_end()), "273=23:59:60\x01");

    message_writer w2(buf, sizeof(buf));
    w2.push_back_date(75, 2024, 2, 29);  // leap-year Feb 29 is a real date
    ASSERT_TRUE(w2.ok());
    EXPECT_EQ(std::string(w2.message_begin(), w2.message_end()), "75=20240229\x01");
}

TEST(NanofixTest, writer_epoch_timestamp_out_of_range_sets_error) {
    char buf[64];
    message_writer w(buf, sizeof(buf));
    w.push_back_timestamp_epoch_millis(52, 253'402'300'800'000LL);  // year 10000
    EXPECT_FALSE(w.ok());
    EXPECT_EQ(w.message_size(), 0u);
}

TEST(Regression, writer_chrono_timestamp_huge_epoch_no_overflow) {
    using namespace std::chrono;
    // A coarse-duration time_point converts to ms/ns by multiplying; a huge
    // count must set the error, not signed-overflow (UB) inside the cast.
    char buf[64];
    message_writer w(buf, sizeof(buf));
    w.push_back_timestamp(52, sys_time<seconds>{seconds{INT64_MAX / 1000 + 1}});
    EXPECT_FALSE(w.ok());

    message_writer w2(buf, sizeof(buf));
    w2.push_back_timestamp_nano(52, sys_time<milliseconds>{milliseconds{INT64_MAX / 1'000'000 + 1}});
    EXPECT_FALSE(w2.ok());

    message_writer w3(buf, sizeof(buf));
    w3.push_back_timestamp(52, sys_time<seconds>{seconds{INT64_MIN / 1000 - 1}});
    EXPECT_FALSE(w3.ok());
}

TEST(Regression, try_atod_exponent_underflow_rejected) {
    // Zero mantissa never trips the overflow check; the exponent decrement
    // must reject a long fraction, not underflow (signed-overflow UB).
    std::string const s = "0." + std::string(200, '0');
    field_value const v(s.data(), s.data() + s.size());
    std::int8_t m8 = 0, e8 = 0;
    EXPECT_FALSE(v.try_as_decimal(m8, e8));

    std::int64_t m = 0, e = 0;  // ample exponent range: same wire parses
    ASSERT_TRUE(v.try_as_decimal(m, e));
    EXPECT_EQ(m, 0);
    EXPECT_EQ(e, -200);
}

TEST(NanofixTest, try_as_string_view_false_on_missed_lookup) {
    char buf[256];
    message_writer w(buf, sizeof(buf));
    w.push_back_header("FIX.4.2");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_string(tag::Text, "");  // present but empty value
    ASSERT_TRUE(w.push_back_trailer());
    message_reader r(w);
    ASSERT_TRUE(r.is_complete() && r.is_valid());

    iter_fields f(r);
    std::string_view sv = "sentinel";
    EXPECT_FALSE(f.find(tag::Account).value().try_as(sv));  // miss
    EXPECT_EQ(sv, "sentinel");                              // out-param untouched

    EXPECT_TRUE(f.find(tag::Text).value().try_as(sv));  // hit, empty value
    EXPECT_TRUE(sv.empty());
}

TEST(NanofixTest, push_back_string_reversed_range_sets_error) {
    char buf[64];
    message_writer w(buf, sizeof(buf));
    char const s[] = "ABC";
    w.push_back_string(55, s + 3, s);  // end < begin
    EXPECT_FALSE(w.ok());
    EXPECT_EQ(w.message_size(), 0u);
}

TEST(NanofixTest, push_back_header_reversed_range_sets_error) {
    char buf[64];
    message_writer w(buf, sizeof(buf));
    char const s[] = "FIX.4.2";
    w.push_back_header(s + 7, s);  // end < begin
    EXPECT_FALSE(w.ok());
    EXPECT_EQ(w.message_size(), 0u);
}

TEST(NanofixTest, chrono) {
    using namespace std::chrono;
    using TimePoint = time_point<system_clock, milliseconds>;

    TimePoint tsend(milliseconds(1502282096123ULL));

    milliseconds timeofday = hours(12) + minutes(34) + seconds(0) + milliseconds(789);

    char buffer[100] = {};
    message_writer writer(buffer);
    writer.push_back_header("FIXT.1.1");
    writer.push_back_string(nanofix::tag::MsgType, "A");
    writer.push_back_timestamp(nanofix::tag::SendingTime, tsend);
    writer.push_back_timeonly(nanofix::tag::MDEntryTime, timeofday);
    ASSERT_TRUE(writer.push_back_trailer());

    message_reader reader(writer);
    message_reader::const_iterator i = reader.begin();
    ASSERT_TRUE(reader.find_with_hint(nanofix::tag::SendingTime, i));
    EXPECT_EQ(i->value().as_string_view(), std::string("20170809-12:34:56.123"));

    TimePoint trecv;
    EXPECT_TRUE(i->value().as_timestamp(trecv));
    EXPECT_EQ(tsend, trecv);

    message_reader::const_iterator j = reader.begin();
    ASSERT_TRUE(reader.find_with_hint(nanofix::tag::MDEntryTime, j));
    EXPECT_EQ(j->value().as_string_view(), std::string("12:34:00.789"));

    milliseconds todrecv;
    EXPECT_TRUE(j->value().as_timeonly(todrecv));
    EXPECT_EQ(timeofday, todrecv);
}

TEST(NanofixTest, chrono_nano) {
    using namespace std::chrono;
    using TimePoint = time_point<system_clock, nanoseconds>;

    TimePoint tsend(nanoseconds(1502282096123456789ULL));

    nanoseconds timeofday = hours(12) + minutes(34) + seconds(0) + nanoseconds(789456123);

    char buffer[100] = {};
    message_writer writer(buffer);
    writer.push_back_header("FIXT.1.1");
    writer.push_back_string(nanofix::tag::MsgType, "A");
    writer.push_back_timestamp_nano(nanofix::tag::SendingTime, tsend);
    writer.push_back_timeonly_nano(nanofix::tag::MDEntryTime, timeofday);
    ASSERT_TRUE(writer.push_back_trailer());

    message_reader reader(writer);
    message_reader::const_iterator i = reader.begin();
    ASSERT_TRUE(reader.find_with_hint(nanofix::tag::SendingTime, i));
    EXPECT_EQ(i->value().as_string_view(), std::string("20170809-12:34:56.123456789"));

    TimePoint trecv;
    EXPECT_TRUE(i->value().as_timestamp_nano(trecv));
    EXPECT_EQ(tsend, trecv);

    message_reader::const_iterator j = reader.begin();
    ASSERT_TRUE(reader.find_with_hint(nanofix::tag::MDEntryTime, j));
    EXPECT_EQ(j->value().as_string_view(), std::string("12:34:00.789456123"));

    nanoseconds todrecv;
    EXPECT_TRUE(j->value().as_timeonly_nano(todrecv));
    EXPECT_EQ(timeofday, todrecv);
}

TEST(NanofixTest, header) {
    char const* begstr_cp = "FIXT.1.1";
    std::string_view begstr_sv = begstr_cp;

    char buffer_cp[100] = {};
    char buffer_sv[100] = {};

    message_writer writer_cp(buffer_cp);
    message_writer writer_sv(buffer_sv);

    writer_cp.push_back_header(begstr_cp);
    writer_sv.push_back_header(begstr_sv);

    ASSERT_EQ(writer_cp.message_size(), writer_sv.message_size());
    ASSERT_EQ(
        std::memcmp(writer_cp.message_begin(), writer_sv.message_begin(), writer_cp.message_size()),
        0);
}

TEST(NanofixTest, read_string) {
    char buffer[100] = {};

    message_writer writer(buffer);

    writer.push_back_header("FIXT.1.1");
    writer.push_back_string(nanofix::tag::MsgType, "A");
    ASSERT_TRUE(writer.push_back_trailer());

    message_reader reader(buffer);

    auto i = reader.begin();
    EXPECT_TRUE(reader.find_with_hint(tag::MsgType, i));
    EXPECT_TRUE(i->value() == "A");
    EXPECT_EQ(i->value().as_string_view(), "A");
}

TEST(NanofixTest, epoch_nanos_roundtrip) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    using namespace std::chrono;
    auto const tp = sys_days{year{2024} / January / 15} + 9h + 30min + nanoseconds{123456789};
    std::int64_t const expected_nanos = tp.time_since_epoch().count();
    w.push_back_timestamp_epoch_nanos(tag::SendingTime, expected_nanos);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    auto it = r.begin();
    ASSERT_TRUE(r.find_with_hint(tag::SendingTime, it));
    auto parsed = it->value().as_epoch_nanos();
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed.value(), expected_nanos);
}

TEST(NanofixTest, epoch_millis_roundtrip) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    using namespace std::chrono;
    auto const tp = sys_days{year{2024} / January / 15} + 9h + 30min + milliseconds{123};
    std::int64_t const expected_millis = tp.time_since_epoch().count();
    w.push_back_timestamp_epoch_millis(tag::SendingTime, expected_millis);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    auto it = r.begin();
    ASSERT_TRUE(r.find_with_hint(tag::SendingTime, it));
    auto parsed = it->value().as_epoch_millis();
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed.value(), expected_millis);
}

TEST(NanofixTest, as_epoch_nanos_returns_nullopt_on_malformed) {
    char buf[64] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_string(tag::SendingTime, "garbage");
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    auto it = r.begin();
    ASSERT_TRUE(r.find_with_hint(tag::SendingTime, it));
    EXPECT_FALSE(it->value().as_epoch_nanos().has_value());
    EXPECT_FALSE(it->value().as_epoch_millis().has_value());
}

TEST(NanofixTest, push_back_trailer_ok_path) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_int(tag::OrderQty, 100);
    EXPECT_TRUE(w.ok());
    EXPECT_TRUE(w.push_back_trailer());
    EXPECT_TRUE(w.ok());

    message_reader r(buf, w.message_end());
    EXPECT_TRUE(r.is_complete());
    EXPECT_TRUE(r.is_valid());
}

TEST(NanofixTest, try_write_message_success) {
    char buf[256] = {};
    char* end = nullptr;
    bool ok = try_write_message(buf, buf + sizeof(buf), end, [](message_writer& w) {
        w.push_back_header("FIXT.1.1");
        w.push_back_string(tag::MsgType, "D");
        w.push_back_int(tag::OrderQty, 100);
    });
    ASSERT_TRUE(ok);

    message_reader r(buf, end);
    EXPECT_TRUE(r.is_complete());
    EXPECT_TRUE(r.is_valid());
}

TEST(NanofixTest, try_write_message_overflow) {
    char buf[20] = {};
    char* end = nullptr;
    bool ok = try_write_message(buf, buf + sizeof(buf), end, [](message_writer& w) {
        w.push_back_header("FIXT.1.1");
        w.push_back_string(tag::MsgType, "D");
        w.push_back_string(tag::SenderCompID, "AAAAAAAA");
    });
    EXPECT_FALSE(ok);
}

TEST(NanofixTest, group_iteration) {
    char buf[1024] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "W");
    w.push_back_int(tag::NoMDEntries, 3);
    for (int i = 0; i < 3; ++i) {
        w.push_back_char(tag::MDEntryType, static_cast<char>('0' + i));
        w.push_back_decimal(tag::MDEntryPx, 10000 + i, -2);
        w.push_back_int(tag::MDEntrySize, 100 * (i + 1));
    }
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    auto md = r.group(tag::NoMDEntries, tag::MDEntryType);
    EXPECT_EQ(md.size(), 3u);

    int seen = 0;
    md.for_each([&](group_entry const& entry) {
        auto it = entry.begin();
        ASSERT_TRUE(entry.find_with_hint(tag::MDEntryType, it));
        EXPECT_EQ(it->value().as_char_unchecked(), char('0' + seen));

        it = entry.begin();
        ASSERT_TRUE(entry.find_with_hint(tag::MDEntrySize, it));
        EXPECT_EQ(it->value().as_int_unchecked<int>(), 100 * (seen + 1));

        ++seen;
    });
    EXPECT_EQ(seen, 3);
}

TEST(NanofixTest, message_writer_noexcept_overflow) {
    char buf[20] = {};
    message_writer w(buf);
    static_assert(noexcept(w.push_back_header("FIXT.1.1")));
    static_assert(noexcept(w.push_back_string(tag::MsgType, "D")));
    static_assert(noexcept(w.push_back_trailer()));

    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_string(tag::SenderCompID, "AAAAAAAA");
    EXPECT_FALSE(w.ok());
    EXPECT_TRUE(w.has_error());
    EXPECT_FALSE(w.push_back_trailer());
}

TEST(NanofixTest, group_runtime_size_and_count) {
    char buf[1024] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "W");
    w.push_back_int(tag::NoMDEntries, 2);
    w.push_back_char(tag::MDEntryType, '0');
    w.push_back_decimal(tag::MDEntryPx, 100, -2);
    w.push_back_char(tag::MDEntryType, '1');
    w.push_back_decimal(tag::MDEntryPx, 200, -2);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    auto md = r.group(tag::NoMDEntries, tag::MDEntryType);
    EXPECT_EQ(md.size(), 2u);
    int seen = 0;
    md.for_each([&](group_entry const& entry) {
        (void)entry;
        ++seen;
    });
    EXPECT_EQ(seen, 2);
}

TEST(NanofixTest, group_iteration_respects_count) {
    char buf[1024] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "W");
    w.push_back_int(tag::NoMDEntries, 2);
    w.push_back_char(tag::MDEntryType, '0');
    w.push_back_decimal(tag::MDEntryPx, 10000, -2);
    w.push_back_int(tag::MDEntrySize, 100);
    w.push_back_char(tag::MDEntryType, '1');
    w.push_back_decimal(tag::MDEntryPx, 20000, -2);
    w.push_back_int(tag::MDEntrySize, 200);
    w.push_back_char(tag::MDEntryType, '2');
    w.push_back_decimal(tag::MDEntryPx, 30000, -2);
    w.push_back_int(tag::MDEntrySize, 300);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    auto md = r.group(tag::NoMDEntries, tag::MDEntryType);
    EXPECT_EQ(md.size(), 2u);

    std::vector<char> seen_types;
    md.for_each([&](group_entry const& entry) {
        auto it = entry.begin();
        ASSERT_TRUE(entry.find_with_hint(tag::MDEntryType, it));
        seen_types.push_back(it->value().as_char_unchecked());
    });
    ASSERT_EQ(seen_types.size(), 2u);
    EXPECT_EQ(seen_types[0], '0');
    EXPECT_EQ(seen_types[1], '1');
}

TEST(NanofixTest, group_zero_entries) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "W");
    w.push_back_int(tag::NoMDEntries, 0);
    w.push_back_string(tag::Symbol, "AAPL");
    w.push_back_int(tag::OrderQty, 42);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    auto md = r.group(tag::NoMDEntries, tag::MDEntryType);
    EXPECT_EQ(md.size(), 0u);
    EXPECT_TRUE(md.empty());
    int seen = 0;
    md.for_each([&](group_entry const& e) {
        (void)e;
        ++seen;
    });
    EXPECT_EQ(seen, 0);

    auto it = r.begin();
    ASSERT_TRUE(r.find_with_hint(tag::Symbol, it));
    EXPECT_EQ(it->value().as_string_view(), "AAPL");
    it = r.begin();
    ASSERT_TRUE(r.find_with_hint(tag::OrderQty, it));
    EXPECT_EQ(it->value().as_int_unchecked<int>(), 42);
}

TEST(NanofixTest, group_entry_field_order) {
    char buf[1024] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "W");
    w.push_back_int(tag::NoMDEntries, 3);
    for (int i = 0; i < 3; ++i) {
        w.push_back_char(tag::MDEntryType, static_cast<char>('0' + i));
        w.push_back_decimal(tag::MDEntryPx, 10000 + i, -2);
        w.push_back_int(tag::MDEntrySize, 100 * (i + 1));
    }
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    auto md = r.group(tag::NoMDEntries, tag::MDEntryType);
    ASSERT_EQ(md.size(), 3u);

    // Each entry spans exactly its three fields, delimiter first, in wire order.
    std::vector<int> tags;
    int entry = 0;
    md.for_each([&](group_entry const& e) {
        for (auto it = e.begin(); it != e.end(); ++it)
            tags.push_back(it->tag());
        auto type_it = e.begin();
        ASSERT_TRUE(e.find_with_hint(tag::MDEntryType, type_it));
        EXPECT_EQ(type_it->value().as_char_unchecked(), static_cast<char>('0' + entry));
        ++entry;
    });
    std::vector<int> const expected = {
        tag::MDEntryType,
        tag::MDEntryPx,
        tag::MDEntrySize,
        tag::MDEntryType,
        tag::MDEntryPx,
        tag::MDEntrySize,
        tag::MDEntryType,
        tag::MDEntryPx,
        tag::MDEntrySize,
    };
    EXPECT_EQ(tags, expected);
}

TEST(NanofixTest, group_nested_runtime) {
    char buf[1024] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "AN");
    w.push_back_int(tag::NoAsgnReqs, 1);
    w.push_back_int(tag::NoPartyIDs, 2);
    w.push_back_string(tag::PartyID, "ALICE");
    w.push_back_string(tag::PartyID, "BOB");
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    auto outer = r.group(tag::NoAsgnReqs, tag::NoPartyIDs);
    ASSERT_EQ(outer.size(), 1u);
    std::vector<std::string> ids;
    outer.for_each([&](group_entry const& outer_entry) {
        auto inner = outer_entry.group(tag::NoPartyIDs, tag::PartyID);
        EXPECT_EQ(inner.size(), 2u);
        inner.for_each([&](group_entry const& e) {
            auto pit = e.begin();
            ASSERT_TRUE(e.find_with_hint(tag::PartyID, pit));
            ids.emplace_back(pit->value().as_string_view());
        });
    });
    EXPECT_EQ(ids, (std::vector<std::string>{"ALICE", "BOB"}));
}

TEST(NanofixTest, group_empty_when_absent) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "0");
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    auto g = r.group(tag::NoMDEntries, tag::MDEntryType);
    EXPECT_EQ(g.size(), 0u);
    EXPECT_TRUE(g.empty());
    int seen = 0;
    g.for_each([&](group_entry const&) { ++seen; });
    EXPECT_EQ(seen, 0);
}

TEST(NanofixTest, try_as_int_validates) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_int(tag::OrderQty, 100);
    w.push_back_string(tag::Symbol, "AAPL");
    w.push_back_string(tag::Account, "12X");
    w.push_back_string(tag::Text, "");
    w.push_back_string(tag::ClOrdID, "-42");
    w.push_back_string(tag::SecurityID, "+5");
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    auto find = [&](int t) {
        auto it = r.begin();
        EXPECT_TRUE(r.find_with_hint(t, it));
        return it;
    };

    {
        int out = -1;
        EXPECT_TRUE(find(tag::OrderQty)->value().try_as_int(out));
        EXPECT_EQ(out, 100);
    }
    {
        int out = -1;
        EXPECT_FALSE(find(tag::Symbol)->value().try_as_int(out));
        EXPECT_EQ(out, -1);
    }
    {
        int out = -1;
        EXPECT_FALSE(find(tag::Account)->value().try_as_int(out));
    }
    {
        int out = -1;
        EXPECT_FALSE(find(tag::Text)->value().try_as_int(out));
    }
    {
        int out = 0;
        EXPECT_TRUE(find(tag::ClOrdID)->value().try_as_int(out));
        EXPECT_EQ(out, -42);
    }
    {
        unsigned out = 0;
        EXPECT_FALSE(find(tag::ClOrdID)->value().try_as_int(out));
    }
    {
        int out = 0;
        EXPECT_FALSE(find(tag::SecurityID)->value().try_as_int(out));
    }
}

TEST(NanofixTest, try_as_decimal_validates) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_decimal(tag::Price, 50001, -2);
    w.push_back_string(tag::Symbol, "1.2.3");
    w.push_back_string(tag::Account, "abc");
    w.push_back_string(tag::Text, ".");
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    auto find = [&](int t) {
        auto it = r.begin();
        EXPECT_TRUE(r.find_with_hint(t, it));
        return it;
    };

    {
        int m = 0, e = 0;
        EXPECT_TRUE(find(tag::Price)->value().try_as_decimal(m, e));
        EXPECT_EQ(m, 50001);
        EXPECT_EQ(e, -2);
    }
    {
        int m = -1, e = -1;
        EXPECT_FALSE(find(tag::Symbol)->value().try_as_decimal(m, e));
    }
    {
        int m = -1, e = -1;
        EXPECT_FALSE(find(tag::Account)->value().try_as_decimal(m, e));
    }
    {
        int m = -1, e = -1;
        EXPECT_FALSE(find(tag::Text)->value().try_as_decimal(m, e));
    }
}

TEST(NanofixTest, length_tag_index_full_table) {
    constexpr nanofix::detail::length_tag_index t{};

    constexpr auto n = sizeof(::length_fields) / sizeof(::length_fields[0]);
    for (std::size_t i = 0; i < n; ++i) {
        int const tg = ::length_fields[i];
        EXPECT_TRUE(t.contains(tg)) << "missing length tag " << tg;
        EXPECT_TRUE(nanofix::detail::is_tag_a_data_length(tg))
            << "is_tag_a_data_length(" << tg << ") false";
    }

    // Tag above the 1024 bitmap split.
    static_assert(t.contains(nanofix::tag::DerivativeEncodedIssuerLen));

    EXPECT_FALSE(t.contains(nanofix::tag::BodyLength));
    EXPECT_FALSE(t.contains(nanofix::tag::MsgType));
    EXPECT_FALSE(t.contains(nanofix::tag::CheckSum));
    EXPECT_FALSE(t.contains(nanofix::tag::OrderQty));
    EXPECT_FALSE(t.contains(nanofix::tag::UnderlyingLegSymbol));
    EXPECT_FALSE(nanofix::detail::is_tag_a_data_length(nanofix::tag::UnderlyingLegSymbol));
}

TEST(NanofixTest, indexed_group_entry_lookup) {
    char buf[1024] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "W");
    w.push_back_int(tag::NoMDEntries, 2);
    w.push_back_char(tag::MDEntryType, '0');
    w.push_back_decimal(tag::MDEntryPx, 10000, -2);
    w.push_back_int(tag::MDEntrySize, 500);
    w.push_back_char(tag::MDEntryType, '1');
    w.push_back_decimal(tag::MDEntryPx, 10050, -2);
    w.push_back_int(tag::MDEntrySize, 300);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    nanofix::field_index_buffer<32> entry_idx;
    int seen = 0;
    r.group(tag::NoMDEntries, tag::MDEntryType).for_each([&](group_entry const& entry) {
        auto idx = nanofix::build_field_index(entry, entry_idx);
        EXPECT_FALSE(idx.truncated());

        std::size_t hh1 = 0;
        auto type_field = idx.find_with_hint(tag::MDEntryType, hh1);  // char-enum
        ASSERT_FALSE(type_field.empty());
        char entry_type = 0;
        ASSERT_TRUE(type_field.try_as_char(entry_type));
        EXPECT_EQ(entry_type, char('0' + seen));

        long m = 0, e = 0;
        std::size_t hh2 = 0;
        ASSERT_TRUE(idx.find_with_hint(tag::MDEntryPx, hh2).try_as_decimal(m, e));
        EXPECT_EQ(m, seen == 0 ? 10000 : 10050);
        EXPECT_EQ(e, -2);

        long sz_m = 0, sz_e = 0;  // MDEntrySize is a Qty (decimal) field
        std::size_t hh3 = 0;
        ASSERT_TRUE(idx.find_with_hint(tag::MDEntrySize, hh3).try_as_decimal(sz_m, sz_e));
        EXPECT_EQ(sz_m, seen == 0 ? 500 : 300);
        EXPECT_EQ(sz_e, 0);
        EXPECT_FALSE(idx.has(tag::Symbol));
        ++seen;
    });
    EXPECT_EQ(seen, 2);
}

TEST(NanofixTest, indexed_message_handles_large_message_above_64k) {
    constexpr std::size_t kBig = std::size_t{70} * 1024;
    std::vector<char> buf(kBig, 'A');
    message_writer w(buf.data(), buf.data() + kBig);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "B");
    std::string filler(std::size_t{64} * 1024, 'X');
    w.push_back_string(tag::Text, filler);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf.data(), w.message_end());
    ASSERT_TRUE(r.is_valid());
    ASSERT_GT(r.message_size(), 0xFFFFu);

    nanofix::field_index_buffer<32> idx_buffer;
    auto idx = nanofix::build_field_index(r, idx_buffer);
    EXPECT_FALSE(idx.truncated());
    EXPECT_GT(idx.field_count(), 0u);

    // Round-trip the large Text field through value_at.
    std::size_t hint = 0;
    auto v = idx.find_with_hint(tag::Text, hint);
    EXPECT_EQ(v.bytes().size(), filler.size());
    EXPECT_EQ(v.as_string_view(), filler);
}

TEST(NanofixTest, indexed_message_truncated) {
    char buf[2048] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    for (int i = 0; i < 16; ++i) {
        std::string val = "v" + std::to_string(i);
        w.push_back_string(10000 + i, val);
    }
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    nanofix::field_index_buffer<8> idx_buffer;
    auto idx = nanofix::build_field_index(r, idx_buffer);
    EXPECT_TRUE(idx.truncated());
    EXPECT_EQ(idx.field_count(), 0u);
    // Truncated index reports absence for every tag; caller must fall
    // back to iterator-based lookup on r.
    std::size_t h = 0;
    EXPECT_EQ(idx.find_with_hint(10000, h).size(), 0u);
    EXPECT_FALSE(idx.has(10000));
}

TEST(NanofixTest, checksum_roundtrip) {
    char buf[512] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_string(tag::SenderCompID, "ALICE");
    w.push_back_string(tag::TargetCompID, "BOB");
    w.push_back_int(tag::MsgSeqNum, 101);
    w.push_back_string(tag::ClOrdID, "ORD-XYZ-001");
    w.push_back_char(tag::Side, '1');
    w.push_back_string(tag::Symbol, "MSFT");
    w.push_back_string(tag::SecurityID, "US5949181045");
    w.push_back_int(tag::OrderQty, 500);
    w.push_back_decimal(tag::Price, 41523, -2);
    w.push_back_char(tag::OrdType, '2');
    w.push_back_string(tag::TimeInForce, "0");
    w.push_back_string(tag::Account, "ACCT-7");
    w.push_back_string(tag::Text, "vectorized checksum smoke test");
    ASSERT_TRUE(w.template push_back_trailer<true>());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    unsigned const written = r.check_sum()->value().as_int_unchecked<unsigned>();
    unsigned const computed = r.calculate_check_sum();
    EXPECT_EQ(written, computed);
    EXPECT_LT(written, 256u);
}

TEST(NanofixTest, reader_copy_preserves_state_without_reparse) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_int(tag::MsgSeqNum, 42);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    ASSERT_TRUE(r.is_complete());

    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization) — exercises the copy ctor
    message_reader copy = r;
    EXPECT_EQ(copy.is_valid(), r.is_valid());
    EXPECT_EQ(copy.is_complete(), r.is_complete());
    EXPECT_EQ(copy.message_begin(), r.message_begin());
    EXPECT_EQ(copy.message_end(), r.message_end());
    EXPECT_EQ(copy.message_size(), r.message_size());
    EXPECT_EQ(copy.begin()->tag(), r.begin()->tag());
    EXPECT_EQ(copy.calculate_check_sum(), r.calculate_check_sum());

    message_reader assigned(buf, buf);
    assigned = r;
    EXPECT_EQ(assigned.is_valid(), r.is_valid());
    EXPECT_EQ(assigned.message_end(), r.message_end());
    EXPECT_EQ(assigned.begin()->tag(), r.begin()->tag());

    static_assert(std::is_nothrow_copy_constructible_v<message_reader>);
    static_assert(std::is_nothrow_copy_assignable_v<message_reader>);
    static_assert(std::is_nothrow_move_constructible_v<message_reader>);
    static_assert(std::is_nothrow_move_assignable_v<message_reader>);
}

namespace {
template <class T>
[[nodiscard]] bool parse_int(std::string_view s, T& out) {
    return ::nanofix::detail::try_atoi<T>(s.data(), s.data() + s.size(), out);
}

template <class T>
[[nodiscard]] bool parse_uint(std::string_view s, T& out) {
    return ::nanofix::detail::try_atou<T>(s.data(), s.data() + s.size(), out);
}

template <class T>
[[nodiscard]] bool parse_dec(std::string_view s, T& m, T& e) {
    return ::nanofix::detail::try_atod<T>(s.data(), s.data() + s.size(), m, e);
}
}  // namespace

TEST(NanofixTest, try_atoi_overflow_rejected_no_ub) {
    int64_t v = 0;
    EXPECT_TRUE(parse_int<int64_t>("9223372036854775807", v));
    EXPECT_EQ(v, std::numeric_limits<int64_t>::max());

    EXPECT_TRUE(parse_int<int64_t>("-9223372036854775808", v));
    EXPECT_EQ(v, std::numeric_limits<int64_t>::min());

    EXPECT_FALSE(parse_int<int64_t>("9223372036854775808", v));  // INT64_MAX + 1
    EXPECT_FALSE(parse_int<int64_t>("-9223372036854775809", v));
    EXPECT_FALSE(parse_int<int64_t>("99999999999999999999", v));

    int32_t w = 0;
    EXPECT_TRUE(parse_int<int32_t>("2147483647", w));
    EXPECT_EQ(w, std::numeric_limits<int32_t>::max());
    EXPECT_TRUE(parse_int<int32_t>("-2147483648", w));
    EXPECT_EQ(w, std::numeric_limits<int32_t>::min());
    EXPECT_FALSE(parse_int<int32_t>("2147483648", w));
    EXPECT_FALSE(parse_int<int32_t>("-2147483649", w));
}

TEST(NanofixTest, try_atou_overflow_rejected) {
    uint64_t v = 0;
    EXPECT_TRUE(parse_uint<uint64_t>("18446744073709551615", v));
    EXPECT_EQ(v, std::numeric_limits<uint64_t>::max());
    EXPECT_FALSE(parse_uint<uint64_t>("18446744073709551616", v));
    EXPECT_FALSE(parse_uint<uint64_t>("99999999999999999999", v));

    uint32_t w = 0;
    EXPECT_TRUE(parse_uint<uint32_t>("4294967295", w));
    EXPECT_FALSE(parse_uint<uint32_t>("4294967296", w));
}

TEST(NanofixTest, try_atod_overflow_rejected_no_ub) {
    int64_t m = 0, e = 0;
    EXPECT_TRUE(parse_dec<int64_t>("9223372036854775807", m, e));
    EXPECT_EQ(m, std::numeric_limits<int64_t>::max());
    EXPECT_EQ(e, 0);

    EXPECT_TRUE(parse_dec<int64_t>("-9223372036854775808", m, e));
    EXPECT_EQ(m, std::numeric_limits<int64_t>::min());
    EXPECT_EQ(e, 0);

    EXPECT_TRUE(parse_dec<int64_t>("123.45", m, e));
    EXPECT_EQ(m, 12345);
    EXPECT_EQ(e, -2);

    EXPECT_FALSE(parse_dec<int64_t>("92233720368547758.08", m, e));  // mantissa overflows
    EXPECT_FALSE(parse_dec<int64_t>("1.2.3", m, e));                 // two dots
    EXPECT_FALSE(parse_dec<int64_t>(".", m, e));                     // no digits
    EXPECT_FALSE(parse_dec<int64_t>("-", m, e));
    EXPECT_FALSE(parse_dec<int64_t>("", m, e));
}

TEST(NanofixTest, as_epoch_nanos_rejects_out_of_range_year_no_overflow) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "0");
    w.push_back_string(tag::SendingTime, "99999999-00:00:00.000000000");
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    auto it = r.begin();
    ASSERT_TRUE(r.find_with_hint(tag::SendingTime, it));
    EXPECT_FALSE(it->value().as_epoch_nanos().has_value());
    EXPECT_FALSE(it->value().as_epoch_millis().has_value());
}

// Fuzzer (crash-e96d1e3c): non-digit subsecond bytes drove the SWAR parser to
// an out-of-range value whose * 1e6 scaling overflowed int (UBSan abort).
// Pre-existing on the iterator/timestamp path; rejected cleanly now.
TEST(Regression, atotime_nano_non_digit_subsecond_no_overflow) {
    int h = 0, mi = 0, s = 0, ns = 0;
    auto parse = [&](std::string_view sv) {
        return nanofix::detail::atotime_nano(sv.data(), sv.data() + sv.size(), h, mi, s, ns);
    };
    // .sss / .ssssss / .sssssssss widths, each with non-digit fraction.
    EXPECT_FALSE(parse("09:30:00.SEP"));
    EXPECT_FALSE(parse("09:30:00.12ABCD"));
    EXPECT_FALSE(parse("09:30:00.1234567XY"));
    // Valid fractions of each width still parse.
    EXPECT_TRUE(parse("09:30:00.123"));
    EXPECT_EQ(ns, 123000000);
    EXPECT_TRUE(parse("09:30:00.123456"));
    EXPECT_EQ(ns, 123456000);
    EXPECT_TRUE(parse("09:30:00.123456789"));
    EXPECT_EQ(ns, 123456789);
}

TEST(Regression, as_epoch_nanos_non_digit_subsecond_rejected) {
    using namespace nanofix;
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "0");
    w.push_back_string(tag::SendingTime, "20240115-09:30:00.SEP");
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    auto it = r.begin();
    ASSERT_TRUE(r.find_with_hint(tag::SendingTime, it));
    EXPECT_FALSE(it->value().as_epoch_nanos().has_value());
}

TEST(Regression, as_epoch_fully_validates_digits_and_clock_ranges) {
    auto v = [](char const* s) { return field_value(s, s + std::strlen(s)); };
    // Non-digit fraction decoded as digits: epoch off, has_value() true.
    EXPECT_FALSE(v("20240115-09:30:00.SEP").as_epoch_millis().has_value());
    // Out-of-range clock parts rolled into the next day.
    EXPECT_FALSE(v("20240115-99:99:99").as_epoch_millis().has_value());
    EXPECT_FALSE(v("20240115-99:99:99").as_epoch_nanos().has_value());
    EXPECT_FALSE(v("20240115X09:30:00").as_epoch_millis().has_value());  // bad separator
    EXPECT_FALSE(v("2024011X-09:30:00").as_epoch_millis().has_value());  // non-digit date

    EXPECT_TRUE(v("20240115-23:59:60").as_epoch_millis().has_value());  // leap second
    EXPECT_TRUE(v("20240115-09:30:00.250").as_epoch_millis().has_value());

    std::chrono::sys_time<std::chrono::milliseconds> tp;
    EXPECT_FALSE(v("20240115-99:99:99").as_timestamp(tp));  // same tier as epoch
}

TEST(Regression, calendar_day_in_month_validated) {
    using namespace std::chrono;
    auto v = [](char const* s) { return field_value(s, s + std::strlen(s)); };

    sys_time<milliseconds> tp;
    EXPECT_FALSE(v("20240231-12:00:00").try_as_timestamp(tp));  // Feb 31
    EXPECT_FALSE(v("20230229-12:00:00").try_as_timestamp(tp));  // non-leap Feb 29
    EXPECT_TRUE(v("20240229-12:00:00").try_as_timestamp(tp));   // leap Feb 29
    EXPECT_FALSE(v("20240431-12:00:00").try_as_timestamp(tp));  // Apr 31
    EXPECT_FALSE(v("20240230-12:00:00").as_epoch_millis().has_value());
    EXPECT_FALSE(v("20240230-12:00:00").as_epoch_nanos().has_value());

    year_month_day ymd{};
    EXPECT_FALSE(v("20240230").try_as_date(ymd));
    EXPECT_FALSE(v("21000229").try_as_date(ymd));  // century year, not leap
    EXPECT_TRUE(v("20000229").try_as_date(ymd));   // 400-year rule, leap
}

TEST(NanofixTest, writer_rejects_non_positive_tag) {
    auto emits_error = [](auto&& fn) {
        char buf[2048];
        message_writer w(buf, sizeof(buf));
        fn(w);
        return !w.ok() && w.message_size() == 0;
    };
    // A negative tag would silently emit its unsigned wrap with ok() true.
    EXPECT_TRUE(emits_error([](message_writer& w) { w.push_back_int(-1, 5); }));
    EXPECT_TRUE(emits_error([](message_writer& w) { w.push_back_string(0, "X"); }));
    EXPECT_TRUE(emits_error([](message_writer& w) { w.push_back_decimal(-5, 100L, -2L); }));
    EXPECT_TRUE(emits_error([](message_writer& w) { w.push_back_data(-95, 96, "D"); }));
    EXPECT_TRUE(emits_error([](message_writer& w) { w.push_back_data(95, -96, "D"); }));
}

#ifndef NANOFIX_ASSERT_FAILFAST
TEST(NanofixTest, inverted_range_writer_ctor_is_guarded) {
    // Reader's (begin, end) ctor is deliberately unguarded (hot path, ~20%
    // measured cost — see the ctor doc); the writer ctor is cold and clamps.
    char buf[64] = {};

    reset_assert_failure_count();
    message_writer w(buf + 32, buf);  // end < begin: clamps to empty, asserts
    EXPECT_EQ(assert_failure_count(), 1u);
    EXPECT_EQ(w.buffer_size(), 0u);
    w.push_back_header("FIX.4.2");
    EXPECT_FALSE(w.ok());
    reset_assert_failure_count();
}
#endif

TEST(NanofixTest, trailer_rejects_body_over_six_digits) {
    // >999999-byte body must error, not wrap the 6-digit backpatch.
    std::vector<char> buf(1'100'000);
    std::vector<char> big(1'000'100, 'X');
    message_writer w(buf.data(), buf.size());
    w.push_back_header("FIX.4.2");
    w.push_back_string(tag::MsgType, "B");
    w.push_back_string(tag::Text, std::string_view(big.data(), big.size()));
    ASSERT_TRUE(w.ok());
    EXPECT_FALSE(w.push_back_trailer());
    EXPECT_FALSE(w.ok());
}

TEST(NanofixTest, group_last_entry_absorbs_trailing_message_fields) {
    // Sharp edge pinned: the group view extends to r.end(), so message-level
    // fields after the last entry land inside it.
    char buf[256];
    message_writer w(buf, sizeof(buf));
    w.push_back_header("FIX.4.2");
    w.push_back_string(tag::MsgType, "W");
    w.push_back_int(tag::NoMDEntries, 1);
    w.push_back_char(tag::MDEntryType, '0');
    w.push_back_decimal(tag::MDEntryPx, 10000L, -2L);
    w.push_back_string(tag::Text, "after-group");  // message-level trailing field
    ASSERT_TRUE(w.push_back_trailer());
    message_reader r(w);
    ASSERT_TRUE(r.is_complete() && r.is_valid());

    r.group(tag::NoMDEntries, tag::MDEntryType).for_each([](group_entry const& e) {
        auto text = e.find(tag::Text);  // absorbed into the final entry
        EXPECT_FALSE(text.empty());
        EXPECT_EQ(text.as_string_view(), "after-group");
    });
}

TEST(NanofixTest, second_push_back_trailer_rejected) {
    char buf[256];
    message_writer w(buf, sizeof(buf));
    w.push_back_header("FIX.4.2");
    w.push_back_string(tag::MsgType, "0");
    ASSERT_TRUE(w.push_back_trailer());
    std::string const wire(w.message_begin(), w.message_end());

    // Second trailer would bury the first 10= inside the body.
    EXPECT_FALSE(w.push_back_trailer());
    EXPECT_FALSE(w.ok());
    EXPECT_EQ(std::string(w.message_begin(), w.message_end()), wire);
}

TEST(NanofixTest, increment_terminates_on_malformed_data_length_field) {
    char const wire[] =
        "8=FIX.4.2\x01"
        "9=000075\x01"
        "35=n\x01"
        "49=ALICE\x01"
        "56=BOB\x01"
        "34=1\x01"
        "52=20240115-09:30:00.000\x01"
        "95=14\x01"
        "96:\x01hello\x01world\x02\x03\x01"
        "10=241\x01";
    auto const wire_len = sizeof(wire) - 1;

    message_reader r(wire, wire + wire_len);
    if (!r.is_valid() || !r.is_complete())
        return;
    std::size_t field_count = 0;
    for (auto it = r.begin(); it != r.end(); ++it) {
        if (++field_count > 1024) {
            FAIL() << "iterator did not terminate on malformed data-length field";
        }
    }
}

TEST(NanofixTest, calculate_check_sum_callable_via_const_ref) {
    char buf[64] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "A");
    ASSERT_TRUE(w.push_back_trailer());

    message_reader const r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    auto const ck = r.calculate_check_sum();
    EXPECT_EQ(ck, r.check_sum()->value().as_int_unchecked<unsigned char>());
    static_assert(noexcept(r.calculate_check_sum()));
}

TEST(NanofixTest, group_overstated_count_does_not_ub) {
    // NoMDEntries=5 on wire, 2 entries in body; iteration stops at 2.
    char buf[1024] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "W");
    w.push_back_int(tag::NoMDEntries, 5);
    w.push_back_char(tag::MDEntryType, '0');
    w.push_back_decimal(tag::MDEntryPx, 10000, -2);
    w.push_back_int(tag::MDEntrySize, 100);
    w.push_back_char(tag::MDEntryType, '1');
    w.push_back_decimal(tag::MDEntryPx, 20000, -2);
    w.push_back_int(tag::MDEntrySize, 200);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    auto md = r.group(tag::NoMDEntries, tag::MDEntryType);
    std::vector<char> seen_types;
    md.for_each([&](group_entry const& entry) {
        auto it = entry.begin();
        ASSERT_TRUE(entry.find_with_hint(tag::MDEntryType, it));
        seen_types.push_back(it->value().as_char_unchecked());
    });
    ASSERT_EQ(seen_types.size(), 2u);
    EXPECT_EQ(seen_types[0], '0');
    EXPECT_EQ(seen_types[1], '1');
}

TEST(NanofixTest, group_garbage_count_returns_empty) {
    // Non-numeric NoMDEntries; group() returns empty.
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "W");
    w.push_back_string(tag::NoMDEntries, "abc");
    w.push_back_char(tag::MDEntryType, '0');
    w.push_back_decimal(tag::MDEntryPx, 10000, -2);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    auto md = r.group(tag::NoMDEntries, tag::MDEntryType);
    EXPECT_TRUE(md.empty());
    EXPECT_EQ(md.size(), 0u);
    int seen = 0;
    md.for_each([&](group_entry const& e) {
        (void)e;
        ++seen;
    });
    EXPECT_EQ(seen, 0);
}

namespace {

struct msg_buffer {
    char buf[8192]{};
    char* cursor = buf;

    void append(std::string_view msg_type, int seq) {
        message_writer w(cursor, buf + sizeof(buf));
        w.push_back_header("FIXT.1.1");
        w.push_back_string(tag::MsgType, msg_type);
        w.push_back_int(tag::MsgSeqNum, seq);
        if (w.push_back_trailer())
            cursor = w.message_end();
    }

    char const* data() const noexcept { return buf; }

    std::size_t size() const noexcept { return static_cast<std::size_t>(cursor - buf); }

    char const* end() const noexcept { return cursor; }
};

}  // namespace

TEST(NanofixTest, messages_range_empty_buffer) {
    char buf[8] = {};
    auto rng = messages(buf, buf);
    int seen = 0;
    for (auto const& r : rng) {
        (void)r;
        ++seen;
    }
    EXPECT_EQ(seen, 0);
    EXPECT_EQ(rng.begin().remainder(), buf);
}

TEST(NanofixTest, messages_range_single_message) {
    msg_buffer mb;
    mb.append("A", 1);

    auto rng = messages(mb.data(), mb.size());
    int seen = 0;
    int last_seq = 0;
    for (auto const& r : rng) {
        ASSERT_TRUE(r.is_complete());
        ASSERT_TRUE(r.is_valid());
        auto it = r.begin();
        ASSERT_TRUE(r.find_with_hint(tag::MsgSeqNum, it));
        ASSERT_TRUE(it->value().try_as_int(last_seq));
        ++seen;
    }
    EXPECT_EQ(seen, 1);
    EXPECT_EQ(last_seq, 1);

    auto it = rng.begin();
    while (it != rng.end())
        ++it;
    EXPECT_EQ(it.remainder(), mb.end());
}

TEST(NanofixTest, messages_range_three_concatenated) {
    msg_buffer mb;
    mb.append("A", 1);
    mb.append("0", 2);
    mb.append("D", 3);

    std::vector<int> seqs;
    auto rng = messages(mb.data(), mb.size());
    for (auto const& r : rng) {
        ASSERT_TRUE(r.is_valid());
        auto it = r.begin();
        ASSERT_TRUE(r.find_with_hint(tag::MsgSeqNum, it));
        int s = 0;
        ASSERT_TRUE(it->value().try_as_int(s));
        seqs.push_back(s);
    }
    EXPECT_EQ(seqs, (std::vector<int>{1, 2, 3}));

    auto it = rng.begin();
    while (it != rng.end())
        ++it;
    EXPECT_EQ(it.remainder(), mb.end());
}

TEST(NanofixTest, messages_range_truncated_tail) {
    msg_buffer mb;
    mb.append("A", 1);
    mb.append("0", 2);
    char const* second_msg_end = mb.end();
    mb.append("D", 3);
    char const* third_msg_end = mb.end();

    // Strip last 5 bytes (mid-message): tail must be the start of msg #3.
    std::size_t truncated_len = mb.size() - 5;
    char const* truncated_end = mb.data() + truncated_len;

    std::vector<int> seqs;
    auto rng = messages(mb.data(), truncated_end);
    auto it = rng.begin();
    for (; it != rng.end(); ++it) {
        int s = 0;
        auto fit = it->begin();
        ASSERT_TRUE(it->find_with_hint(tag::MsgSeqNum, fit));
        ASSERT_TRUE(fit->value().try_as_int(s));
        seqs.push_back(s);
    }
    EXPECT_EQ(seqs, (std::vector<int>{1, 2}));
    EXPECT_EQ(it.remainder(), second_msg_end);
    EXPECT_NE(it.remainder(), third_msg_end);
}

TEST(NanofixTest, for_each_message_returns_tail) {
    msg_buffer mb;
    mb.append("A", 7);
    mb.append("0", 8);
    char const* full_tail = mb.end();

    std::vector<int> seqs;
    char const* tail = for_each_message(mb.data(), full_tail, [&](message_reader const& r) {
        ASSERT_TRUE(r.is_valid());
        auto it = r.begin();
        ASSERT_TRUE(r.find_with_hint(tag::MsgSeqNum, it));
        int s = 0;
        ASSERT_TRUE(it->value().try_as_int(s));
        seqs.push_back(s);
    });
    EXPECT_EQ(seqs, (std::vector<int>{7, 8}));
    EXPECT_EQ(tail, full_tail);
}

TEST(NanofixTest, for_each_message_partial_tail_pointer) {
    msg_buffer mb;
    mb.append("A", 1);
    char const* msg1_end = mb.end();
    mb.append("D", 2);

    std::size_t partial = mb.size() - 3;
    char const* partial_end = mb.data() + partial;

    int count = 0;
    char const* tail =
        for_each_message(mb.data(), partial_end, [&](message_reader const&) { ++count; });
    EXPECT_EQ(count, 1);
    EXPECT_EQ(tail, msg1_end);
}

TEST(NanofixTest, messages_range_skips_invalid_with_resync) {
    msg_buffer mb;
    mb.append("A", 1);
    // Splat random bytes into the middle, then append more valid messages.
    // The reader will hit a bad frame after msg 1, then next_message_reader
    // resyncs by scanning forward for "8=FIX".
    std::memcpy(mb.cursor, "garbage_bytes_not_a_fix_frame", 29);
    mb.cursor += 29;
    mb.append("D", 2);

    std::vector<int> seqs;
    for (auto const& r : messages(mb.data(), mb.size())) {
        auto it = r.begin();
        ASSERT_TRUE(r.find_with_hint(tag::MsgSeqNum, it));
        int s = 0;
        ASSERT_TRUE(it->value().try_as_int(s));
        seqs.push_back(s);
    }
    EXPECT_EQ(seqs, (std::vector<int>{1, 2}));
}

TEST(IsKnownTag, accepts_spec_tags) {
    EXPECT_TRUE(is_known_tag(tag::MsgType));
    EXPECT_TRUE(is_known_tag(tag::OrderQty));
    EXPECT_TRUE(is_known_tag(tag::CheckSum));
    EXPECT_TRUE(is_known_tag(tag::EncodedUnderlyingProvisionText));  // 42172
}

TEST(IsKnownTag, rejects_out_of_range) {
    EXPECT_FALSE(is_known_tag(0));
    EXPECT_FALSE(is_known_tag(-1));
    EXPECT_FALSE(is_known_tag(std::numeric_limits<int>::min()));
    EXPECT_FALSE(is_known_tag(detail::known_tag_max + 1));
    EXPECT_FALSE(is_known_tag(std::numeric_limits<int>::max()));
}

TEST(SpanOverloads, writer_and_reader_round_trip_through_span) {
    std::array<char, 256> buf{};
    message_writer w{std::span<char>(buf)};
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_int(tag::OrderQty, 42);
    ASSERT_TRUE(w.push_back_trailer());

    std::span<char const> view{buf.data(), static_cast<std::size_t>(w.message_end() - buf.data())};
    message_reader r{view};
    ASSERT_TRUE(r.is_complete());
    ASSERT_TRUE(r.is_valid());

    message_reader::const_iterator i = r.begin();
    ASSERT_TRUE(r.find_with_hint(tag::OrderQty, i));
    EXPECT_EQ(i->value().as_int_unchecked<int>(), 42);
}

TEST(SpanOverloads, messages_and_for_each_message_accept_span) {
    char buf[512] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "0");
    ASSERT_TRUE(w.push_back_trailer());
    auto first_end = w.message_end();

    message_writer w2(first_end, buf + sizeof(buf));
    w2.push_back_header("FIXT.1.1");
    w2.push_back_string(tag::MsgType, "0");
    ASSERT_TRUE(w2.push_back_trailer());

    std::span<char const> view{buf, static_cast<std::size_t>(w2.message_end() - buf)};

    int count_from_range = 0;
    for (auto const& m : nanofix::messages(view)) {
        EXPECT_TRUE(m.is_valid());
        ++count_from_range;
    }
    EXPECT_EQ(count_from_range, 2);

    int count_from_callback = 0;
    char const* tail = nanofix::for_each_message(view, [&](message_reader const& m) noexcept {
        EXPECT_TRUE(m.is_valid());
        ++count_from_callback;
    });
    EXPECT_EQ(count_from_callback, 2);
    EXPECT_EQ(tail, view.data() + view.size());
}

TEST(Regression, prefix_accessors_are_noexcept) {
    static_assert(noexcept(std::declval<message_reader const&>().prefix_begin()));
    static_assert(noexcept(std::declval<message_reader const&>().prefix_end()));
    static_assert(noexcept(std::declval<message_reader const&>().prefix_size()));
}

TEST(Regression, find_soh_handles_empty_range) {
    char const* p = "";
    EXPECT_EQ(nanofix::detail::find_soh(p, p), p);
}

TEST(Regression, find_soh_returns_end_on_no_soh) {
    char const buf[] = "no soh here";
    EXPECT_EQ(nanofix::detail::find_soh(buf, buf + sizeof(buf) - 1), buf + sizeof(buf) - 1);
}

TEST(Regression, find_soh_finds_first_soh) {
    char const buf[] =
        "abc\x01"
        "def\x01";
    EXPECT_EQ(nanofix::detail::find_soh(buf, buf + sizeof(buf) - 1), buf + 3);
}

TEST(Regression, data_length_non_digit_does_not_ub) {
    // Garbage RawDataLength (95) must not propagate as data-len offset.
    char const buf[] =
        "8=FIX.4.4\x01"
        "9=24\x01"
        "35=D\x01"
        "95=abc\x01"
        "96=hello\x01"
        "10=000\x01";
    message_reader r(buf, buf + sizeof(buf) - 1);
    if (r.is_complete() && r.is_valid()) {
        std::size_t fields = 0;
        for (auto it = r.begin(); it != r.end(); ++it) {
            ++fields;
            ASSERT_LT(fields, 32u);  // termination guard
        }
    }
}

TEST(Regression, body_length_above_cap_classified_invalid) {
    char const buf[] =
        "8=FIX.4.4\x01"
        "9=9999999999\x01"
        "35=D\x01"
        "10=000\x01";
    message_reader r(buf, buf + sizeof(buf) - 1);
    EXPECT_TRUE(r.is_complete());
    EXPECT_FALSE(r.is_valid());
}

TEST(Regression, time_parsers_are_noexcept) {
    static_assert(noexcept(std::declval<nanofix::field_value const&>().as_date(
        std::declval<int&>(), std::declval<int&>(), std::declval<int&>())));
    static_assert(noexcept(std::declval<nanofix::field_value const&>().as_monthyear(
        std::declval<int&>(), std::declval<int&>())));
    static_assert(noexcept(std::declval<nanofix::field_value const&>().as_timeonly(
        std::declval<int&>(), std::declval<int&>(), std::declval<int&>(), std::declval<int&>())));
    static_assert(noexcept(std::declval<nanofix::field_value const&>().as_timeonly_nano(
        std::declval<int&>(), std::declval<int&>(), std::declval<int&>(), std::declval<int&>())));
    static_assert(
        noexcept(std::declval<nanofix::field_value const&>().as_timestamp(std::declval<int&>(),
                                                                          std::declval<int&>(),
                                                                          std::declval<int&>(),
                                                                          std::declval<int&>(),
                                                                          std::declval<int&>(),
                                                                          std::declval<int&>(),
                                                                          std::declval<int&>())));
    static_assert(noexcept(
        std::declval<nanofix::field_value const&>().as_timestamp_nano(std::declval<int&>(),
                                                                      std::declval<int&>(),
                                                                      std::declval<int&>(),
                                                                      std::declval<int&>(),
                                                                      std::declval<int&>(),
                                                                      std::declval<int&>(),
                                                                      std::declval<int&>())));
}

TEST(Regression, as_timeonly_rejects_microsecond_precision) {
    char const buf[] = "12:34:56.123456";
    char wb[64];
    message_writer w(wb);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "0");
    w.push_back_string(60, buf);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(wb, w.message_end());
    ASSERT_TRUE(r.is_valid());
    auto it = r.begin();
    ASSERT_TRUE(r.find_with_hint(60, it));
    int h = -1, m = -1, s = -1, ms = -1;
    EXPECT_FALSE(it->value().as_timeonly(h, m, s, ms));

    int hn, mn, sn, ns;
    ASSERT_TRUE(it->value().as_timeonly_nano(hn, mn, sn, ns));
    EXPECT_EQ(hn, 12);
    EXPECT_EQ(ns, 123456000);
}

TEST(Regression, field_value_equals_cstring_handles_short_field_and_long_cstring) {
    char wb[64];
    message_writer w(wb);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "A");
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(wb, w.message_end());
    ASSERT_TRUE(r.is_valid());
    auto it = r.begin();
    EXPECT_TRUE(it->value() == "A");
    EXPECT_FALSE(it->value() == "AB");
    EXPECT_FALSE(it->value() == "");
}

TEST(Regression, field_value_equals_string_view_uses_length_first) {
    char wb[64];
    message_writer w(wb);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "ABC");
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(wb, w.message_end());
    ASSERT_TRUE(r.is_valid());
    auto it = r.begin();
    EXPECT_TRUE(it->value() == "ABC"sv);
    EXPECT_FALSE(it->value() == "AB"sv);
    EXPECT_FALSE(it->value() == "ABCD"sv);
}

TEST(Regression, atotime_strict_ms_only) {
    int h, m, s, ms;
    char const t08[] = "12:34:56";
    char const t12[] = "12:34:56.789";
    char const t15[] = "12:34:56.789012";
    char const t18[] = "12:34:56.789012345";
    char const tbad[] = "12:34:56.78";

    ASSERT_TRUE(nanofix::detail::atotime(t08, t08 + 8, h, m, s, ms));
    EXPECT_EQ(ms, 0);
    ASSERT_TRUE(nanofix::detail::atotime(t12, t12 + 12, h, m, s, ms));
    EXPECT_EQ(ms, 789);
    EXPECT_FALSE(nanofix::detail::atotime(t15, t15 + 15, h, m, s, ms));
    EXPECT_FALSE(nanofix::detail::atotime(t18, t18 + 18, h, m, s, ms));
    EXPECT_FALSE(nanofix::detail::atotime(tbad, tbad + 11, h, m, s, ms));
}

TEST(Regression, try_write_message_accepts_rvalue_lambda_without_copy) {
    char buf[256];
    char* end_out = nullptr;
    int captured = 0;
    bool ok = nanofix::try_write_message(std::span<char>(buf), end_out, [&](message_writer& w) {
        w.push_back_header("FIXT.1.1");
        w.push_back_string(tag::MsgType, "A");
        ++captured;
    });
    EXPECT_TRUE(ok);
    EXPECT_EQ(captured, 1);
    EXPECT_NE(end_out, nullptr);
}

TEST(Regression, try_write_message_forwards_move_only_functor) {
    struct MoveOnly {
        std::unique_ptr<int> sentinel{std::make_unique<int>(42)};

        void operator()(message_writer& w) const {
            w.push_back_header("FIXT.1.1");
            w.push_back_string(tag::MsgType, "0");
        }
    };

    char buf[256];
    char* end_out = nullptr;
    MoveOnly f;
    bool ok = nanofix::try_write_message(std::span<char>(buf), end_out, std::move(f));
    EXPECT_TRUE(ok);
}

TEST(Regression, timepointtoparts_pre_epoch_floored) {
    using namespace std::chrono;
    int y, mo, d, h, mi, s, ms;
    // 1969-12-31 23:59:59.999
    auto tp = sys_time<milliseconds>{milliseconds{-1}};
    ASSERT_TRUE(nanofix::detail::timepointtoparts(tp, y, mo, d, h, mi, s, ms));
    EXPECT_EQ(y, 1969);
    EXPECT_EQ(mo, 12);
    EXPECT_EQ(d, 31);
    EXPECT_EQ(h, 23);
    EXPECT_EQ(mi, 59);
    EXPECT_EQ(s, 59);
    EXPECT_EQ(ms, 999);
}

TEST(Regression, timepointtoparts_nano_roundtrip_through_writer_reader) {
    char buf[256];
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "0");
    auto tp = std::chrono::sys_time<std::chrono::nanoseconds>{
        std::chrono::nanoseconds{1'700'000'000'123'456'789LL}};
    w.push_back_timestamp_nano(60, tp);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    auto it = r.begin();
    ASSERT_TRUE(r.find_with_hint(60, it));
    auto back = it->value().as_epoch_nanos();
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back.value(), 1'700'000'000'123'456'789LL);
}

TEST(NanofixTest, version_macros) {
    static_assert(NANOFIX_VERSION_MAJOR >= 0);
    static_assert(NANOFIX_VERSION_MINOR >= 0);
    static_assert(NANOFIX_VERSION_PATCH >= 0);
    // Optional suffix past the triple is git build metadata ("+5.gabc1234").
    std::string const base = std::to_string(NANOFIX_VERSION_MAJOR) + "." +
                             std::to_string(NANOFIX_VERSION_MINOR) + "." +
                             std::to_string(NANOFIX_VERSION_PATCH);
    std::string_view const full = NANOFIX_VERSION;
    ASSERT_GE(full.size(), base.size());
    EXPECT_EQ(full.substr(0, base.size()), base);
    if (full.size() > base.size())
        EXPECT_EQ(full[base.size()], '+');
}
