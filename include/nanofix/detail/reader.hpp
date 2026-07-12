#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <nanofix/detail/diagnostics.hpp>
#include <nanofix/detail/fields.hpp>
#include <nanofix/detail/numeric.hpp>
#include <nanofix/detail/simd.hpp>
#include <nanofix/detail/value_iter.hpp>

namespace nanofix {

/**
 * \brief Why `message_reader` framing rejected a complete-but-malformed
 * message. Meaningful only when `is_complete() && !is_valid()`; otherwise
 * `none`. Maps to the FIX SessionRejectReason a gateway would emit.
 */
enum class parse_error : std::uint8_t {
    none = 0,
    begin_string_unterminated,  // BeginString has no SOH within the bound
    body_length_tag_missing,    // BeginString not followed by tag 9
    body_length_not_numeric,    // non-digit in BodyLength
    body_length_overflow,       // BodyLength exceeds NANOFIX_MAX_BODY_LENGTH
    msg_type_tag_missing,       // BodyLength not followed by tag 35
    checksum_soh_missing,       // no SOH immediately before CheckSum
    trailer_soh_missing,        // no SOH terminating CheckSum
    msg_type_unterminated,      // MsgType value runs past the trailer
    begin_string_tag_missing,   // buffer does not start with "8="
    checksum_tag_missing,       // trailer field is not "10="
};

/**
 * \brief Immutable forward view over one FIX message. Does not modify or own
 * the buffer, which must outlive the reader.
 *
 * Construction validates only the header/trailer transport fields
 * (BeginString, BodyLength, MsgType, CheckSum), not content — O(1) for a
 * well-formed frame. `begin()` points at MsgType, `end()` at CheckSum.
 *
 * Iteration yields content fields only; framing fields are skipped:
 * BeginString, BodyLength, CheckSum, and every binary-data length tag
 * (those `detail::is_tag_a_data_length()` recognizes). A binary-data field
 * is itself a content field; its length is its `value().size()`.
 */
class message_reader {
public:
    typedef field value_type;
    typedef field const& const_reference;
    typedef message_reader_const_iterator const_iterator;
    typedef field const* const_pointer;
    typedef size_t size_type;

    explicit message_reader(std::span<char const> buffer) noexcept
        : end_(*this, nullptr),
          buffer_(buffer.data()),
          buffer_end_(buffer.data() + buffer.size()),
          is_complete_(false),
          is_valid_(true),
          begin_(*this, nullptr) {
        init();
    }

    message_reader(char const* buffer, std::size_t size) noexcept
        : message_reader(std::span<char const>(buffer, size)) {}

    message_reader(char const* begin, char const* end) noexcept
        : message_reader(std::span<char const>(begin, static_cast<std::size_t>(end - begin))) {}

    message_reader(message_reader const&) noexcept = default;
    message_reader& operator=(message_reader const&) noexcept = default;
    message_reader(message_reader&&) noexcept = default;
    message_reader& operator=(message_reader&&) noexcept = default;

    /// Read back a just-written message. Equivalent to
    /// `message_reader(w.message_begin(), w.message_end())`.
    message_reader(message_writer const& that) noexcept
        : end_(*this, nullptr),
          buffer_(that.message_begin()),
          buffer_end_(that.message_end()),
          is_complete_(false),
          is_valid_(true),
          begin_(*this, nullptr) {
        init();
    }

    /// Read from the entire array of length N.
    template <size_t N>
    message_reader(char const (&buffer)[N]) noexcept
        : end_(*this, nullptr),
          buffer_(buffer),
          buffer_end_(&(buffer[N])),
          is_complete_(false),
          is_valid_(true),
          begin_(*this, nullptr) {
        init();
    }

    /// True if the buffer holds a complete FIX message.
    [[nodiscard]] NANOFIX_ALWAYS_INLINE bool is_complete() const noexcept { return is_complete_; }

    /**
     * \brief True if the message is well-framed: first field BeginString, then
     * BodyLength, then MsgType, with a CheckSum at the offset BodyLength
     * dictates. If false the message is unintelligible and its length unknown.
     */
    [[nodiscard]] NANOFIX_ALWAYS_INLINE bool is_valid() const noexcept { return is_valid_; }

    /** \brief Why a complete message failed framing. `parse_error::none` when
     *  valid or merely incomplete; set only when `is_complete() && !is_valid()`. */
    [[nodiscard]] NANOFIX_ALWAYS_INLINE parse_error error() const noexcept { return reason_; }

    /**
     * \brief Reader for the next FIX message in the buffer.
     *
     * On a valid frame the next message begins at `message_end()`. On an invalid
     * frame, resyncs by scanning from one byte past this message's start for the
     * next "8=FIX"; with fewer than 10 bytes left it returns an at-end reader
     * rather than scan.
     *
     * \pre `is_complete()`. Fires `NANOFIX_ASSERT` otherwise.
     */
    message_reader next_message_reader() const noexcept {
        NANOFIX_ASSERT(is_complete_, "Can't call next_message_reader on an incomplete message.");

        if (!is_valid_) [[unlikely]] {  // resync by scanning for the next "8=FIX"
            // Guard against `buffer_end_ - kResyncMinBytes` forming a pointer
            // below `buffer_` (pointer arithmetic UB) on a short buffer.
            if (buffer_end_ - buffer_ < kResyncMinBytes) {
                return message_reader(buffer_end_, buffer_end_);
            }
            char const* b = buffer_ + 1;
            while (b < buffer_end_ - kResyncMinBytes) {
                if (!std::memcmp(b, "8=FIX", 5))
                    break;
                ++b;
            }
            return message_reader(b, buffer_end_);
        }

        char const* next = end_.current_.value_.end_ + 1;
        NANOFIX_PREFETCH(next + 64);
        return message_reader(next, buffer_end_);
    }

    /**
     * \brief Compute the checksum over this message. Never computed implicitly;
     * compare against the message's CheckSum field yourself (framing does not
     * validate that the CheckSum bytes are digits, hence `try_as_int`):
     * \code
     * unsigned char wire = 0;
     * if (r.check_sum()->value().try_as_int(wire) && wire == r.calculate_check_sum()) {}
     * \endcode
     * \pre `is_complete() && is_valid()`. Fires `NANOFIX_ASSERT` otherwise.
     */
    [[nodiscard]] unsigned char calculate_check_sum() const noexcept {
        NANOFIX_ASSERT(usable_, "Cannot calculate checksum for an incomplete or invalid message.");
        if (!usable_) [[unlikely]]  // end_ internals are null
            return 0;
        return detail::checksum_bytes(buffer_, end_.buffer_);
    }

    /** \name Field Access */
    //@{
    /// Iterator at MsgType; synonym `message_type()`.
    /// \pre `is_complete() && is_valid()`. Fires `NANOFIX_ASSERT` otherwise.
    NANOFIX_ALWAYS_INLINE const_iterator begin() const noexcept {
        NANOFIX_ASSERT(usable_, "Cannot return iterator for an incomplete or invalid message.");
        return begin_;
    }

    /// Iterator at CheckSum; synonym `check_sum()`.
    /// \pre `is_complete() && is_valid()`. Fires `NANOFIX_ASSERT` otherwise.
    NANOFIX_ALWAYS_INLINE const_iterator end() const noexcept {
        NANOFIX_ASSERT(usable_, "Cannot return iterator for an incomplete or invalid message.");
        return end_;
    }

    /// Synonym for `begin()`.
    /// \pre `is_complete() && is_valid()`. Fires `NANOFIX_ASSERT` otherwise.
    const_iterator message_type() const noexcept {
        NANOFIX_ASSERT(usable_, "Cannot return iterator for an incomplete or invalid message.");
        return begin_;
    }

    /// Synonym for `end()`.
    /// \pre `is_complete() && is_valid()`. Fires `NANOFIX_ASSERT` otherwise.
    const_iterator check_sum() const noexcept {
        NANOFIX_ASSERT(usable_, "Cannot return iterator for an incomplete or invalid message.");
        return end_;
    }

    /// Begin pointer of the BeginString value (e.g. "FIXT.1.1").
    /// \pre `is_complete() && is_valid()`. Fires `NANOFIX_ASSERT` otherwise.
    char const* prefix_begin() const noexcept {
        NANOFIX_ASSERT(usable_, "Cannot read BeginString prefix on incomplete or invalid message.");
        return buffer_ + 2;
    }

    /// End pointer of the BeginString value.
    /// \pre `is_complete() && is_valid()`. Fires `NANOFIX_ASSERT` otherwise.
    char const* prefix_end() const noexcept {
        NANOFIX_ASSERT(usable_, "Cannot read BeginString prefix on incomplete or invalid message.");
        return prefix_end_;
    }

    /// Length of the BeginString value (e.g. 8 for "FIXT.1.1").
    /// \pre `is_complete() && is_valid()`. Fires `NANOFIX_ASSERT` otherwise.
    size_t prefix_size() const noexcept {
        NANOFIX_ASSERT(usable_, "Cannot read BeginString prefix on incomplete or invalid message.");
        return static_cast<size_t>(prefix_end_ - buffer_ - 2);
    }

    /**
     * \brief Find `tag` starting at cursor `i`; on hit advances `i` to the found
     * field and returns true, on miss leaves `i` unchanged and returns false.
     * Reading tags in wire order through one cursor stays O(length).
     * Synonym for `nanofix::find_with_hint(begin(), end(), tag_equal(tag), i)`.
     *
     * \code
     * nanofix::message_reader::const_iterator i = reader.begin();
     *
     * int seqnum = 0;
     * if (reader.find_with_hint(nanofix::tag::MsgSeqNum, i) && i++->value().try_as_int(seqnum))
     *   consume(seqnum);
     *
     * if (reader.find_with_hint(nanofix::tag::TargetCompID, i))
     *   std::string_view targetcompid = i++->value().as_string_view();
     * \endcode
     */
    [[nodiscard]] NANOFIX_ALWAYS_INLINE bool find_with_hint(int tag,
                                                            const_iterator& i) const noexcept {
        return nanofix::find_with_hint(begin(), end(), tag_equal(tag), i);
    }

    /// Typed lookup by a `tag::` handle. Returns a typed_value whose accessors
    /// are restricted to the tag's FIX category; empty() on miss. Fresh scan
    /// from begin() — for many wire-order reads use the hinted overload.
    template <int Tag, fix_type Type>
    [[nodiscard]] NANOFIX_ALWAYS_INLINE typed_value<Type> find(field_tag<Tag, Type>) const noexcept {
        const_iterator it = begin();
        if (find_with_hint(Tag, it))
            return typed_value<Type>{it->value()};
        return typed_value<Type>{};
    }

    /// Typed hinted lookup: carries the caller's cursor `it` (advanced as the
    /// int overload does), returns a category-restricted typed_value; empty() on
    /// miss. Reading tags in wire order through one cursor stays O(length).
    template <int Tag, fix_type Type>
    [[nodiscard]] NANOFIX_ALWAYS_INLINE typed_value<Type> find_with_hint(
        field_tag<Tag, Type>, const_iterator& it) const noexcept {
        if (find_with_hint(Tag, it))
            return typed_value<Type>{it->value()};
        return typed_value<Type>{};
    }

    /**
     * \brief `group_view` over a FIX repeating group introduced by `count_tag`
     * (NoXxx). `first_tag_in_group` is the per-entry delimiter; the caller
     * supplies it from the dictionary because a group's delimiter can differ by
     * MsgType. Empty view if `count_tag` is absent, its value does not parse
     * as an int, or the group is empty. Call `group()` on a `group_entry` for
     * nested groups.
     */
    group_view group(int count_tag, int first_tag_in_group) const noexcept;

    //@}

    /** \name Buffer Access */
    //@{
    char const* buffer_begin() const noexcept { return buffer_; }

    char const* buffer_end() const noexcept { return buffer_end_; }

    size_t buffer_size() const noexcept { return buffer_end_ - buffer_; }

    /** \brief Start of the FIX message; equals `buffer_begin()` — a reader views
     *  exactly one message. */
    char const* message_begin() const noexcept { return buffer_; }

    /// Past-the-end of the message.
    /// \pre `is_complete() && is_valid()`; fires `NANOFIX_ASSERT` otherwise.
    char const* message_end() const noexcept {
        NANOFIX_ASSERT(usable_, "Cannot determine size of an incomplete or invalid message.");
        if (!usable_) [[unlikely]]  // end_ internals are null
            return buffer_;
        return end_.current_.value_.end_ + 1;
    }

    /// Message size in bytes.
    /// \pre `is_complete() && is_valid()`; fires `NANOFIX_ASSERT` otherwise.
    size_t message_size() const noexcept {
        NANOFIX_ASSERT(usable_, "Cannot determine size of an incomplete or invalid message.");
        if (!usable_) [[unlikely]]  // end_ internals are null
            return 0;
        return end_.current_.value_.end_ - buffer_ + 1;
    }

    //@}

private:
    friend class message_reader_const_iterator;

    // FIX framing limits, in bytes.
    static constexpr std::ptrdiff_t kMinBeginStringLen = 9;    // shortest "8=FIX.4.4"
    static constexpr std::ptrdiff_t kMaxBeginStringSpan = 11;  // longest BeginString before SOH
    // Overridable frame cap: a hostile BodyLength otherwise forces the caller
    // to buffer toward 1 GB before the frame classifies. Must keep the 9-digit
    // bound so b+len cannot wrap size_t.
    static constexpr std::size_t kMaxBodyLength = NANOFIX_MAX_BODY_LENGTH;
    static_assert(kMaxBodyLength <= 999'999'999,
                  "NANOFIX_MAX_BODY_LENGTH must fit BodyLength's 9-digit cap");
    static constexpr std::ptrdiff_t kChecksumFieldBytes = 7;  // "10=XXX\x01"
    static constexpr std::ptrdiff_t kResyncMinBytes =
        10;  // need more than this to scan for next "8=FIX"

    void init() noexcept {
        if (buffer_end_ - buffer_ < 2) [[unlikely]] {
            is_complete_ = false;
            return;
        }
        // Literal "8=" check: without it a garbage prefix whose SOH positions
        // happen to line up parses as a valid message, and resync never runs.
        if (buffer_[0] != '8' || buffer_[1] != '=') [[unlikely]] {
            invalid(parse_error::begin_string_tag_missing);
            return;
        }
        // Need at least kMinBeginStringLen bytes before forming the pointer
        // below; otherwise the arithmetic itself is UB per [expr.add]/4.
        if (buffer_end_ - buffer_ < kMinBeginStringLen) [[unlikely]] {
            is_complete_ = false;
            return;
        }
        // BeginString length varies ("8=FIX.4.4", "8=FIXT.1.1", ...).
        char const* b = buffer_ + kMinBeginStringLen;

        while (true) {
            if (b >= buffer_end_) [[unlikely]] {
                is_complete_ = false;
                return;
            }
            if (*b == '\x01') {
                prefix_end_ = b;
                break;
            }
            if (b - buffer_ > kMaxBeginStringSpan) [[unlikely]] {
                invalid(parse_error::begin_string_unterminated);
                return;
            }
            ++b;
        }

        // 3 bytes ("\x01 9=") also keep the b += 3 pointer formation in bounds.
        if (buffer_end_ - b < 3) [[unlikely]] {
            is_complete_ = false;
            return;
        }
        // Spec: BeginString must be followed by BodyLength (tag 9). The '='
        // rules out tags 90-99.
        if (b[1] != '9' || b[2] != '=') [[unlikely]] {
            invalid(parse_error::body_length_tag_missing);
            return;
        }
        b += 3;  // past "\x01 9="

        size_t bodylength(0);

        while (true) {
            if (b >= buffer_end_) [[unlikely]] {
                is_complete_ = false;
                return;
            }
            if (*b == '\x01')
                break;
            if (*b < '0' || *b > '9') [[unlikely]] {
                invalid(parse_error::body_length_not_numeric);
                return;
            }
            bodylength = bodylength * 10 + static_cast<size_t>(*b++ - '0');
            if (bodylength > kMaxBodyLength) [[unlikely]] {
                invalid(parse_error::body_length_overflow);
                return;
            }
        }

        ++b;
        if (buffer_end_ - b < 4) [[unlikely]] {
            is_complete_ = false;
            return;
        }

        // Spec: BodyLength must be followed by MsgType (tag 35). The '='
        // rules out tags 350-359.
        if (*b != '3' || b[1] != '5' || b[2] != '=') [[unlikely]] {
            invalid(parse_error::msg_type_tag_missing);
            return;
        }

        // Reject bodylength large enough to overflow pointer arithmetic on `b + bodylength`.
        // If bodylength would push past buffer_end_, the frame is incomplete (or truncated).
        if (bodylength > static_cast<std::size_t>(buffer_end_ - b)) [[unlikely]] {
            is_complete_ = false;
            return;
        }

        char const* checksum = b + bodylength;

        if (buffer_end_ - checksum < kChecksumFieldBytes) [[unlikely]] {
            is_complete_ = false;
            return;
        }

        // SOH before checksum bounds iteration against a malformed message.
        if (*(checksum - 1) != '\x01') [[unlikely]] {
            invalid(parse_error::checksum_soh_missing);
            return;
        }

        if (*(checksum + 6) != '\x01') [[unlikely]] {
            invalid(parse_error::trailer_soh_missing);
            return;
        }

        // Literal "10=" check: catches most BodyLength lies that still land
        // on an SOH pair, instead of reading arbitrary bytes as the CheckSum.
        if (std::memcmp(checksum, "10=", 3) != 0) [[unlikely]] {
            invalid(parse_error::checksum_tag_missing);
            return;
        }

        begin_.buffer_ = b;
        begin_.current_.tag_ = tag::MsgType;
        b += 3;
        begin_.current_.value_.begin_ = b;
        char const* msgtype_end = ::nanofix::detail::find_soh(b, checksum);
        if (msgtype_end >= checksum) [[unlikely]] {
            invalid(parse_error::msg_type_unterminated);
            return;
        }
        begin_.current_.value_.end_ = msgtype_end;

        end_.buffer_ = checksum;
        end_.current_.tag_ = tag::CheckSum;
        end_.current_.value_.begin_ = checksum + 3;
        end_.current_.value_.end_ = checksum + 6;

        char const* message_end = checksum + kChecksumFieldBytes;
        begin_.message_end_ = message_end;
        end_.message_end_ = message_end;

        is_complete_ = true;
        usable_ = true;
    }

    const_iterator end_;
    char const* buffer_;
    char const* buffer_end_;
    bool is_complete_;
    bool is_valid_;
    // is_complete_ && is_valid_, precomputed: the hot accessors (begin()/end()
    // per find) assert on one byte instead of two.
    bool usable_ = false;
    parse_error reason_ = parse_error::none;
    const_iterator begin_;
    char const* prefix_end_ = nullptr;

    void invalid(parse_error reason) noexcept {
        is_complete_ = true;  // lets next_message_reader() resync past this frame
        is_valid_ = false;
        reason_ = reason;
    }
};

/**
 * \brief One entry of a FIX repeating group. Half-open iterator range over
 * the entry's fields; use `find_with_hint`/`begin`/`end` or pass to
 * `nanofix::build_field_index`.
 */
class group_entry {
public:
    using const_iterator = message_reader_const_iterator;

    const_iterator begin() const noexcept { return begin_; }

    const_iterator end() const noexcept { return end_; }

    bool empty() const noexcept { return begin_ == end_; }

    [[nodiscard]] NANOFIX_ALWAYS_INLINE bool find_with_hint(int tag,
                                                            const_iterator& it) const noexcept {
        return nanofix::find_with_hint(begin_, end_, tag_equal(tag), it);
    }

    /// Typed lookup by a `tag::` handle; empty() on miss. Fresh scan from the
    /// entry's first field.
    template <int Tag, fix_type Type>
    [[nodiscard]] NANOFIX_ALWAYS_INLINE typed_value<Type> find(field_tag<Tag, Type>) const noexcept {
        const_iterator it = begin_;
        if (find_with_hint(Tag, it))
            return typed_value<Type>{it->value()};
        return typed_value<Type>{};
    }

    /// Typed hinted lookup within this entry; carries the caller's cursor.
    template <int Tag, fix_type Type>
    [[nodiscard]] NANOFIX_ALWAYS_INLINE typed_value<Type> find_with_hint(
        field_tag<Tag, Type>, const_iterator& it) const noexcept {
        if (find_with_hint(Tag, it))
            return typed_value<Type>{it->value()};
        return typed_value<Type>{};
    }

    /** \brief Nested group lookup within this entry. */
    group_view group(int count_tag, int first_tag_in_group) const noexcept;

private:
    friend class group_view;

    group_entry(const_iterator b, const_iterator e) noexcept : begin_(b), end_(e) {}

    const_iterator begin_;
    const_iterator end_;
};

/**
 * \brief View over a FIX repeating group. Iterates `group_entry`.
 * `size()` is the declared NoXxx count; actual iteration may be shorter
 * if the message is truncated.
 */
class group_view {
public:
    std::size_t size() const noexcept { return count_; }

    bool empty() const noexcept { return count_ == 0; }

    // No range-for: a resumable iterator state machine adds per-entry branching this avoids.
    /// Returns the number of entries visited. A return smaller than `size()`
    /// means the wire carried fewer entries than the NoXxx count declared —
    /// gateways wanting to reject on the mismatch compare the two.
    template <typename Fn>
    NANOFIX_ALWAYS_INLINE std::size_t for_each(Fn&& fn) const noexcept {
        auto it = view_begin_;
        std::size_t remaining = count_;
        while (it != view_end_ && it->tag() != delimiter_)
            ++it;
        while (remaining != 0 && it != view_end_) {
            auto entry_begin = it;
            ++it;
            while (it != view_end_ && it->tag() != delimiter_)
                ++it;
            fn(group_entry{entry_begin, it});
            --remaining;
        }
        return count_ - remaining;
    }

private:
    friend class message_reader;
    friend class group_entry;

    using const_msg_iterator = message_reader_const_iterator;

    group_view(const_msg_iterator view_begin,
               const_msg_iterator view_end,
               std::size_t count,
               int delimiter) noexcept
        : view_begin_(view_begin), view_end_(view_end), count_(count), delimiter_(delimiter) {}

    static group_view empty_view(const_msg_iterator end_it, int delimiter) noexcept {
        return group_view(end_it, end_it, 0, delimiter);
    }

    const_msg_iterator view_begin_;
    const_msg_iterator view_end_;
    std::size_t count_;
    int delimiter_;
};

NANOFIX_ALWAYS_INLINE group_view message_reader::group(int count_tag,
                                                       int first_tag_in_group) const noexcept {
    auto it = begin();
    if (!nanofix::find_with_hint(begin(), end(), tag_equal(count_tag), it)) {
        return group_view::empty_view(end(), first_tag_in_group);
    }
    std::size_t count;
    if (!it->value().try_as_int<std::size_t>(count)) {
        return group_view::empty_view(end(), first_tag_in_group);
    }
    ++it;
    return group_view(it, end(), count, first_tag_in_group);
}

NANOFIX_ALWAYS_INLINE group_view group_entry::group(int count_tag,
                                                    int first_tag_in_group) const noexcept {
    auto it = begin_;
    if (!nanofix::find_with_hint(begin_, end_, tag_equal(count_tag), it)) {
        return group_view::empty_view(end_, first_tag_in_group);
    }
    std::size_t count;
    if (!it->value().try_as_int<std::size_t>(count)) {
        return group_view::empty_view(end_, first_tag_in_group);
    }
    ++it;
    return group_view(it, end_, count, first_tag_in_group);
}

/** @cond EXCLUDE */
namespace detail {
bool is_tag_a_data_length(int tag) noexcept;
}

/** @endcond */

NANOFIX_ALWAYS_INLINE void message_reader_const_iterator::increment() noexcept {
    // Mirror message_reader::end_ so `it != r.end()` terminates.
    auto const set_at_end = [this]() noexcept {
        buffer_ = message_end_ - 7;
        current_.tag_ = tag::CheckSum;
        current_.value_.begin_ = message_end_ - 4;
        current_.value_.end_ = message_end_ - 1;
    };

    char const* p = current_.value_.end_ + 1;
    buffer_ = p;

    // Opaque tag accumulation, no overflow guard by design: wrap is
    // well-defined (unsigned, then C++20 modulo signed conversion), and every
    // tag-indexed consumer (is_tag_a_data_length range-guards, is_known_tag
    // rejects unknowns) rejects a garbage tag — never UB/OOB. A per-digit
    // guard would cost the hot loop for no safety gain.
    int tag = 0;
    p = detail::scan_tag_digits<true>(p, message_end_, tag);
    if (p >= message_end_) [[unlikely]] {
        set_at_end();
        return;
    }

    if (*p == '\x01') [[unlikely]] {
        current_.tag_ = tag;
        current_.value_.begin_ = p;
        current_.value_.end_ = p;
        return;
    }

    ++p;
    char const* value_begin = p;
    char const* value_end = ::nanofix::detail::find_soh(p, message_end_);

    if (detail::is_tag_a_data_length(tag)) [[unlikely]] {
        std::size_t data_len = 0;
        if (!detail::try_atou<std::size_t>(value_begin, value_end, data_len)) [[unlikely]] {
            // Garbage data-length: can't trust the offset to the next field.
            set_at_end();
            return;
        }

        p = value_end + 1;
        buffer_ = p;
        int next_tag = 0;
        p = detail::scan_tag_digits<false>(p, message_end_, next_tag);
        if (p >= message_end_) [[unlikely]] {
            set_at_end();
            return;
        }
        ++p;
        // Length must fit inside the body (not run into the trailer) and be
        // SOH-terminated, else we can't trust it to find the next field. The
        // signed `room` also keeps the p[data_len] probe in bounds.
        std::ptrdiff_t const room = (message_end_ - message_reader::kChecksumFieldBytes) - p;
        if (room <= 0 || data_len >= static_cast<std::size_t>(room) || p[data_len] != '\x01')
            [[unlikely]] {
            set_at_end();
            return;
        }

        current_.tag_ = next_tag;
        current_.value_.begin_ = p;
        current_.value_.end_ = p + data_len;
        return;
    }

    current_.tag_ = tag;
    current_.value_.begin_ = value_begin;
    current_.value_.end_ = value_end;
}
}  // namespace nanofix
