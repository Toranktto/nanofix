#pragma once

#include <algorithm>
#include <chrono>
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
Int_type atoi(char const* begin, char const* end) {
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
inline Uint_type atou(char const* begin, char const* end) {
    Uint_type val(0);

    for (; begin < end; ++begin) {
        val *= 10u;
        val += static_cast<Uint_type>(*begin - '0');
    }

    return val;
}

template <typename Int_type>
void atod(char const* begin, char const* end, Int_type& mantissa, Int_type& exponent) {
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
        auto const c = static_cast<unsigned char>(*begin);
        if (c < '0' || c > '9') [[unlikely]]
            return false;
        U const d = static_cast<U>(c - '0');
        if (val > (limit - d) / 10u) [[unlikely]]
            return false;
        val = val * 10u + d;
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
        auto const c = static_cast<unsigned char>(*begin);
        if (c < '0' || c > '9') [[unlikely]]
            return false;
        Uint_type const d = static_cast<Uint_type>(c - '0');
        if (val > (max - d) / 10u) [[unlikely]]
            return false;
        val = val * 10u + d;
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
        auto const uc = static_cast<unsigned char>(c);
        if (uc < '0' || uc > '9') [[unlikely]]
            return false;
        seen_digit = true;
        U const d = static_cast<U>(uc - '0');
        if (m > (limit - d) / 10u) [[unlikely]]
            return false;
        m = m * 10u + d;
        if (seen_dot)
            --e;
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

inline bool atodate(char const* begin, char const* end, int& year, int& month, int& day) noexcept {
    if (end - begin != 8)
        return false;
    std::uint32_t yyyymmdd = detail::parse_eight_digits(begin);
    year = static_cast<int>(yyyymmdd / 10000U);
    month = static_cast<int>((yyyymmdd / 100U) % 100U);
    day = static_cast<int>(yyyymmdd % 100U);
    return true;
}

// ms-only; sub-ms wire goes through atotime_nano (truncating here would hide loss).
inline bool atotime(char const* begin,
                    char const* end,
                    int& hour,
                    int& minute,
                    int& second,
                    int& millisecond) noexcept {
    if (end - begin != 8 && end - begin != 12)
        return false;
    hour = static_cast<int>(detail::parse_two_digits(begin));
    minute = static_cast<int>(detail::parse_two_digits(begin + 3));
    second = static_cast<int>(detail::parse_two_digits(begin + 6));
    if (end - begin == 12) {
        millisecond = static_cast<int>(detail::parse_two_digits(begin + 9)) * 10 +
                      static_cast<int>(begin[11] - '0');
    } else {
        millisecond = 0;
    }

    return true;
}

inline bool atotime_nano(
    char const* begin, char const* end, int& hour, int& minute, int& second, int& nanosecond) noexcept {
    if (end - begin < 8)
        return false;

    hour = static_cast<int>(detail::parse_two_digits(begin));
    minute = static_cast<int>(detail::parse_two_digits(begin + 3));
    second = static_cast<int>(detail::parse_two_digits(begin + 6));

    // Validate fractional digits before the SWAR parsers run: they are
    // unchecked, and non-digit bytes produce out-of-range values whose scale
    // (* 1e6 / * 1e3) overflows int. Reject like atotime rejects bad lengths.
    switch (end - begin) {
        case 8:  // no subsecond
            nanosecond = 0;
            break;
        case 12:  // .sss
            if (!detail::all_digits(begin + 9, 3)) [[unlikely]]
                return false;
            nanosecond = static_cast<int>(detail::parse_two_digits(begin + 9) * 10U +
                                          std::uint32_t(begin[11] - '0')) *
                         1000000;
            break;
        case 15:  // .ssssss
            if (!detail::all_digits(begin + 9, 6)) [[unlikely]]
                return false;
            nanosecond = static_cast<int>(detail::parse_four_digits(begin + 9) * 100U +
                                          detail::parse_two_digits(begin + 13)) *
                         1000;
            break;
        case 18:  // .sssssssss
            if (!detail::all_digits(begin + 9, 9)) [[unlikely]]
                return false;
            nanosecond = static_cast<int>(detail::parse_eight_digits(begin + 9) * 10U +
                                          std::uint32_t(begin[17] - '0'));
            break;
        default:
            return false;
    }
    return true;
}

template <typename T>
struct is_time_point : std::false_type {};

template <typename Clock, typename Duration>
struct is_time_point<std::chrono::time_point<Clock, Duration>> : std::true_type {};

// days_since_epoch * 86400 * 1e9 overflows int64 around year 2262; cap at
// 2200 to keep margin below the exact edge.
inline constexpr int kMinSupportedYear = 1970;
inline constexpr int kMaxSupportedYear = 2200;

// Days between 1970-01-01 and the given civil date, and its inverse.
// from http://howardhinnant.github.io/date_algorithms.html
NANOFIX_ALWAYS_INLINE std::int64_t days_from_civil(int year, int month, int day) noexcept {
    year -= month <= 2;
    unsigned const era = static_cast<unsigned>(year) / 400u;
    unsigned const yoe = static_cast<unsigned>(year) - era * 400u;
    unsigned const doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    unsigned const doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

NANOFIX_ALWAYS_INLINE void civil_from_days(std::int64_t days_since_epoch,
                                           int& year,
                                           int& month,
                                           int& day) noexcept {
    days_since_epoch += 719468;
    unsigned const era = static_cast<unsigned>(
        (days_since_epoch >= 0 ? days_since_epoch : days_since_epoch - 146096) / 146097);
    unsigned const doe =
        static_cast<unsigned>(days_since_epoch - static_cast<std::int64_t>(era) * 146097);
    unsigned const yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int const y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    unsigned const doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned const mp = (5 * doy + 2) / 153;
    day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    month = static_cast<int>(mp + (mp < 10 ? 3 : -9));
    year = y + (month <= 2 ? 1 : 0);
}

template <typename TimePoint>
    requires detail::is_time_point<TimePoint>::value
inline bool atotimepoint(char const* begin, char const* end, TimePoint& tp) noexcept {
    if (end - begin < 9)
        return false;
    int year, month, day, hour, minute, second, millisecond;
    if (!atotime(begin + 9, end, hour, minute, second, millisecond))
        return false;
    if (!atodate(begin, begin + 8, year, month, day))
        return false;
    if (year < kMinSupportedYear || year > kMaxSupportedYear || month < 1 || month > 12 ||
        day < 1 || day > 31)
        return false;

    std::int64_t const days_since_epoch = days_from_civil(year, month, day);
    tp = TimePoint(std::chrono::seconds(days_since_epoch * 86400) + std::chrono::hours(hour) +
                   std::chrono::minutes(minute) + std::chrono::seconds(second) +
                   std::chrono::milliseconds(millisecond));

    return true;
}

template <typename TimePoint>
    requires detail::is_time_point<TimePoint>::value
inline bool atotimepoint_nano(char const* begin, char const* end, TimePoint& tp) noexcept {
    if (end - begin < 9)
        return false;
    int year, month, day, hour, minute, second, nanosecond;
    if (!atotime_nano(begin + 9, end, hour, minute, second, nanosecond))
        return false;
    if (!atodate(begin, begin + 8, year, month, day))
        return false;
    if (year < kMinSupportedYear || year > kMaxSupportedYear || month < 1 || month > 12 ||
        day < 1 || day > 31)
        return false;

    std::int64_t const days_since_epoch = days_from_civil(year, month, day);
    tp = TimePoint(std::chrono::seconds(days_since_epoch * 86400) + std::chrono::hours(hour) +
                   std::chrono::minutes(minute) + std::chrono::seconds(second) +
                   std::chrono::nanoseconds(nanosecond));

    return true;
}

template <typename TimePoint>
    requires detail::is_time_point<TimePoint>::value
inline void timepointtoparts(TimePoint tp,
                             int& year,
                             int& month,
                             int& day,
                             int& hour,
                             int& minute,
                             int& second,
                             int& millisecond) noexcept {
    constexpr std::int64_t kMsPerDay = 86'400'000;
    auto const total_ms =
        std::chrono::time_point_cast<std::chrono::milliseconds>(tp).time_since_epoch().count();
    // Floored divmod: C++ `/` truncates to zero, mis-buckets pre-epoch by a day.
    std::int64_t days_since_epoch = total_ms / kMsPerDay;
    std::int64_t ms_in_day = total_ms - days_since_epoch * kMsPerDay;
    if (ms_in_day < 0) [[unlikely]] {
        ms_in_day += kMsPerDay;
        --days_since_epoch;
    }

    civil_from_days(days_since_epoch, year, month, day);

    auto ms = static_cast<std::int32_t>(ms_in_day);
    hour = ms / 3'600'000;
    ms -= hour * 3'600'000;
    minute = ms / 60'000;
    ms -= minute * 60'000;
    second = ms / 1000;
    millisecond = ms - second * 1000;
}

template <typename TimePoint>
    requires detail::is_time_point<TimePoint>::value
inline void timepointtoparts_nano(TimePoint tp,
                                  int& year,
                                  int& month,
                                  int& day,
                                  int& hour,
                                  int& minute,
                                  int& second,
                                  int& nanosecond) noexcept {
    constexpr std::int64_t kNsPerDay = 86'400'000'000'000LL;
    auto const total_ns =
        std::chrono::time_point_cast<std::chrono::nanoseconds>(tp).time_since_epoch().count();
    std::int64_t days_since_epoch = total_ns / kNsPerDay;
    std::int64_t ns_in_day = total_ns - days_since_epoch * kNsPerDay;
    if (ns_in_day < 0) [[unlikely]] {
        ns_in_day += kNsPerDay;
        --days_since_epoch;
    }

    civil_from_days(days_since_epoch, year, month, day);

    hour = static_cast<int>(ns_in_day / 3'600'000'000'000LL);
    ns_in_day -= static_cast<std::int64_t>(hour) * 3'600'000'000'000LL;
    minute = static_cast<int>(ns_in_day / 60'000'000'000LL);
    ns_in_day -= static_cast<std::int64_t>(minute) * 60'000'000'000LL;
    second = static_cast<int>(ns_in_day / 1'000'000'000LL);
    nanosecond = static_cast<int>(ns_in_day - static_cast<std::int64_t>(second) * 1'000'000'000LL);
}

}  // namespace detail

/* @endcond*/
}  // namespace nanofix
