#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <ratio>
#include <span>
#include <string_view>
#include <utility>
#include <nanofix/detail/diagnostics.hpp>
#include <nanofix/detail/fields.hpp>
#include <nanofix/detail/numeric.hpp>
#include <nanofix/detail/time.hpp>
#include <nanofix/detail/typed.hpp>
#include <nanofix/detail/writer.hpp>

namespace nanofix {

/**
 * \brief Carrier for a decimal parsed as `mantissa * 10^exponent`.
 *
 * A plain aggregate, not an arithmetic type — no operators, no scaling. It
 * lets a decimal ride the single-value `try_as` / `as_unchecked` generic.
 *
 * \tparam Int_type Signed integer type for both fields (default `int64_t`).
 */
template <typename Int_type = std::int64_t>
struct decimal_parts {
    Int_type mantissa;
    Int_type exponent;
};

namespace detail {
template <typename>
struct is_decimal_parts : std::false_type {};

template <typename Int_type>
struct is_decimal_parts<decimal_parts<Int_type>> : std::true_type {};

NANOFIX_ALWAYS_INLINE char const* find_soh(char const* begin, char const* end) noexcept {
    while (begin < end && *begin != '\x01')
        ++begin;
    return begin;
}

template <bool StopOnSoh>
NANOFIX_ALWAYS_INLINE char const* scan_tag_digits(char const* p,
                                                  char const* end,
                                                  int& out_tag) noexcept {
    unsigned utag = 0;
    while (p < end && *p != '=' && (!StopOnSoh || *p != '\x01')) {
        utag = utag * 10u + static_cast<unsigned>(*p - '0');
        ++p;
    }
    out_tag = static_cast<int>(utag);
    return p;
}
}  // namespace detail

/**
 * \brief FIX field value for nanofix::message_reader: a `begin(),end()` byte
 * range into the reader's buffer delimiting one field's value.
 *
 * The validating `try_as_*` family is the default deserialization surface;
 * `as_*_unchecked` variants trade validation for speed on pre-validated
 * input. For anything else, read the raw bytes and convert yourself.
 */
class field_value {
public:
    constexpr field_value() noexcept = default;

    constexpr field_value(char const* b, char const* e) noexcept : begin_(b), end_(e) {}

    char const* begin() const noexcept { return begin_; }

    char const* end() const noexcept { return end_; }

    /** \brief Size of the field value, in bytes. */
    size_t size() const noexcept { return end_ - begin_; }

    /** \brief The field value as a byte span (may contain embedded SOH/NUL).
     *  `begin()`/`end()` give the same bytes for iteration. */
    [[nodiscard]] std::span<char const> bytes() const noexcept { return {begin_, size()}; }

    /** \brief True when the value spans no bytes — what a missed lookup
     *  (`find` / `find_with_hint`) returns. */
    [[nodiscard]] bool empty() const noexcept { return begin_ == end_; }

    /** \brief `*this` if non-empty, else `f()` (returns `field_value`). `f`
     *  runs only on a miss, so a hit is one predicted branch. Combines
     *  fallback lookups left to right:
     *  \code
     *  auto px = primary.or_else([&]{ return idx.find_with_hint(54, h); })
     *                   .or_else([&]{ return iter_fields(r).find(54); });
     *  \endcode */
    template <class F>
    [[nodiscard]] NANOFIX_ALWAYS_INLINE field_value or_else(F&& f) const {
        return empty() ? std::forward<F>(f)() : *this;
    }

    // Stops at cstring's NUL, so a shorter literal is never read past its end;
    // a field with an embedded NUL never matches a C string.
    inline friend bool operator==(field_value const& that, char const* cstring) noexcept {
        auto const sz = that.size();
        for (std::size_t i = 0; i < sz; ++i)
            if (cstring[i] == '\0' || cstring[i] != that.begin()[i])
                return false;
        return cstring[sz] == '\0';
    }

    inline friend bool operator==(char const* cstring, field_value const& that) noexcept {
        return that == cstring;
    }

    inline friend bool operator!=(field_value const& that, char const* cstring) noexcept {
        return !(that == cstring);
    }

    inline friend bool operator!=(char const* cstring, field_value const& that) noexcept {
        return !(that == cstring);
    }

    inline friend bool operator==(field_value const& that, std::string_view s) noexcept {
        return that.size() == s.size() && std::memcmp(that.begin(), s.data(), s.size()) == 0;
    }

    inline friend bool operator==(std::string_view s, field_value const& that) noexcept {
        return that == s;
    }

    inline friend bool operator!=(field_value const& that, std::string_view s) noexcept {
        return !(that == s);
    }

    inline friend bool operator!=(std::string_view s, field_value const& that) noexcept {
        return !(that == s);
    }

    /** \name String Conversion Methods */
    //@{

    [[nodiscard]] std::string_view as_string_view() const noexcept {
        return std::string_view(begin(), size());
    }

    /**
     * \brief First byte of the field value. Reads `*begin()` unconditionally.
     *
     * \warning Undefined behavior when `size() == 0`. FIX char-type
     * fields are always one byte by spec, so on conforming input from
     * a validated reader this cannot fire; untrusted callers must
     * check `size() > 0` first.
     */
    [[nodiscard]] char as_char_unchecked() const noexcept { return *begin(); }

    /**
     * \brief Validating single-character read. A FIX `char` field is exactly
     * one byte, so any length other than one (including empty) is rejected.
     * Out-param set only on success.
     * \return True on success, false otherwise.
     */
    [[nodiscard]] bool try_as_char(char& out) const noexcept {
        if (size() != 1) [[unlikely]]
            return false;
        out = *begin();
        return true;
    }

    //@}

    /** \name Boolean Conversion Methods */
    //@{

    /**
     * \brief Validating FIX Boolean: `'Y'` -> true, `'N'` -> false. Rejects
     * any other byte, an empty value, and lengths other than one. Out-param
     * set only on success.
     * \return True on success, false otherwise.
     */
    [[nodiscard]] bool try_as_bool(bool& out) const noexcept {
        if (size() != 1) [[unlikely]]
            return false;
        char const c = *begin();
        if (c == 'Y') {
            out = true;
            return true;
        }
        if (c == 'N') {
            out = false;
            return true;
        }
        return false;
    }

    /**
     * \brief Non-validating FIX Boolean: first byte `== 'Y'`. Trusted-input
     * only — any non-`'Y'` byte (including `'N'`) reads as false, and an empty
     * value is undefined behavior (reads `*begin()`).
     */
    [[nodiscard]] bool as_bool_unchecked() const noexcept { return as_char_unchecked() == 'Y'; }

    //@}

    /** \name Decimal Float Conversion Methods */
    //@{

    /**
     * \brief Non-validating ascii-to-decimal. Decimal float as
     * `mantissa * 10^exponent`, non-normalized, `exponent <= 0`.
     *
     * \warning **Trusted-input only.** Behavior is undefined for any of
     * the following on the input range `[begin(), end())`:
     *
     * - any byte outside `'0'..'9'` other than a single leading `-` or a
     *   single `.`,
     * - more than one `.`,
     * - an empty value (zero bytes),
     * - a digit count large enough that `Int_type` overflows; no
     *   overflow detection is performed and the result silently wraps.
     *
     * Wire-untrusted values must use `try_as_decimal` instead. This
     * function exists for the hot path after the caller has externally
     * validated the byte range (e.g. from a fixed-format venue feed with
     * schema-checked fields).
     *
     * \tparam Int_type Signed integer type for mantissa and exponent.
     * \param[out] mantissa Integer mantissa.
     * \param[out] exponent Decimal exponent, `<= 0`.
     */
    template <typename Int_type>
    void as_decimal_unchecked(Int_type& mantissa, Int_type& exponent) const noexcept {
        detail::atod_unchecked<Int_type>(begin(), end(), mantissa, exponent);
    }

    /**
     * \brief Validating ascii-to-decimal conversion. Rejects empty value,
     * multiple dots, stray '+', non-digit chars, and mantissa overflow.
     * Sets out-params only on success.
     * \return True on success, false otherwise.
     */
    template <typename Int_type>
    [[nodiscard]] bool try_as_decimal(Int_type& mantissa, Int_type& exponent) const noexcept {
        return detail::try_atod<Int_type>(begin(), end(), mantissa, exponent);
    }

    //@}

    /** \name Integer Conversion Methods */
    //@{

    /**
     * \brief Non-validating ascii-to-integer.
     *
     * \warning **Trusted-input only.** Behavior is undefined for any of
     * the following on the input range `[begin(), end())`:
     *
     * - any byte outside `'0'..'9'` other than a single leading `-` on a
     *   signed `Int_type`,
     * - a stray `+` (no positive sign permitted),
     * - an empty value (zero bytes),
     * - a digit count large enough to overflow `Int_type`; no overflow
     *   detection is performed and the result silently wraps.
     *
     * Wire-untrusted values must use `try_as_int` instead. This function
     * exists for the hot path after the caller has externally validated
     * the byte range. Marked `NANOFIX_HOT` for consumer inner-loop reads
     * of externally validated fields (e.g. `MsgSeqNum`).
     *
     * \tparam Int_type Signed or unsigned integer type.
     * \return The parsed value. Undefined for inputs that violate the
     * preconditions above.
     */
    template <typename Int_type>
    [[nodiscard]] NANOFIX_HOT Int_type as_int_unchecked() const noexcept {
        if constexpr (std::numeric_limits<Int_type>::is_signed)
            return detail::atoi_unchecked<Int_type>(begin(), end());
        else
            return detail::atou_unchecked<Int_type>(begin(), end());
    }

    /**
     * \brief Validating ascii-to-integer conversion.
     *
     * Rejects an empty value, a stray '+', any non-digit other than a
     * leading '-' on a signed Int_type, and overflow of Int_type's range.
     *
     * \tparam Int_type Signed or unsigned integer type.
     * \param[out] out Set only if the parse succeeded.
     * \return True on success, false otherwise.
     */
    template <typename Int_type>
    [[nodiscard]] NANOFIX_ALWAYS_INLINE bool try_as_int(Int_type& out) const noexcept {
        if constexpr (std::numeric_limits<Int_type>::is_signed)
            return detail::try_atoi<Int_type>(begin(), end(), out);
        else
            return detail::try_atou<Int_type>(begin(), end(), out);
    }

    //@}

    /** \name Generic Conversion Methods */
    //@{

    /**
     * \brief Validating conversion dispatched on the out-param type — a facade
     * over the named `try_as_*`. `T` is `decimal_parts<Int>`, `bool`, `char`,
     * `std::string_view`, an integral, or a chrono type:
     * `std::chrono::time_point` (UTCTimestamp), `std::chrono::duration`
     * (UTCTimeOnly), `std::chrono::year_month_day` (LocalMktDate/UTCDateOnly),
     * `std::chrono::year_month` (MonthYear). Out-param set only on success.
     *
     * Chrono types route to the fully validating `try_as_timestamp` /
     * `try_as_timeonly` / `try_as_date` / `try_as_monthyear` (see those for
     * the precision policy). For `string_view`, a missed lookup
     * (the null `field_value` sentinel) returns false; a present-but-empty
     * value returns true with an empty view.
     * \return True on success, false otherwise.
     */
    template <typename T>
    [[nodiscard]] NANOFIX_ALWAYS_INLINE bool try_as(T& out) const noexcept {
        if constexpr (detail::is_decimal_parts<T>::value) {
            return try_as_decimal(out.mantissa, out.exponent);
        } else if constexpr (std::is_same_v<T, bool>) {
            return try_as_bool(out);
        } else if constexpr (std::is_same_v<T, std::string_view>) {
            if (begin_ == nullptr) [[unlikely]]  // missed lookup, not an empty value
                return false;
            out = as_string_view();
            return true;
        } else if constexpr (std::is_same_v<T, char>) {
            return try_as_char(out);
        } else if constexpr (std::is_integral_v<T>) {
            return try_as_int(out);
        } else if constexpr (detail::is_time_point<T>::value) {
            if constexpr (std::ratio_less_v<typename T::duration::period, std::milli>)
                return detail::try_atotimepoint_nano_strict(begin(), end(), out);
            else
                return detail::try_atotimepoint_strict(begin(), end(), out);
        } else if constexpr (detail::is_duration<T>::value) {
            int h, m, sec, frac;
            if constexpr (std::ratio_less_v<typename T::period, std::milli>) {
                if (!detail::try_atotime_nano_strict(begin(), end(), h, m, sec, frac))
                    return false;
                out = std::chrono::hours(h) + std::chrono::minutes(m) + std::chrono::seconds(sec) +
                      std::chrono::nanoseconds(frac);
            } else {
                if (!detail::try_atotime_strict(begin(), end(), h, m, sec, frac))
                    return false;
                out = std::chrono::hours(h) + std::chrono::minutes(m) + std::chrono::seconds(sec) +
                      std::chrono::milliseconds(frac);
            }
            return true;
        } else if constexpr (std::is_same_v<T, std::chrono::year_month_day>) {
            int y, m, d;
            if (!detail::try_atodate_strict(begin(), end(), y, m, d))
                return false;
            out = std::chrono::year_month_day{std::chrono::year{y},
                                              std::chrono::month{static_cast<unsigned>(m)},
                                              std::chrono::day{static_cast<unsigned>(d)}};
            return true;
        } else if constexpr (std::is_same_v<T, std::chrono::year_month>) {
            if (size() != 6 || !detail::all_digits(begin(), 6))
                return false;
            int const y = detail::atoi_unchecked<int>(begin(), begin() + 4);
            int const m = detail::atoi_unchecked<int>(begin() + 4, begin() + 6);
            if (m < 1 || m > 12)
                return false;
            out = std::chrono::year_month{std::chrono::year{y},
                                          std::chrono::month{static_cast<unsigned>(m)}};
            return true;
        } else {
            static_assert(detail::is_decimal_parts<T>::value,
                          "try_as<T>: unsupported type; T must be decimal_parts<Int>, bool, an "
                          "integral, char, std::string_view, or a chrono time_point / duration / "
                          "year_month_day / year_month");
            return false;
        }
    }

    /**
     * \brief Non-validating counterpart of `try_as`, returned by value. Same
     * `T` set; trusted-input only — undefined output on malformed bytes, as
     * the `as_*_unchecked` methods it wraps. The chrono conversions keep the
     * named `as_*` accessors' validation (length; plus the [1970, 2200] year
     * gate for time_points) and return an epoch/zero placeholder on failure.
     */
    template <typename T>
    [[nodiscard]] NANOFIX_HOT T as_unchecked() const noexcept {
        if constexpr (detail::is_decimal_parts<T>::value) {
            T d{};
            as_decimal_unchecked(d.mantissa, d.exponent);
            return d;
        } else if constexpr (std::is_same_v<T, bool>) {
            return as_bool_unchecked();
        } else if constexpr (std::is_same_v<T, std::string_view>) {
            return as_string_view();
        } else if constexpr (std::is_same_v<T, char>) {
            return as_char_unchecked();
        } else if constexpr (std::is_integral_v<T>) {
            return as_int_unchecked<T>();
        } else if constexpr (detail::is_time_point<T>::value) {
            T tp{};
            if constexpr (std::ratio_less_v<typename T::duration::period, std::milli>)
                (void)as_timestamp_nano(tp);
            else
                (void)as_timestamp(tp);
            return tp;
        } else if constexpr (detail::is_duration<T>::value) {
            T dur{};
            if constexpr (std::ratio_less_v<typename T::period, std::milli>)
                (void)as_timeonly_nano(dur);
            else
                (void)as_timeonly(dur);
            return dur;
        } else if constexpr (std::is_same_v<T, std::chrono::year_month_day>) {
            int y{}, m{1}, d{1};
            (void)as_date(y, m, d);
            return std::chrono::year_month_day{std::chrono::year{y},
                                               std::chrono::month{static_cast<unsigned>(m)},
                                               std::chrono::day{static_cast<unsigned>(d)}};
        } else if constexpr (std::is_same_v<T, std::chrono::year_month>) {
            int y{}, m{1};
            (void)as_monthyear(y, m);
            return std::chrono::year_month{std::chrono::year{y},
                                           std::chrono::month{static_cast<unsigned>(m)}};
        } else {
            static_assert(detail::is_decimal_parts<T>::value,
                          "as_unchecked<T>: unsupported type; T must be decimal_parts<Int>, bool, "
                          "an integral, char, std::string_view, or a chrono time_point / duration "
                          "/ year_month_day / year_month");
        }
    }

    //@}

    /** \name Date and Time Conversion Methods */
    //@{

    /**
     * \brief Parse a LocalMktDate or UTCDate `YYYYMMDD` field.
     *
     * Validates length only (8 bytes); the digits parse unchecked, so
     * non-digit content yields undefined values with `true` still returned.
     * Range-check the out-params when the source is untrusted.
     *
     * \return `true` only if `size() == 8`; out-params unmodified on `false`.
     */
    [[nodiscard]] bool as_date(int& year, int& month, int& day) const noexcept {
        return detail::atodate(begin(), end(), year, month, day);
    }

    /**
     * \brief Parse a MonthYear `YYYYMM` field.
     *
     * Validates length only (6 bytes). The 6 bytes are then parsed with
     * the non-validating `atoi_unchecked`, so non-digit content yields undefined
     * output but cannot read past the field. Callers receiving values
     * from untrusted sources should additionally range-check
     * `month ∈ [1, 12]` and `year` against a venue-plausible band after
     * this returns `true`.
     *
     * \return `true` only if `size() == 6`; out-params unmodified on `false`.
     */
    [[nodiscard]] bool as_monthyear(int& year, int& month) const noexcept {
        if (end() - begin() != 6)
            return false;

        year = detail::atoi_unchecked<int>(begin(), begin() + 4);
        month = detail::atoi_unchecked<int>(begin() + 4, begin() + 6);

        return true;
    }

    /**
     * \brief Parse a UTCTimeOnly `HH:MM:SS[.sss]` field.
     *
     * Validates length only (8 or 12 bytes); separators and digits parse
     * unchecked — see `as_date` for the untrusted-input consequence.
     */
    [[nodiscard]] bool as_timeonly(int& hour, int& minute, int& second, int& millisecond) const noexcept {
        return detail::atotime(begin(), end(), hour, minute, second, millisecond);
    }

    /**
     * \brief Parse a UTCTimeOnly field with nanosecond precision
     * (`HH:MM:SS[.sss|.ssssss|.sssssssss]`).
     *
     * Fractional digits are validated (non-digits reject); the `HH:MM:SS`
     * part parses unchecked like `as_timeonly`.
     */
    [[nodiscard]] bool as_timeonly_nano(int& hour,
                                        int& minute,
                                        int& second,
                                        int& nanosecond) const noexcept {
        return detail::atotime_nano(begin(), end(), hour, minute, second, nanosecond);
    }

    /**
     * \brief Parse a UTCTimestamp `YYYYMMDD-HH:MM:SS[.sss]` field.
     * Validation strength as in `as_date` + `as_timeonly` (length checks
     * only; digits unchecked).
     */
    [[nodiscard]] bool as_timestamp(int& year,
                                    int& month,
                                    int& day,
                                    int& hour,
                                    int& minute,
                                    int& second,
                                    int& millisecond) const noexcept {
        if (end() - begin() < 9)
            return false;
        return detail::atotime(begin() + 9, end(), hour, minute, second, millisecond) &&
               detail::atodate(begin(), begin() + 8, year, month, day);
    }

    /**
     * \brief Parse a UTCTimestamp field with nanosecond precision.
     * Validation strength as in `as_date` + `as_timeonly_nano`.
     */
    [[nodiscard]] bool as_timestamp_nano(int& year,
                                         int& month,
                                         int& day,
                                         int& hour,
                                         int& minute,
                                         int& second,
                                         int& nanosecond) const noexcept {
        if (end() - begin() < 9)
            return false;
        return detail::atotime_nano(begin() + 9, end(), hour, minute, second, nanosecond) &&
               detail::atodate(begin(), begin() + 8, year, month, day);
    }

    //@}

    /** \name Fully Validating Chrono Conversion Methods
     *  Unlike the length-only `as_*` accessors above, these check digits,
     *  separators, and calendar/clock ranges (leap-second `:60` accepted) —
     *  the `try_*` contract, for chrono out-params. `try_as<T>` dispatches
     *  here for chrono `T`. */
    //@{

    /**
     * \brief Fully validating UTCTimestamp parse into a
     * `std::chrono::time_point`. A sub-millisecond-precision `Duration` reads
     * the `.ssssss`/`.sssssssss` wire formats; millisecond-or-coarser rejects
     * sub-ms wire rather than silently truncating. Years outside [1970, 2200]
     * reject (epoch math would overflow int64).
     */
    template <typename Clock, typename Duration>
    [[nodiscard]] bool try_as_timestamp(std::chrono::time_point<Clock, Duration>& tp) const noexcept {
        if constexpr (std::ratio_less_v<typename Duration::period, std::milli>)
            return detail::try_atotimepoint_nano_strict(begin(), end(), tp);
        else
            return detail::try_atotimepoint_strict(begin(), end(), tp);
    }

    /** \brief Fully validating UTCTimeOnly parse into a
     *  `std::chrono::duration`; precision policy as in `try_as_timestamp`. */
    template <typename Rep, typename Period>
    [[nodiscard]] bool try_as_timeonly(std::chrono::duration<Rep, Period>& dur) const noexcept {
        int h, m, sec, frac;
        if constexpr (std::ratio_less_v<Period, std::milli>) {
            if (!detail::try_atotime_nano_strict(begin(), end(), h, m, sec, frac))
                return false;
            dur = std::chrono::hours(h) + std::chrono::minutes(m) + std::chrono::seconds(sec) +
                  std::chrono::nanoseconds(frac);
        } else {
            if (!detail::try_atotime_strict(begin(), end(), h, m, sec, frac))
                return false;
            dur = std::chrono::hours(h) + std::chrono::minutes(m) + std::chrono::seconds(sec) +
                  std::chrono::milliseconds(frac);
        }
        return true;
    }

    /** \brief Fully validating LocalMktDate/UTCDateOnly parse (8 digits,
     *  calendar range) into a `std::chrono::year_month_day`. */
    [[nodiscard]] bool try_as_date(std::chrono::year_month_day& out) const noexcept {
        int y, m, d;
        if (!detail::try_atodate_strict(begin(), end(), y, m, d))
            return false;
        out = std::chrono::year_month_day{std::chrono::year{y},
                                          std::chrono::month{static_cast<unsigned>(m)},
                                          std::chrono::day{static_cast<unsigned>(d)}};
        return true;
    }

    /** \brief Fully validating MonthYear parse (6 digits, month range) into a
     *  `std::chrono::year_month`. */
    [[nodiscard]] bool try_as_monthyear(std::chrono::year_month& out) const noexcept {
        if (size() != 6 || !detail::all_digits(begin(), 6))
            return false;
        int const y = detail::atoi_unchecked<int>(begin(), begin() + 4);
        int const m = detail::atoi_unchecked<int>(begin() + 4, begin() + 6);
        if (m < 1 || m > 12)
            return false;
        out = std::chrono::year_month{std::chrono::year{y},
                                      std::chrono::month{static_cast<unsigned>(m)}};
        return true;
    }

    //@}

    /** \brief UTCTimestamp as signed epoch nanoseconds, or nullopt. Years
     *  outside [1970, 2200] yield nullopt (epoch math would overflow int64). */
    [[nodiscard]] std::optional<std::int64_t> as_epoch_nanos() const noexcept {
        std::chrono::sys_time<std::chrono::nanoseconds> tp;
        if (!detail::atotimepoint_nano(begin(), end(), tp))
            return std::nullopt;
        return tp.time_since_epoch().count();
    }

    /**
     * \brief UTCTimestamp as signed epoch milliseconds, or nullopt.
     * Millisecond wire precision at most: a `.ssssss`/`.sssssssss` timestamp
     * returns nullopt rather than silently truncating — use
     * `as_epoch_nanos` for those. Years outside [1970, 2200] yield nullopt
     * (epoch math would overflow int64).
     */
    [[nodiscard]] std::optional<std::int64_t> as_epoch_millis() const noexcept {
        std::chrono::sys_time<std::chrono::milliseconds> tp;
        if (!detail::atotimepoint(begin(), end(), tp))
            return std::nullopt;
        return tp.time_since_epoch().count();
    }

    /** \name std::chrono Date and Time Conversion Methods */
    //@{

    /**
     * \brief Parse a UTCTimestamp field into a `std::chrono::time_point`.
     * Years outside [1970, 2200] return false (epoch math would overflow
     * int64). Uses Howard Hinnant's proleptic Gregorian algorithms (see
     * `http://howardhinnant.github.io/date_algorithms.html`).
     */
    template <typename Clock, typename Duration>
    [[nodiscard]] bool as_timestamp(std::chrono::time_point<Clock, Duration>& tp) const {
        return detail::atotimepoint(begin(), end(), tp);
    }

    /**
     * \brief Parse a UTCTimestamp field with nanosecond precision into a
     * `std::chrono::time_point`. Year range as in the millisecond overload.
     */
    template <typename Clock, typename Duration>
    [[nodiscard]] bool as_timestamp_nano(std::chrono::time_point<Clock, Duration>& tp) const {
        return detail::atotimepoint_nano(begin(), end(), tp);
    }

    /**
     * \brief Parse a UTCTimeOnly field as a `std::chrono::duration`.
     */
    template <typename Rep, typename Period>
    [[nodiscard]] bool as_timeonly(std::chrono::duration<Rep, Period>& dur) const {
        int hour, minute, second, millisecond;
        if (!as_timeonly(hour, minute, second, millisecond))
            return false;
        dur = std::chrono::hours(hour) + std::chrono::minutes(minute) +
              std::chrono::seconds(second) + std::chrono::milliseconds(millisecond);
        return true;
    }

    /**
     * \brief Parse a UTCTimeOnly field with nanosecond precision as a
     * `std::chrono::duration`.
     */
    template <typename Rep, typename Period>
    [[nodiscard]] bool as_timeonly_nano(std::chrono::duration<Rep, Period>& dur) const {
        int hour, minute, second, nanosecond;
        if (!as_timeonly_nano(hour, minute, second, nanosecond))
            return false;
        dur = std::chrono::hours(hour) + std::chrono::minutes(minute) +
              std::chrono::seconds(second) + std::chrono::nanoseconds(nanosecond);
        return true;
    }

    //@}

private:
    friend class field;
    friend class message_reader_const_iterator;
    friend class message_reader;
    friend class indexed_message;
    char const* begin_ = nullptr;
    char const* end_ = nullptr;
};

/// A field_value whose checked accessors are restricted to its FIX category.
/// `bytes()`, `as_string_view()`, `empty()`, `value()` are universal (callable
/// for any category); `try_as_int` / `try_as_decimal` / `try_as_char` /
/// `try_as_bool` / date-time accessors compile only for the matching category
/// (or `unknown`). No `_unchecked` methods here: for the ungated, any-tag
/// unchecked path go through `value()` (`v.value().as_int_unchecked<T>()`).
/// Same size as field_value (one member, two pointers) — zero runtime
/// overhead.
template <fix_type Type>
class typed_value {
public:
    constexpr typed_value() noexcept = default;

    constexpr explicit typed_value(field_value v) noexcept : v_(v) {}

    [[nodiscard]] field_value value() const noexcept { return v_; }

    [[nodiscard]] std::span<char const> bytes() const noexcept { return v_.bytes(); }

    [[nodiscard]] std::string_view as_string_view() const noexcept { return v_.as_string_view(); }

    [[nodiscard]] bool empty() const noexcept { return v_.empty(); }

    /// True when the lookup hit (non-empty) — `if (r.find(tag::Price)) …`.
    [[nodiscard]] explicit operator bool() const noexcept { return !v_.empty(); }

    template <typename Int_type>
    [[nodiscard]] NANOFIX_ALWAYS_INLINE bool try_as_int(Int_type& out) const noexcept
        requires(Type == fix_type::integer || Type == fix_type::unknown)
    {
        return v_.try_as_int(out);
    }

    template <typename Int_type>
    [[nodiscard]] NANOFIX_ALWAYS_INLINE bool try_as_decimal(Int_type& mantissa,
                                                            Int_type& exponent) const noexcept
        requires(Type == fix_type::decimal || Type == fix_type::unknown)
    {
        return v_.try_as_decimal(mantissa, exponent);
    }

    [[nodiscard]] bool try_as_char(char& out) const noexcept
        requires(Type == fix_type::character || Type == fix_type::unknown)
    {
        return v_.try_as_char(out);
    }

    [[nodiscard]] bool try_as_bool(bool& out) const noexcept
        requires(Type == fix_type::character || Type == fix_type::unknown)
    {
        return v_.try_as_bool(out);
    }

    [[nodiscard]] bool as_date(int& year, int& month, int& day) const noexcept
        requires(Type == fix_type::date || Type == fix_type::unknown)
    {
        return v_.as_date(year, month, day);
    }

    [[nodiscard]] bool as_monthyear(int& year, int& month) const noexcept
        requires(Type == fix_type::monthyear || Type == fix_type::unknown)
    {
        return v_.as_monthyear(year, month);
    }

    [[nodiscard]] bool as_timestamp(int& year,
                                    int& month,
                                    int& day,
                                    int& hour,
                                    int& minute,
                                    int& second,
                                    int& millisecond) const noexcept
        requires(Type == fix_type::timestamp || Type == fix_type::unknown)
    {
        return v_.as_timestamp(year, month, day, hour, minute, second, millisecond);
    }

    [[nodiscard]] bool as_timestamp_nano(
        int& year, int& month, int& day, int& hour, int& minute, int& second, int& nanosecond) const noexcept
        requires(Type == fix_type::timestamp || Type == fix_type::unknown)
    {
        return v_.as_timestamp_nano(year, month, day, hour, minute, second, nanosecond);
    }

    template <typename Clock, typename Duration>
    [[nodiscard]] bool as_timestamp(std::chrono::time_point<Clock, Duration>& tp) const noexcept
        requires(Type == fix_type::timestamp || Type == fix_type::unknown)
    {
        return v_.as_timestamp(tp);
    }

    template <typename Clock, typename Duration>
    [[nodiscard]] bool as_timestamp_nano(std::chrono::time_point<Clock, Duration>& tp) const noexcept
        requires(Type == fix_type::timestamp || Type == fix_type::unknown)
    {
        return v_.as_timestamp_nano(tp);
    }

    [[nodiscard]] std::optional<std::int64_t> as_epoch_millis() const noexcept
        requires(Type == fix_type::timestamp || Type == fix_type::unknown)
    {
        return v_.as_epoch_millis();
    }

    [[nodiscard]] std::optional<std::int64_t> as_epoch_nanos() const noexcept
        requires(Type == fix_type::timestamp || Type == fix_type::unknown)
    {
        return v_.as_epoch_nanos();
    }

    [[nodiscard]] bool as_timeonly(int& hour, int& minute, int& second, int& millisecond) const noexcept
        requires(Type == fix_type::timeonly || Type == fix_type::unknown)
    {
        return v_.as_timeonly(hour, minute, second, millisecond);
    }

    [[nodiscard]] bool as_timeonly_nano(int& hour, int& minute, int& second, int& nanosecond) const noexcept
        requires(Type == fix_type::timeonly || Type == fix_type::unknown)
    {
        return v_.as_timeonly_nano(hour, minute, second, nanosecond);
    }

    template <typename Rep, typename Period>
    [[nodiscard]] bool as_timeonly(std::chrono::duration<Rep, Period>& dur) const
        requires(Type == fix_type::timeonly || Type == fix_type::unknown)
    {
        return v_.as_timeonly(dur);
    }

    template <typename Rep, typename Period>
    [[nodiscard]] bool as_timeonly_nano(std::chrono::duration<Rep, Period>& dur) const
        requires(Type == fix_type::timeonly || Type == fix_type::unknown)
    {
        return v_.as_timeonly_nano(dur);
    }

    template <typename Clock, typename Duration>
    [[nodiscard]] bool try_as_timestamp(std::chrono::time_point<Clock, Duration>& tp) const noexcept
        requires(Type == fix_type::timestamp || Type == fix_type::unknown)
    {
        return v_.try_as_timestamp(tp);
    }

    template <typename Rep, typename Period>
    [[nodiscard]] bool try_as_timeonly(std::chrono::duration<Rep, Period>& dur) const noexcept
        requires(Type == fix_type::timeonly || Type == fix_type::unknown)
    {
        return v_.try_as_timeonly(dur);
    }

    [[nodiscard]] bool try_as_date(std::chrono::year_month_day& out) const noexcept
        requires(Type == fix_type::date || Type == fix_type::unknown)
    {
        return v_.try_as_date(out);
    }

    [[nodiscard]] bool try_as_monthyear(std::chrono::year_month& out) const noexcept
        requires(Type == fix_type::monthyear || Type == fix_type::unknown)
    {
        return v_.try_as_monthyear(out);
    }

private:
    field_value v_{};
};

/**
 * \brief A FIX field for nanofix::message_reader, with tag and nanofix::field_value.
 */
class field {
public:
    int tag() const noexcept { return tag_; }

    field_value const& value() const noexcept { return value_; }

private:
    friend class message_reader_const_iterator;
    friend class message_reader;
    int tag_ = 0;
    field_value value_;
};

/**
 * \brief The iterator type for nanofix::message_reader.
 *
 * Satisfies the const Forward Iterator concept (multipass) for an immutable
 * nanofix::message_reader container of fields.
 */
class message_reader_const_iterator {
public:
    /**
     * \brief Default-constructs an invalid iterator. Not dereferenceable.
     */
    message_reader_const_iterator() = default;

private:
    message_reader_const_iterator(message_reader const&, char const* buffer) noexcept
        : buffer_(buffer), message_end_(nullptr), current_() {}

public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = field;
    using difference_type = std::ptrdiff_t;
    using pointer = field const*;
    using reference = field const&;

    field const& operator*() const noexcept { return current_; }

    field const* operator->() const noexcept { return &current_; }

    /**
     * \brief Pointer to the first byte of the current field on the wire.
     */
    char const* buffer_begin() const noexcept { return buffer_; }

    friend bool operator==(message_reader_const_iterator const& a,
                           message_reader_const_iterator const& b) noexcept {
        return a.buffer_ == b.buffer_;
    }

    friend bool operator!=(message_reader_const_iterator const& a,
                           message_reader_const_iterator const& b) noexcept {
        return a.buffer_ != b.buffer_;
    }

    friend bool operator<(message_reader_const_iterator const& a,
                          message_reader_const_iterator const& b) noexcept {
        return a.buffer_ < b.buffer_;
    }

    friend bool operator>(message_reader_const_iterator const& a,
                          message_reader_const_iterator const& b) noexcept {
        return a.buffer_ > b.buffer_;
    }

    friend bool operator<=(message_reader_const_iterator const& a,
                           message_reader_const_iterator const& b) noexcept {
        return a.buffer_ <= b.buffer_;
    }

    friend bool operator>=(message_reader_const_iterator const& a,
                           message_reader_const_iterator const& b) noexcept {
        return a.buffer_ >= b.buffer_;
    }

    message_reader_const_iterator operator++(int) noexcept {
        message_reader_const_iterator i(*this);
        ++(*this);
        return i;
    }

    message_reader_const_iterator& operator++() noexcept {
        increment();
        return *this;
    }

    /**
     * \brief Advance by `addend` fields.
     * \pre `addend >= 0`. Fires `NANOFIX_ASSERT` otherwise.
     */
    friend message_reader_const_iterator operator+(message_reader_const_iterator a,
                                                   int addend) noexcept {
        NANOFIX_ASSERT(addend >= 0,
                       "message_reader::const_iterator is a Forward Iterator, so only "
                       "positive addends are allowed.");
        for (int i = 0; i < addend; ++i)
            ++a;

        return a;
    }

    friend message_reader_const_iterator operator+(int addend,
                                                   message_reader_const_iterator a) noexcept {
        return a + addend;
    }

private:
    friend class message_reader;
    char const* buffer_ = nullptr;
    char const* message_end_ = nullptr;
    field current_;

    void increment() noexcept;
};

struct tag_equal {
    explicit tag_equal(int t) noexcept : tag(t) {}

    int tag;

    bool operator()(field const& v) const noexcept { return v.tag() == tag; }
};

/**
 * \brief An algorithm similar to `std::find_if` for forward-searching over a range and finding items which match a predicate.
 *
 * Instead of searching from `begin` to `end`, searches from `i` to `end`, then searches from `begin` to `i`.
 * Efficient for finding multiple items when the expected ordering of the items is known.
 *
 * This expression:
 * \code
 * find_with_hint(begin, end, predicate, i)
 * \endcode
 * will behave exactly the same as this expression:
 * \code
 * end != (i = std::find_if(begin, end, predicate))
 * \endcode
 * except for these two differences:
 * * In the first expression, `i` is not modified if no item is found.
 * * The first expression is faster if the found item is a near successor of `i`.
 *
 * Example usage:
 * \code
 * nanofix::message_reader::const_iterator i = reader.begin();
 *
 * int seqnum = 0;
 * if (nanofix::find_with_hint(reader.begin(), reader.end(), nanofix::tag_equal(nanofix::tag::MsgSeqNum), i)
 *     && i++->value().try_as_int(seqnum))
 *   consume(seqnum);
 *
 * if (nanofix::find_with_hint(reader.begin(), reader.end(), nanofix::tag_equal(nanofix::tag::TargetCompID), i))
 *   std::string_view targetcompid = i++->value().as_string_view();
 * \endcode
 *
 * See also the convenience method nanofix::message_reader::find_with_hint.
 *
 * \param begin The beginning of the range to search.
 * \param end The end of the range to search.
 * \param predicate A predicate which provides function `bool operator() (ForwardIterator::value_type const &v) const`.
 * \param i If an item is found which satisfies `predicate`, then `i` is modified to point to the found item. Else `i` is unmodified.
 * \return True if an item was found which matched `predicate`, and `i` was modified to point to the found item.
 *
 * \note A past-the-end hint costs a full scan, not an early `false`.
 */
template <typename ForwardIterator, typename UnaryPredicate>
[[nodiscard]] NANOFIX_ALWAYS_INLINE bool find_with_hint(ForwardIterator begin,
                                                        ForwardIterator end,
                                                        UnaryPredicate predicate,
                                                        ForwardIterator& i) {
    ForwardIterator j = std::find_if(i, end, predicate);
    if (j != end) {
        i = j;
        return true;
    }
    j = std::find_if(begin, i, predicate);
    if (j != i) {
        i = j;
        return true;
    }
    return false;
}
}  // namespace nanofix
