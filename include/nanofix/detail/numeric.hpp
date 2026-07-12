#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <nanofix/detail/config.hpp>

namespace nanofix {

/* @cond EXCLUDE */

namespace detail {

static_assert(std::endian::native == std::endian::little,
              "nanofix SWAR digit parsers assume little-endian byte order.");

NANOFIX_ALWAYS_INLINE bool all_digits(char const* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (static_cast<unsigned char>(p[i] - '0') > 9u)
            return false;
    return true;
}

NANOFIX_ALWAYS_INLINE std::uint32_t parse_two_digits(char const* p) {
    return std::uint32_t(p[0] - '0') * 10 + std::uint32_t(p[1] - '0');
}

NANOFIX_ALWAYS_INLINE std::uint32_t parse_four_digits(char const* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    v -= 0x30303030U;
    v = (v & 0x000f000fU) * 10 + ((v & 0x0f000f00U) >> 8);
    return (v & 0x000000ffU) * 100 + ((v & 0x00ff0000U) >> 16);
}

NANOFIX_ALWAYS_INLINE std::uint32_t parse_eight_digits(char const* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    v -= 0x3030303030303030ULL;
    v = (v & 0x000f000f000f000fULL) * 10 + ((v & 0x0f000f000f000f00ULL) >> 8);
    v = (v & 0x000000ff000000ffULL) * 100 + ((v & 0x00ff000000ff0000ULL) >> 16);
    v = (v & 0x000000000000ffffULL) * 10000 + (v >> 32);
    return static_cast<std::uint32_t>(v);
}

template <typename Int_type>
Int_type atoi_unchecked(char const* begin, char const* end) {
    using U = std::make_unsigned_t<Int_type>;
    U uval = 0;
    bool isnegative = false;

    if (begin < end && *begin == '-') {
        isnegative = true;
        ++begin;
    }

    for (; begin < end; ++begin) {
        uval = uval * 10u + static_cast<U>(static_cast<unsigned char>(*begin) - '0');
    }

    return isnegative ? static_cast<Int_type>(U{0} - uval) : static_cast<Int_type>(uval);
}

template <typename Uint_type>
inline Uint_type atou_unchecked(char const* begin, char const* end) {
    Uint_type val(0);

    for (; begin < end; ++begin) {
        val *= 10u;
        val += static_cast<Uint_type>(*begin - '0');
    }

    return val;
}

template <typename Int_type>
void atod_unchecked(char const* begin, char const* end, Int_type& mantissa, Int_type& exponent) {
    using U = std::make_unsigned_t<Int_type>;
    U m = 0;
    Int_type exponent_ = 0;
    bool isdecimal(false);
    bool isnegative(false);

    if (begin < end && *begin == '-') {
        isnegative = true;
        ++begin;
    }

    for (; begin < end; ++begin) {
        if (*begin == '.') {
            isdecimal = true;
        } else {
            m = m * 10u + static_cast<U>(static_cast<unsigned char>(*begin) - '0');
            if (isdecimal)
                --exponent_;
        }
    }

    mantissa = isnegative ? static_cast<Int_type>(U{0} - m) : static_cast<Int_type>(m);
    exponent = exponent_;
}

template <typename U>
NANOFIX_ALWAYS_INLINE bool try_accumulate_digit(U& val, char c, U limit) noexcept {
    auto const uc = static_cast<unsigned char>(c);
    if (uc < '0' || uc > '9') [[unlikely]]
        return false;
    U const d = static_cast<U>(uc - '0');
    if (val > (limit - d) / 10u) [[unlikely]]
        return false;
    val = val * 10u + d;
    return true;
}

template <typename Int_type>
[[nodiscard]] inline bool try_atoi(char const* begin, char const* end, Int_type& out) noexcept {
    static_assert(std::numeric_limits<Int_type>::is_signed,
                  "try_atoi requires a signed Int_type; use try_atou for unsigned.");
    if (begin == end)
        return false;
    bool neg = false;
    if (*begin == '-') {
        neg = true;
        ++begin;
        if (begin == end)
            return false;
    }
    using U = std::make_unsigned_t<Int_type>;
    constexpr U max_pos = static_cast<U>(std::numeric_limits<Int_type>::max());
    U const limit = neg ? static_cast<U>(max_pos + U{1}) : max_pos;
    U val = 0;
    for (; begin < end; ++begin) {
        if (!try_accumulate_digit(val, *begin, limit)) [[unlikely]]
            return false;
    }
    out = neg ? static_cast<Int_type>(U{0} - val) : static_cast<Int_type>(val);
    return true;
}

template <typename Uint_type>
[[nodiscard]] inline bool try_atou(char const* begin, char const* end, Uint_type& out) noexcept {
    static_assert(!std::numeric_limits<Uint_type>::is_signed,
                  "try_atou requires an unsigned Uint_type; use try_atoi for signed.");
    if (begin == end)
        return false;
    constexpr Uint_type max = std::numeric_limits<Uint_type>::max();
    Uint_type val = 0;
    for (; begin < end; ++begin) {
        if (!try_accumulate_digit(val, *begin, max)) [[unlikely]]
            return false;
    }
    out = val;
    return true;
}

template <typename Int_type>
[[nodiscard]] inline bool try_atod(char const* begin,
                                   char const* end,
                                   Int_type& mantissa,
                                   Int_type& exponent) noexcept {
    static_assert(std::numeric_limits<Int_type>::is_signed,
                  "try_atod requires a signed Int_type for the mantissa.");
    if (begin == end)
        return false;
    bool neg = false;
    if (*begin == '-') {
        neg = true;
        ++begin;
        if (begin == end)
            return false;
    }
    using U = std::make_unsigned_t<Int_type>;
    constexpr U max_pos = static_cast<U>(std::numeric_limits<Int_type>::max());
    U const limit = neg ? static_cast<U>(max_pos + U{1}) : max_pos;
    U m = 0;
    Int_type e = 0;
    bool seen_dot = false;
    bool seen_digit = false;
    for (; begin < end; ++begin) {
        char const c = *begin;
        if (c == '.') {
            if (seen_dot) [[unlikely]]
                return false;
            seen_dot = true;
            continue;
        }
        if (!try_accumulate_digit(m, c, limit)) [[unlikely]]
            return false;
        seen_digit = true;
        if (seen_dot) {
            // A zero-mantissa field with more fractional digits than Int_type's
            // range would otherwise underflow e (signed-overflow UB).
            if (e == std::numeric_limits<Int_type>::min()) [[unlikely]]
                return false;
            --e;
        }
    }
    if (!seen_digit) [[unlikely]]
        return false;
    mantissa = neg ? static_cast<Int_type>(U{0} - m) : static_cast<Int_type>(m);
    exponent = e;
    return true;
}

// Max ascii chars to print Int_type (digits + sign).
template <class Int_type>
inline constexpr std::ptrdiff_t max_ascii_chars = std::numeric_limits<Int_type>::digits10 + 2;

template <typename Uint_type>
NANOFIX_ALWAYS_INLINE char* utoa_unchecked(Uint_type number, char* buffer) noexcept {
    char* b = buffer;
    do {
        *b++ = static_cast<char>('0' + (number % 10));
        number /= 10;
    } while (number);
    std::reverse(buffer, b);
    return b;
}

template <typename Int_type>
NANOFIX_ALWAYS_INLINE char* itoa_unchecked(Int_type number, char* buffer) noexcept {
    using U = std::make_unsigned_t<Int_type>;
    bool const isnegative = number < 0;
    U n = isnegative ? U{0} - static_cast<U>(number) : static_cast<U>(number);
    char* b = buffer;
    do {
        *b++ = static_cast<char>('0' + (n % 10));
        n /= 10;
    } while (n);
    if (isnegative)
        *b++ = '-';
    std::reverse(buffer, b);
    return b;
}

template <typename Int_type>
NANOFIX_ALWAYS_INLINE char* dtoa_unchecked(Int_type mantissa, Int_type exponent, char* buffer) noexcept {
    using U = std::make_unsigned_t<Int_type>;
    bool const isnegative = mantissa < 0;
    U m = isnegative ? U{0} - static_cast<U>(mantissa) : static_cast<U>(mantissa);
    char* b = buffer;
    if (exponent > 0) [[unlikely]] {
        if (m != 0)
            for (; exponent > 0; --exponent)
                *b++ = '0';
        exponent = 0;
    }

    do {
        *b++ = static_cast<char>('0' + (m % 10));
        m /= 10;
        if (++exponent == 0)
            *b++ = '.';
    } while (m > 0 || exponent < 1);
    if (isnegative)
        *b++ = '-';
    std::reverse(buffer, b);
    return b;
}

NANOFIX_ALWAYS_INLINE void itoa_padded_unchecked(int x, char* b, char* e) noexcept {
    while (e > b) {
        *--e = static_cast<char>('0' + (x % 10));
        x /= 10;
    }
}

}  // namespace detail

/* @endcond*/
}  // namespace nanofix
