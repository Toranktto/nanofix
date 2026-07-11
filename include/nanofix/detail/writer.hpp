#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>
#include <nanofix/detail/numeric.hpp>
#include <nanofix/detail/simd.hpp>

namespace nanofix {

/**
 * \brief noexcept FIX writer. Overflow sets error flag; push_back_* no-op
 * afterwards. Caller checks push_back_trailer() return.
 */
class message_writer {
public:
    explicit message_writer(std::span<char> buffer) noexcept
        : buffer_(buffer.data()), buffer_end_(buffer.data() + buffer.size()), next_(buffer.data()) {}

    message_writer(char* buffer, std::size_t size) noexcept
        : message_writer(std::span<char>(buffer, size)) {}

    message_writer(char* begin, char* end) noexcept
        : message_writer(std::span<char>(begin, static_cast<std::size_t>(end - begin))) {}

    template <std::size_t N>
    explicit message_writer(char (&buffer)[N]) noexcept
        : buffer_(buffer), buffer_end_(buffer + N), next_(buffer) {}

    [[nodiscard]] bool ok() const noexcept { return !error_; }

    bool has_error() const noexcept { return error_; }

    char* message_begin() const noexcept { return buffer_; }

    char* message_end() const noexcept { return next_; }

    std::size_t message_size() const noexcept { return static_cast<std::size_t>(next_ - buffer_); }

    std::size_t buffer_size() const noexcept {
        return static_cast<std::size_t>(buffer_end_ - buffer_);
    }

    std::size_t buffer_size_remaining() const noexcept {
        return static_cast<std::size_t>(buffer_end_ - next_);
    }

    /**
     * \brief Backfill BodyLength and append the CheckSum trailer.
     *
     * `false` (and sticky error) when no `push_back_header` preceded it, the
     * body exceeds BodyLength's 6 digits (999999), or the trailer does not
     * fit. `CalculateChecksum = false` emits a literal `10=000` stub.
     */
    template <bool CalculateChecksum = true>
    [[nodiscard]] bool push_back_trailer() noexcept {
        if (error_) [[unlikely]]
            return false;
        if (!body_length_) [[unlikely]] {
            error_ = true;
            return false;
        }
        std::size_t const len = static_cast<std::size_t>(next_ - (body_length_ + 7));
        // BodyLength is 6 digits; >999999 would wrap in itoa_padded_unchecked.
        if (len > 999999u) [[unlikely]] {
            error_ = true;
            return false;
        }
        detail::itoa_padded_unchecked(static_cast<int>(len), body_length_, body_length_ + 6);
        if (buffer_end_ - next_ < 7) [[unlikely]] {
            error_ = true;
            return false;
        }
        if constexpr (CalculateChecksum) {
            using std::uint8_t;
            uint8_t const checksum = detail::checksum_bytes(buffer_, next_);
            std::memcpy(next_, "10=", 3);
            next_ += 3;
            next_[0] = static_cast<char>('0' + ((checksum / 100) % 10));
            next_[1] = static_cast<char>('0' + ((checksum / 10) % 10));
            next_[2] = static_cast<char>('0' + (checksum % 10));
            next_ += 3;
            *next_++ = '\x01';
        } else {
            std::memcpy(next_, "10=000\x01", 7);
            next_ += 7;
        }
        return true;
    }

    void push_back_header(char const* begin, char const* end) noexcept {
        if (error_) [[unlikely]]
            return;
        if (body_length_) [[unlikely]] {
            error_ = true;
            return;
        }
        std::ptrdiff_t const vlen = end - begin;
        if (buffer_end_ - next_ < 2 + vlen + 3 + 7) [[unlikely]] {
            error_ = true;
            return;
        }
        std::memcpy(next_, "8=", 2);
        next_ += 2;
        std::memcpy(next_, begin, static_cast<std::size_t>(vlen));
        next_ += vlen;
        *next_++ = '\x01';
        std::memcpy(next_, "9=", 2);
        next_ += 2;
        body_length_ = next_;
        next_ += 6;
        *next_++ = '\x01';
    }

    /** String-literal overload; runtime char const* must be wrapped in std::string_view. */
    template <std::size_t N>
    void push_back_header(char const (&literal)[N]) noexcept {
        static_assert(N > 0);
        push_back_header(literal, literal + (N - 1));
    }

    void push_back_header(std::string_view v) noexcept {
        push_back_header(v.data(), v.data() + v.size());
    }

    void push_back_string(int tag, char const* begin, char const* end) noexcept {
        std::ptrdiff_t const slen = end - begin;
        if (!open_field(tag, slen)) [[unlikely]]
            return;
        std::memcpy(next_, begin, static_cast<std::size_t>(slen));
        next_ += slen;
        *next_++ = '\x01';
    }

    /** String-literal overload; runtime char const* must be wrapped in std::string_view. */
    template <std::size_t N>
    void push_back_string(int tag, char const (&literal)[N]) noexcept {
        static_assert(N > 0);
        push_back_string(tag, literal, literal + (N - 1));
    }

    void push_back_string(int tag, std::string_view s) noexcept {
        push_back_string(tag, s.data(), s.data() + s.size());
    }

    void push_back_char(int tag, char c) noexcept {
        if (!open_field(tag, 1)) [[unlikely]]
            return;
        *next_++ = c;
        *next_++ = '\x01';
    }

    template <class Int_type>
    void push_back_int(int tag, Int_type n) noexcept {
        if (!open_field(tag, detail::max_ascii_chars<Int_type>)) [[unlikely]]
            return;
        next_ = detail::itoa_unchecked(n, next_);
        *next_++ = '\x01';
    }

    template <class Int_type>
    void push_back_decimal(int tag, Int_type mantissa, Int_type exponent) noexcept {
        if (error_) [[unlikely]]
            return;
        std::ptrdiff_t const remaining = buffer_end_ - next_;
        // dtoa writes max(mantissa-field, 1 - exponent) digits + a dot; a deep
        // negative exponent outgrows the mantissa field. Reserve the wider
        // (|exponent| in uint64, clamped) so it errors instead of overflowing.
        std::uint64_t positions = static_cast<std::uint64_t>(detail::max_ascii_chars<Int_type>);
        if (exponent < 0) {
            std::uint64_t const mag =
                std::uint64_t{0} - static_cast<std::uint64_t>(static_cast<std::int64_t>(exponent));
            if (mag + 1u > positions)
                positions = mag + 1u;
            std::uint64_t const cap = static_cast<std::uint64_t>(remaining) + 1u;
            if (positions > cap)
                positions = cap;
        }
        std::ptrdiff_t const need =
            detail::max_ascii_chars<int> + 1 + static_cast<std::ptrdiff_t>(positions) + 1 + 1;
        if (remaining < need) [[unlikely]] {
            error_ = true;
            return;
        }
        next_ = detail::utoa_unchecked(static_cast<unsigned>(tag), next_);
        *next_++ = '=';
        next_ = detail::dtoa_unchecked(mantissa, exponent, next_);
        *next_++ = '\x01';
    }

    void push_back_date(int tag, int y, int m, int d) noexcept {
        if (!open_field(tag, 8)) [[unlikely]]
            return;
        detail::itoa_padded_unchecked(y, next_, next_ + 4);
        next_ += 4;
        detail::itoa_padded_unchecked(m, next_, next_ + 2);
        next_ += 2;
        detail::itoa_padded_unchecked(d, next_, next_ + 2);
        next_ += 2;
        *next_++ = '\x01';
    }

    void push_back_monthyear(int tag, int y, int m) noexcept {
        if (!open_field(tag, 6)) [[unlikely]]
            return;
        detail::itoa_padded_unchecked(y, next_, next_ + 4);
        next_ += 4;
        detail::itoa_padded_unchecked(m, next_, next_ + 2);
        next_ += 2;
        *next_++ = '\x01';
    }

    void push_back_timeonly(int tag, int h, int m, int s) noexcept {
        if (!open_field(tag, 8)) [[unlikely]]
            return;
        detail::itoa_padded_unchecked(h, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        detail::itoa_padded_unchecked(m, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        detail::itoa_padded_unchecked(s, next_, next_ + 2);
        next_ += 2;
        *next_++ = '\x01';
    }

    void push_back_timeonly(int tag, int h, int m, int s, int ms) noexcept {
        if (!open_field(tag, 12)) [[unlikely]]
            return;
        detail::itoa_padded_unchecked(h, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        detail::itoa_padded_unchecked(m, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        detail::itoa_padded_unchecked(s, next_, next_ + 2);
        next_ += 2;
        *next_++ = '.';
        detail::itoa_padded_unchecked(ms, next_, next_ + 3);
        next_ += 3;
        *next_++ = '\x01';
    }

    template <class Rep, class Period>
    void push_back_timeonly(int tag, std::chrono::duration<Rep, Period> t) noexcept {
        using namespace std::chrono;
        push_back_timeonly(tag,
                           static_cast<int>(duration_cast<hours>(t).count()),
                           static_cast<int>(duration_cast<minutes>(t % hours(1)).count()),
                           static_cast<int>(duration_cast<seconds>(t % minutes(1)).count()),
                           static_cast<int>(duration_cast<milliseconds>(t % seconds(1)).count()));
    }

    void push_back_timeonly_nano(int tag, int h, int m, int s, int ns) noexcept {
        if (!open_field(tag, 18)) [[unlikely]]
            return;
        detail::itoa_padded_unchecked(h, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        detail::itoa_padded_unchecked(m, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        detail::itoa_padded_unchecked(s, next_, next_ + 2);
        next_ += 2;
        *next_++ = '.';
        detail::itoa_padded_unchecked(ns, next_, next_ + 9);
        next_ += 9;
        *next_++ = '\x01';
    }

    template <class Rep, class Period>
    void push_back_timeonly_nano(int tag, std::chrono::duration<Rep, Period> t) noexcept {
        using namespace std::chrono;
        push_back_timeonly_nano(tag,
                                static_cast<int>(duration_cast<hours>(t).count()),
                                static_cast<int>(duration_cast<minutes>(t % hours(1)).count()),
                                static_cast<int>(duration_cast<seconds>(t % minutes(1)).count()),
                                static_cast<int>(duration_cast<nanoseconds>(t % seconds(1)).count()));
    }

    void push_back_timestamp(int tag, int y, int mo, int d, int h, int mi, int s) noexcept {
        if (!open_field(tag, 17)) [[unlikely]]
            return;
        detail::itoa_padded_unchecked(y, next_, next_ + 4);
        next_ += 4;
        detail::itoa_padded_unchecked(mo, next_, next_ + 2);
        next_ += 2;
        detail::itoa_padded_unchecked(d, next_, next_ + 2);
        next_ += 2;
        *next_++ = '-';
        detail::itoa_padded_unchecked(h, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        detail::itoa_padded_unchecked(mi, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        detail::itoa_padded_unchecked(s, next_, next_ + 2);
        next_ += 2;
        *next_++ = '\x01';
    }

    void push_back_timestamp(int tag, int y, int mo, int d, int h, int mi, int s, int ms) noexcept {
        if (!open_field(tag, 21)) [[unlikely]]
            return;
        detail::itoa_padded_unchecked(y, next_, next_ + 4);
        next_ += 4;
        detail::itoa_padded_unchecked(mo, next_, next_ + 2);
        next_ += 2;
        detail::itoa_padded_unchecked(d, next_, next_ + 2);
        next_ += 2;
        *next_++ = '-';
        detail::itoa_padded_unchecked(h, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        detail::itoa_padded_unchecked(mi, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        detail::itoa_padded_unchecked(s, next_, next_ + 2);
        next_ += 2;
        *next_++ = '.';
        detail::itoa_padded_unchecked(ms, next_, next_ + 3);
        next_ += 3;
        *next_++ = '\x01';
    }

    template <class Clock, class Duration>
    void push_back_timestamp(int tag, std::chrono::time_point<Clock, Duration> tp) noexcept {
        int year, month, day, hour, minute, second, millisecond;
        detail::timepointtoparts(tp, year, month, day, hour, minute, second, millisecond);
        push_back_timestamp(tag, year, month, day, hour, minute, second, millisecond);
    }

    void push_back_timestamp_nano(int tag, int y, int mo, int d, int h, int mi, int s, int ns) noexcept {
        if (!open_field(tag, 27)) [[unlikely]]
            return;
        detail::itoa_padded_unchecked(y, next_, next_ + 4);
        next_ += 4;
        detail::itoa_padded_unchecked(mo, next_, next_ + 2);
        next_ += 2;
        detail::itoa_padded_unchecked(d, next_, next_ + 2);
        next_ += 2;
        *next_++ = '-';
        detail::itoa_padded_unchecked(h, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        detail::itoa_padded_unchecked(mi, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        detail::itoa_padded_unchecked(s, next_, next_ + 2);
        next_ += 2;
        *next_++ = '.';
        detail::itoa_padded_unchecked(ns, next_, next_ + 9);
        next_ += 9;
        *next_++ = '\x01';
    }

    template <class Clock, class Duration>
    void push_back_timestamp_nano(int tag, std::chrono::time_point<Clock, Duration> tp) noexcept {
        int year, month, day, hour, minute, second, nanosecond;
        detail::timepointtoparts_nano(tp, year, month, day, hour, minute, second, nanosecond);
        push_back_timestamp_nano(tag, year, month, day, hour, minute, second, nanosecond);
    }

    /** \brief UTCTimestamp from raw signed epoch milliseconds. */
    void push_back_timestamp_epoch_millis(int tag, std::int64_t epoch_millis) noexcept {
        push_back_timestamp(tag,
                            std::chrono::sys_time<std::chrono::milliseconds>{
                                std::chrono::milliseconds{epoch_millis}});
    }

    /** \brief UTCTimestamp from raw signed epoch nanoseconds. */
    void push_back_timestamp_epoch_nanos(int tag, std::int64_t epoch_nanos) noexcept {
        push_back_timestamp_nano(
            tag,
            std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{epoch_nanos}});
    }

    /**
     * \brief Append a data-field pair: `tag_data_length=<len>` is emitted
     * automatically, then `tag_data=<bytes>` verbatim (SOH in the value is
     * legal — that is what the length field is for).
     */
    void push_back_data(int tag_data_length, int tag_data, char const* begin, char const* end) noexcept {
        if (error_) [[unlikely]]
            return;
        if (end < begin) [[unlikely]] {
            error_ = true;
            return;
        }
        std::ptrdiff_t const dlen = end - begin;
        std::ptrdiff_t const need = detail::max_ascii_chars<int> + 1 + detail::max_ascii_chars<int> +
                                    1 + detail::max_ascii_chars<int> + 1 + dlen + 1;
        if (buffer_end_ - next_ < need) [[unlikely]] {
            error_ = true;
            return;
        }
        next_ = detail::utoa_unchecked(static_cast<unsigned>(tag_data_length), next_);
        *next_++ = '=';
        next_ = detail::itoa_unchecked(static_cast<int>(dlen), next_);
        *next_++ = '\x01';
        next_ = detail::utoa_unchecked(static_cast<unsigned>(tag_data), next_);
        *next_++ = '=';
        std::memcpy(next_, begin, static_cast<std::size_t>(dlen));
        next_ += dlen;
        *next_++ = '\x01';
    }

private:
    // Reserve room for `tag=<value_len bytes>\x01` and write the `tag=` prefix,
    // leaving next_ at the value. Returns false (and sets the sticky error_) if
    // it won't fit. value_len is the caller's known max value width.
    NANOFIX_ALWAYS_INLINE bool open_field(int tag, std::ptrdiff_t value_len) noexcept {
        if (error_) [[unlikely]]
            return false;
        std::ptrdiff_t const need = detail::max_ascii_chars<int> + 1 + value_len + 1;
        if (buffer_end_ - next_ < need) [[unlikely]] {
            error_ = true;
            return false;
        }
        next_ = detail::utoa_unchecked(static_cast<unsigned>(tag), next_);
        *next_++ = '=';
        return true;
    }

    char* buffer_;
    char* buffer_end_;
    char* next_;
    char* body_length_ = nullptr;
    bool error_ = false;
};

/**
 * \brief Build one FIX message via `body(writer)` and push_back_trailer. Returns
 * `false` on any writer error — overflow, or `body` never called
 * `push_back_header`; `end_out` set to one past the last byte on success.
 */
template <class F>
[[nodiscard]] inline bool try_write_message(std::span<char> buffer, char*& end_out, F&& body) noexcept {
    message_writer w(buffer);
    std::forward<F>(body)(w);
    if (!w.push_back_trailer())
        return false;
    end_out = w.message_end();
    return true;
}

template <class F>
[[nodiscard]] inline bool try_write_message(char* begin, char* end, char*& end_out, F&& body) noexcept {
    return try_write_message(std::span<char>(begin, static_cast<std::size_t>(end - begin)),
                             end_out,
                             std::forward<F>(body));
}

class message_reader;
class message_reader_const_iterator;
class indexed_message;
class group_entry;
class group_view;
}  // namespace nanofix
