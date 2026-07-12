#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <ratio>
#include <type_traits>
#include <nanofix/detail/config.hpp>
#include <nanofix/detail/numeric.hpp>

namespace nanofix {

/* @cond EXCLUDE */

namespace detail {

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

NANOFIX_ALWAYS_INLINE bool is_leap_year(int year) noexcept {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

/// \pre `month` in [1, 12].
NANOFIX_ALWAYS_INLINE int days_in_month(int year, int month) noexcept {
    constexpr unsigned char kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return kDays[month - 1] + (month == 2 && is_leap_year(year));
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

// Shared tail of the two timestamp parsers: parse the YYYYMMDD prefix, gate
// the range (epoch math overflows int64 outside [1970, 2200]), assemble the
// time_point.
template <typename TimePoint, typename SubsecondDur>
NANOFIX_ALWAYS_INLINE bool timestamp_to_timepoint(char const* date_begin,
                                                  int hour,
                                                  int minute,
                                                  int second,
                                                  SubsecondDur subsecond,
                                                  TimePoint& tp) noexcept {
    int year, month, day;
    if (!atodate(date_begin, date_begin + 8, year, month, day))
        return false;
    if (year < kMinSupportedYear || year > kMaxSupportedYear || month < 1 || month > 12 ||
        day < 1 || day > days_in_month(year, month))
        return false;

    std::int64_t const days_since_epoch = days_from_civil(year, month, day);
    tp = TimePoint(std::chrono::seconds(days_since_epoch * 86400) + std::chrono::hours(hour) +
                   std::chrono::minutes(minute) + std::chrono::seconds(second) + subsecond);
    return true;
}

template <typename T>
struct is_duration : std::false_type {};

template <typename Rep, typename Period>
struct is_duration<std::chrono::duration<Rep, Period>> : std::true_type {};

// Fully validating variants backing the generic `try_as<T>` facade: digits,
// separators, and calendar/clock ranges all checked (the named `as_*`
// accessors validate length only — documented tradeoff on the hot path).

inline bool try_atodate_strict(
    char const* begin, char const* end, int& year, int& month, int& day) noexcept {
    if (end - begin != 8 || !all_digits(begin, 8))
        return false;
    int y, m, d;
    if (!atodate(begin, end, y, m, d))
        return false;
    if (m < 1 || m > 12 || d < 1 || d > days_in_month(y, m))
        return false;
    year = y;
    month = m;
    day = d;
    return true;
}

// Shared HH:MM:SS shape check for the strict time parsers; second 60 is a
// legal leap second on the FIX wire.
NANOFIX_ALWAYS_INLINE bool valid_hms_shape(char const* begin, char const* end) noexcept {
    if (end - begin < 8)
        return false;
    if (begin[2] != ':' || begin[5] != ':')
        return false;
    if (!all_digits(begin, 2) || !all_digits(begin + 3, 2) || !all_digits(begin + 6, 2))
        return false;
    return end - begin == 8 || begin[8] == '.';
}

NANOFIX_ALWAYS_INLINE bool valid_hms_range(int hour, int minute, int second) noexcept {
    return hour <= 23 && minute <= 59 && second <= 60;
}

inline bool try_atotime_strict(char const* begin,
                               char const* end,
                               int& hour,
                               int& minute,
                               int& second,
                               int& millisecond) noexcept {
    if (!valid_hms_shape(begin, end))
        return false;
    if (end - begin == 12 && !all_digits(begin + 9, 3))
        return false;
    int h, m, s, ms;
    if (!atotime(begin, end, h, m, s, ms))
        return false;
    if (!valid_hms_range(h, m, s))
        return false;
    hour = h;
    minute = m;
    second = s;
    millisecond = ms;
    return true;
}

inline bool try_atotime_nano_strict(
    char const* begin, char const* end, int& hour, int& minute, int& second, int& nanosecond) noexcept {
    if (!valid_hms_shape(begin, end))
        return false;
    int h, m, s, ns;
    if (!atotime_nano(begin, end, h, m, s, ns))  // validates fraction digits + lengths
        return false;
    if (!valid_hms_range(h, m, s))
        return false;
    hour = h;
    minute = m;
    second = s;
    nanosecond = ns;
    return true;
}

template <typename TimePoint>
    requires detail::is_time_point<TimePoint>::value
inline bool try_atotimepoint_strict(char const* begin, char const* end, TimePoint& tp) noexcept {
    if (end - begin < 9 || begin[8] != '-' || !all_digits(begin, 8))
        return false;
    int hour, minute, second, millisecond;
    if (!try_atotime_strict(begin + 9, end, hour, minute, second, millisecond))
        return false;
    return timestamp_to_timepoint(
        begin, hour, minute, second, std::chrono::milliseconds(millisecond), tp);
}

template <typename TimePoint>
    requires detail::is_time_point<TimePoint>::value
inline bool try_atotimepoint_nano_strict(char const* begin, char const* end, TimePoint& tp) noexcept {
    if (end - begin < 9 || begin[8] != '-' || !all_digits(begin, 8))
        return false;
    int hour, minute, second, nanosecond;
    if (!try_atotime_nano_strict(begin + 9, end, hour, minute, second, nanosecond))
        return false;
    return timestamp_to_timepoint(
        begin, hour, minute, second, std::chrono::nanoseconds(nanosecond), tp);
}

// Every time_point-producing parse is fully validating: a wrong-but-plausible
// epoch value (non-digit bytes decoded as numbers, hour 99 rolling into the
// next day) is worse than a reject. The length-only fast tier stays available
// through the parts-based `as_*` accessors, which never assemble an epoch.
template <typename TimePoint>
    requires detail::is_time_point<TimePoint>::value
inline bool atotimepoint(char const* begin, char const* end, TimePoint& tp) noexcept {
    return try_atotimepoint_strict(begin, end, tp);
}

template <typename TimePoint>
    requires detail::is_time_point<TimePoint>::value
inline bool atotimepoint_nano(char const* begin, char const* end, TimePoint& tp) noexcept {
    return try_atotimepoint_nano_strict(begin, end, tp);
}

// Floored divmod of an epoch count into (days, remainder-in-day). C++ `/`
// truncates toward zero, which would mis-bucket pre-epoch times by a day.
NANOFIX_ALWAYS_INLINE std::int64_t split_epoch_days(std::int64_t total,
                                                    std::int64_t per_day,
                                                    std::int64_t& in_day) noexcept {
    std::int64_t days = total / per_day;
    in_day = total - days * per_day;
    if (in_day < 0) [[unlikely]] {
        in_day += per_day;
        --days;
    }
    return days;
}

// Decompose a time_point into calendar/clock parts. Returns false when the
// input is outside what the writers can represent — the conversion to the
// target precision would multiply (source coarser than target) and a huge
// epoch count would signed-overflow (UB) before any range check could run,
// so the bound is checked in *source* units (dividing direction, safe).
template <typename TimePoint>
    requires detail::is_time_point<TimePoint>::value
[[nodiscard]] inline bool timepointtoparts(TimePoint tp,
                                           int& year,
                                           int& month,
                                           int& day,
                                           int& hour,
                                           int& minute,
                                           int& second,
                                           int& millisecond) noexcept {
    using Dur = typename TimePoint::duration;
    constexpr std::int64_t kMsPerDay = 86'400'000;
    if constexpr (std::ratio_greater_v<typename Dur::period, std::milli>) {
        // 9999-12-31 23:59:59.999, the writers' year cap, in ms.
        constexpr std::int64_t kMaxWritableMs = 253'402'300'799'999;
        auto const bound = std::chrono::duration_cast<Dur>(std::chrono::milliseconds(kMaxWritableMs));
        if (tp.time_since_epoch() > bound || tp.time_since_epoch() < -bound) [[unlikely]]
            return false;
    }
    auto const total_ms =
        std::chrono::time_point_cast<std::chrono::milliseconds>(tp).time_since_epoch().count();
    std::int64_t ms_in_day = 0;
    std::int64_t const days_since_epoch = split_epoch_days(total_ms, kMsPerDay, ms_in_day);

    civil_from_days(days_since_epoch, year, month, day);

    auto ms = static_cast<std::int32_t>(ms_in_day);
    hour = ms / 3'600'000;
    ms -= hour * 3'600'000;
    minute = ms / 60'000;
    ms -= minute * 60'000;
    second = ms / 1000;
    millisecond = ms - second * 1000;
    return true;
}

template <typename TimePoint>
    requires detail::is_time_point<TimePoint>::value
[[nodiscard]] inline bool timepointtoparts_nano(TimePoint tp,
                                                int& year,
                                                int& month,
                                                int& day,
                                                int& hour,
                                                int& minute,
                                                int& second,
                                                int& nanosecond) noexcept {
    using Dur = typename TimePoint::duration;
    constexpr std::int64_t kNsPerDay = 86'400'000'000'000LL;
    if constexpr (std::ratio_greater_v<typename Dur::period, std::nano>) {
        // int64 nanoseconds saturate around year 2262; see timepointtoparts.
        auto const bound = std::chrono::duration_cast<Dur>(
            std::chrono::nanoseconds(std::numeric_limits<std::int64_t>::max()));
        if (tp.time_since_epoch() > bound || tp.time_since_epoch() < -bound) [[unlikely]]
            return false;
    }
    auto const total_ns =
        std::chrono::time_point_cast<std::chrono::nanoseconds>(tp).time_since_epoch().count();
    std::int64_t ns_in_day = 0;
    std::int64_t const days_since_epoch = split_epoch_days(total_ns, kNsPerDay, ns_in_day);

    civil_from_days(days_since_epoch, year, month, day);

    hour = static_cast<int>(ns_in_day / 3'600'000'000'000LL);
    ns_in_day -= static_cast<std::int64_t>(hour) * 3'600'000'000'000LL;
    minute = static_cast<int>(ns_in_day / 60'000'000'000LL);
    ns_in_day -= static_cast<std::int64_t>(minute) * 60'000'000'000LL;
    second = static_cast<int>(ns_in_day / 1'000'000'000LL);
    nanosecond = static_cast<int>(ns_in_day - static_cast<std::int64_t>(second) * 1'000'000'000LL);
    return true;
}

}  // namespace detail

/* @endcond*/
}  // namespace nanofix
