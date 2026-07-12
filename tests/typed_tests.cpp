// Category gating on typed_value, and typed find()/find_with_hint across the
// access paths.

#include <gtest/gtest.h>

#include <nanofix.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <type_traits>

using namespace nanofix;

namespace {
field_value make_value(char const* s) {
    return field_value(s, s + std::char_traits<char>::length(s));
}
}  // namespace

TEST(TypedValue, DecimalCategoryReadsDecimal) {
    typed_value<fix_type::decimal> v(make_value("50001"));
    long mant = 0, exp = 0;
    EXPECT_TRUE(v.try_as_decimal(mant, exp));
    EXPECT_EQ(mant, 50001);
    EXPECT_EQ(exp, 0);
}

TEST(TypedValue, IntegerCategoryReadsInt) {
    typed_value<fix_type::integer> v(make_value("100"));
    int n = 0;
    EXPECT_TRUE(v.try_as_int(n));
    EXPECT_EQ(n, 100);
}

TEST(TypedValue, CharacterCategoryReadsChar) {
    typed_value<fix_type::character> v(make_value("1"));
    char c = 0;
    ASSERT_TRUE(v.try_as_char(c));
    EXPECT_EQ(c, '1');
}

TEST(TypedValue, DateAndMonthyearCategoriesRead) {
    typed_value<fix_type::date> d(make_value("20260712"));
    int y = 0, m = 0, day = 0;
    ASSERT_TRUE(d.as_date(y, m, day));
    EXPECT_EQ(y, 2026);
    EXPECT_EQ(m, 7);
    EXPECT_EQ(day, 12);

    typed_value<fix_type::monthyear> my(make_value("202607"));
    ASSERT_TRUE(my.as_monthyear(y, m));
    EXPECT_EQ(y, 2026);
    EXPECT_EQ(m, 7);
}

TEST(TypedValue, TimestampCategoryReadsTimestamp) {
    typed_value<fix_type::timestamp> v(make_value("20260712-12:30:05.250"));
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
    ASSERT_TRUE(v.as_timestamp(y, mo, d, h, mi, s, ms));
    EXPECT_EQ(y, 2026);
    EXPECT_EQ(ms, 250);

    std::chrono::sys_time<std::chrono::nanoseconds> tp;
    ASSERT_TRUE(v.as_timestamp_nano(tp));
}

TEST(TypedValue, TypedTierFullChronoValidation) {
    // try_as_timestamp is the fully validating tier: the length-only
    // as_timestamp accepts this garbage, the named try_as_* must not.
    typed_value<fix_type::timestamp> bad(make_value("2026071X-12:30:05"));
    std::chrono::sys_time<std::chrono::milliseconds> tp;
    EXPECT_FALSE(bad.try_as_timestamp(tp));
    typed_value<fix_type::timestamp> good(make_value("20260712-12:30:05"));
    EXPECT_TRUE(good.try_as_timestamp(tp));

    typed_value<fix_type::timeonly> t(make_value("12:30:05"));
    std::chrono::nanoseconds dur{};
    EXPECT_TRUE(t.try_as_timeonly(dur));

    typed_value<fix_type::date> d(make_value("20260712"));
    std::chrono::year_month_day ymd{};
    EXPECT_TRUE(d.try_as_date(ymd));

    typed_value<fix_type::monthyear> my(make_value("202607"));
    std::chrono::year_month ym{};
    EXPECT_TRUE(my.try_as_monthyear(ym));
}

TEST(TypedValue, TimeonlyCategoryReadsTimeonly) {
    typed_value<fix_type::timeonly> v(make_value("12:30:05.250"));
    int h = 0, m = 0, s = 0, ms = 0;
    ASSERT_TRUE(v.as_timeonly(h, m, s, ms));
    EXPECT_EQ(h, 12);
    EXPECT_EQ(m, 30);
    EXPECT_EQ(s, 5);
    EXPECT_EQ(ms, 250);

    int ns = 0;
    ASSERT_TRUE(v.as_timeonly_nano(h, m, s, ns));
    EXPECT_EQ(ns, 250000000);
}

TEST(TypedValue, UniversalAccessorsOnEveryCategory) {
    typed_value<fix_type::string> s(make_value("ABC"));
    EXPECT_EQ(s.as_string_view(), "ABC");
    EXPECT_EQ(s.bytes().size(), 3u);
    EXPECT_FALSE(s.empty());
    EXPECT_TRUE(typed_value<fix_type::string>{}.empty());
}

TEST(TypedValue, UnknownCategoryEnablesEverything) {
    typed_value<fix_type::unknown> v(make_value("42"));
    int n = 0;
    EXPECT_TRUE(v.try_as_int(n));
    EXPECT_EQ(n, 42);
    EXPECT_EQ(v.as_string_view(), "42");
}

namespace {
template <class T, class = void>
struct detect_try_as_int : std::false_type {};

template <class T>
struct detect_try_as_int<T, std::void_t<decltype(std::declval<T&>().try_as_int(std::declval<int&>()))>>
    : std::true_type {};

template <class T, class = void>
struct detect_try_as_decimal : std::false_type {};

template <class T>
struct detect_try_as_decimal<T,
                             std::void_t<decltype(std::declval<T&>().try_as_decimal(
                                 std::declval<long&>(), std::declval<long&>()))>> : std::true_type {
};
}  // namespace

// Compile-time enforcement: disabled accessors must NOT be callable.
static_assert(!detect_try_as_int<typed_value<fix_type::string>>::value,
              "try_as_int must be disabled on a string-typed value");
static_assert(!detect_try_as_decimal<typed_value<fix_type::string>>::value,
              "try_as_decimal must be disabled on a string-typed value");
static_assert(!detect_try_as_int<typed_value<fix_type::decimal>>::value,
              "try_as_int must be disabled on a decimal-typed value");
static_assert(detect_try_as_decimal<typed_value<fix_type::decimal>>::value,
              "try_as_decimal must be enabled on a decimal-typed value");
static_assert(detect_try_as_int<typed_value<fix_type::unknown>>::value,
              "all accessors enabled on unknown");

namespace {
template <class T, class = void>
struct detect_as_date : std::false_type {};

template <class T>
struct detect_as_date<T,
                      std::void_t<decltype(std::declval<T&>().as_date(
                          std::declval<int&>(), std::declval<int&>(), std::declval<int&>()))>>
    : std::true_type {};

template <class T, class = void>
struct detect_as_timeonly : std::false_type {};

template <class T>
struct detect_as_timeonly<
    T,
    std::void_t<decltype(std::declval<T&>().as_timeonly(
        std::declval<int&>(), std::declval<int&>(), std::declval<int&>(), std::declval<int&>()))>>
    : std::true_type {};

template <class T, class = void>
struct detect_as_monthyear : std::false_type {};

template <class T>
struct detect_as_monthyear<
    T,
    std::void_t<decltype(std::declval<T&>().as_monthyear(std::declval<int&>(), std::declval<int&>()))>>
    : std::true_type {};

template <class T, class = void>
struct detect_as_timestamp : std::false_type {};

template <class T>
struct detect_as_timestamp<T,
                           std::void_t<decltype(std::declval<T&>().as_timestamp(std::declval<int&>(),
                                                                                std::declval<int&>(),
                                                                                std::declval<int&>(),
                                                                                std::declval<int&>(),
                                                                                std::declval<int&>(),
                                                                                std::declval<int&>(),
                                                                                std::declval<int&>()))>>
    : std::true_type {};

template <class T, class = void>
struct detect_try_as_timestamp : std::false_type {};

template <class T>
struct detect_try_as_timestamp<T,
                               std::void_t<decltype(std::declval<T&>().try_as_timestamp(
                                   std::declval<std::chrono::sys_time<std::chrono::nanoseconds>&>()))>>
    : std::true_type {};

template <class T, class = void>
struct detect_try_as_timeonly : std::false_type {};

template <class T>
struct detect_try_as_timeonly<
    T,
    std::void_t<decltype(std::declval<T&>().try_as_timeonly(std::declval<std::chrono::nanoseconds&>()))>>
    : std::true_type {};

template <class T, class = void>
struct detect_try_as_date : std::false_type {};

template <class T>
struct detect_try_as_date<
    T,
    std::void_t<decltype(std::declval<T&>().try_as_date(std::declval<std::chrono::year_month_day&>()))>>
    : std::true_type {};

template <class T, class = void>
struct detect_try_as_monthyear : std::false_type {};

template <class T>
struct detect_try_as_monthyear<
    T,
    std::void_t<decltype(std::declval<T&>().try_as_monthyear(std::declval<std::chrono::year_month&>()))>>
    : std::true_type {};

template <class T, class = void>
struct detect_as_char_unchecked : std::false_type {};

template <class T>
struct detect_as_char_unchecked<T, std::void_t<decltype(std::declval<T&>().as_char_unchecked())>>
    : std::true_type {};
}  // namespace

// A UTCTimeOnly field must expose the time-of-day parse and must NOT expose
// the date/epoch accessors (an 8-byte "12:30:05" passes as_date's length-only
// validation and yields garbage with `true`).
static_assert(detect_as_timeonly<typed_value<fix_type::timeonly>>::value,
              "as_timeonly must be enabled on a timeonly-typed value");
static_assert(!detect_as_date<typed_value<fix_type::timeonly>>::value,
              "as_date must be disabled on a timeonly-typed value");
static_assert(!detect_as_timeonly<typed_value<fix_type::timestamp>>::value,
              "as_timeonly must be disabled on a timestamp-typed value");
static_assert(detect_as_timeonly<typed_value<fix_type::unknown>>::value,
              "all accessors enabled on unknown");

// Date-carrying categories: LocalMktDate/UTCDateOnly fields are `date` (an
// 8-byte YYYYMMDD), MonthYear fields are `monthyear`; a full 17+ byte
// UTCTimestamp never passes as_date's length check, so as_date has no
// business on the timestamp tier.
static_assert(detect_as_date<typed_value<fix_type::date>>::value,
              "as_date must be enabled on a date-typed value");
static_assert(!detect_as_date<typed_value<fix_type::timestamp>>::value,
              "as_date must be disabled on a timestamp-typed value");
static_assert(detect_as_monthyear<typed_value<fix_type::monthyear>>::value,
              "as_monthyear must be enabled on a monthyear-typed value");
static_assert(!detect_as_monthyear<typed_value<fix_type::date>>::value,
              "as_monthyear must be disabled on a date-typed value");
static_assert(detect_as_timestamp<typed_value<fix_type::timestamp>>::value,
              "as_timestamp must be enabled on a timestamp-typed value");
static_assert(!detect_as_timestamp<typed_value<fix_type::date>>::value,
              "as_timestamp must be disabled on a date-typed value");

// Named fully-validating chrono accessors are gated per category, like their
// length-only as_* siblings.
static_assert(detect_try_as_timestamp<typed_value<fix_type::timestamp>>::value,
              "try_as_timestamp must be enabled on a timestamp-typed value");
static_assert(!detect_try_as_timestamp<typed_value<fix_type::timeonly>>::value,
              "try_as_timestamp must be disabled on a timeonly-typed value");
static_assert(detect_try_as_timeonly<typed_value<fix_type::timeonly>>::value,
              "try_as_timeonly must be enabled on a timeonly-typed value");
static_assert(!detect_try_as_timeonly<typed_value<fix_type::timestamp>>::value,
              "try_as_timeonly must be disabled on a timestamp-typed value");
static_assert(detect_try_as_date<typed_value<fix_type::date>>::value,
              "try_as_date must be enabled on a date-typed value");
static_assert(!detect_try_as_date<typed_value<fix_type::string>>::value,
              "try_as_date must be disabled on a string-typed value");
static_assert(detect_try_as_monthyear<typed_value<fix_type::monthyear>>::value,
              "try_as_monthyear must be enabled on a monthyear-typed value");
static_assert(!detect_try_as_monthyear<typed_value<fix_type::date>>::value,
              "try_as_monthyear must be disabled on a date-typed value");
static_assert(detect_try_as_timestamp<typed_value<fix_type::unknown>>::value &&
                  detect_try_as_timeonly<typed_value<fix_type::unknown>>::value &&
                  detect_try_as_date<typed_value<fix_type::unknown>>::value &&
                  detect_try_as_monthyear<typed_value<fix_type::unknown>>::value,
              "all accessors enabled on unknown");

// Design rule (CLAUDE.md): no `_unchecked` on typed_value — the ungated
// escape hatch is `.value()`.
static_assert(!detect_as_char_unchecked<typed_value<fix_type::character>>::value,
              "as_char_unchecked must not exist on typed_value");
static_assert(detect_as_char_unchecked<field_value>::value,
              "as_char_unchecked stays on the raw field_value tier");

namespace {
// Builds a NewOrderSingle with Symbol=MSFT, Price=50001, OrderQty=100 into buf;
// returns the message length.
std::size_t build_nos(std::span<char> buf) {
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_string(tag::Symbol, "MSFT");
    w.push_back_int(tag::OrderQty, 100);
    w.push_back_decimal(tag::Price, 50001, 0);
    [[maybe_unused]] bool const fit = w.push_back_trailer();
    return static_cast<std::size_t>(w.message_end() - buf.data());
}
}  // namespace

TEST(TypedFind, IteratorPathReturnsTypedValue) {
    std::array<char, 256> buf{};
    std::size_t const n = build_nos(buf);
    message_reader r(buf.data(), buf.data() + n);
    ASSERT_TRUE(r.is_complete() && r.is_valid());

    auto px = r.find(tag::Price);
    long mant = 0, exp = 0;
    ASSERT_TRUE(px.try_as_decimal(mant, exp));
    EXPECT_EQ(mant, 50001);

    EXPECT_EQ(r.find(tag::Symbol).as_string_view(), "MSFT");

    EXPECT_TRUE(r.find(tag::Account).empty());
}

TEST(TypedFind, IteratorHintedPathCarriesCursor) {
    std::array<char, 256> buf{};
    std::size_t const n = build_nos(buf);
    message_reader r(buf.data(), buf.data() + n);
    ASSERT_TRUE(r.is_complete() && r.is_valid());

    auto it = r.begin();
    auto sym = r.find_with_hint(tag::Symbol, it);
    EXPECT_EQ(sym.as_string_view(), "MSFT");

    long mant = 0, exp = 0;
    auto px = r.find_with_hint(tag::Price, it);  // continues from cursor
    ASSERT_FALSE(px.empty());
    ASSERT_TRUE(px.try_as_decimal(mant, exp));
    EXPECT_EQ(mant, 50001);
}

TEST(TypedFind, IndexedPathReturnsTypedValue) {
    std::array<char, 256> buf{};
    std::size_t const n = build_nos(buf);
    message_reader r(buf.data(), buf.data() + n);
    field_index_buffer<64> ibuf;
    indexed_fields<64> f(build_field_index(r, ibuf));

    long mant = 0, exp = 0;
    ASSERT_TRUE(f.find(tag::Price).try_as_decimal(mant, exp));
    EXPECT_EQ(mant, 50001);
}

TEST(TypedFind, IndexedMessageTypedFindAndHint) {
    std::array<char, 256> buf{};
    std::size_t const n = build_nos(buf);
    message_reader r(buf.data(), buf.data() + n);
    field_index_buffer<64> ibuf;
    indexed_message idx = build_field_index(r, ibuf);
    ASSERT_FALSE(idx.truncated());

    long mant = 0, exp = 0;
    ASSERT_TRUE(idx.find(tag::Price).try_as_decimal(mant, exp));
    EXPECT_EQ(mant, 50001);

    std::size_t hint = 0;
    EXPECT_EQ(idx.find_with_hint(tag::Symbol, hint).as_string_view(), "MSFT");
}

TEST(TypedFind, WithFieldsTieredPathReturnsTypedValue) {
    std::array<char, 256> buf{};
    std::size_t const n = build_nos(buf);
    message_reader r(buf.data(), buf.data() + n);
    field_index_buffer<64> ibuf;
    with_fields(r, ibuf, [&](auto& fields) {
        EXPECT_EQ(fields.find(tag::Symbol).as_string_view(), "MSFT");
    });
}

TEST(TypedTags, GeneratedHandlesCarryCorrectCategory) {
    static_assert(tag::Price.type == fix_type::decimal);
    static_assert(tag::Symbol.type == fix_type::string);
    static_assert(tag::OrderQty.type == fix_type::decimal);       // Qty
    static_assert(tag::MsgSeqNum.type == fix_type::integer);      // SeqNum
    static_assert(tag::MDEntryTime.type == fix_type::timeonly);   // UTCTimeOnly
    static_assert(tag::SendingTime.type == fix_type::timestamp);  // UTCTimestamp
    static_assert(tag::SettlDate.type == fix_type::date);         // LocalMktDate
    static_assert(tag::MDEntryDate.type == fix_type::date);       // UTCDateOnly
    static_assert(tag::MaturityMonthYear.type == fix_type::monthyear);
    static_assert(tag::ExecInst.type == fix_type::string);  // MultipleCharValue
    static_assert(tag::Price.tag == 44);
    SUCCEED();
}

// tag:: handles still behave as ints via operator int() (writer, comparison).
TEST(TypedTags, ImplicitIntInterop) {
    static_assert(tag::Price == 44);
    static_assert(static_cast<int>(tag::Symbol) == 55);

    std::array<char, 256> buf{};
    std::size_t const n = build_nos(buf);
    message_reader r(buf.data(), buf.data() + n);
    auto it = r.begin();
    ASSERT_TRUE(r.find_with_hint(tag::Symbol, it));  // int overload, tag -> int
    EXPECT_EQ(it->tag(), tag::Symbol);               // int == field_tag
}
