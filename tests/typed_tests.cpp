// Category gating on typed_value, and typed find()/find_with_hint across the
// access paths.

#include <gtest/gtest.h>

#include <nanofix.hpp>
#include <nanofix/detail/fields.hpp>

#include <array>
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
    EXPECT_EQ(v.as_char_unchecked(), '1');
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
    static_assert(tag::OrderQty.type == fix_type::decimal);   // Qty
    static_assert(tag::MsgSeqNum.type == fix_type::integer);  // SeqNum
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
