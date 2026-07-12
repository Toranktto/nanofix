#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <nanofix/detail/diagnostics.hpp>
#include <nanofix/detail/fields.hpp>
#include <nanofix/detail/numeric.hpp>
#include <nanofix/detail/reader.hpp>
#include <nanofix/detail/simd.hpp>

namespace nanofix {

/* @cond EXCLUDE */

namespace detail {

struct length_tag_index {
    static constexpr int kBitmapSize = 1024;
    std::uint64_t bitmap[kBitmapSize / 64]{};
    int const* overflow_begin{};
    int const* overflow_end{};

    constexpr length_tag_index() {
        constexpr auto n = sizeof(length_fields) / sizeof(length_fields[0]);
        static_assert(
            std::is_sorted(std::begin(length_fields), std::end(length_fields)),
            "length_fields must be sorted ascending; bitmap/binary-search split depends on it.");
        std::size_t split = 0;
        while (split < n && length_fields[split] < kBitmapSize)
            ++split;
        for (std::size_t i = 0; i < split; ++i) {
            int const t = length_fields[i];
            bitmap[t / 64] |= std::uint64_t(1) << (t % 64);
        }
        overflow_begin = length_fields + split;
        overflow_end = length_fields + n;
    }

    NANOFIX_ALWAYS_INLINE constexpr bool contains(int tag) const {
        if (tag >= 0 && tag < kBitmapSize) [[likely]] {
            return (bitmap[tag / 64] >> (tag % 64)) & 1u;
        }
        return std::binary_search(overflow_begin, overflow_end, tag);
    }
};

// constinit guarantees constant initialization: there is no runtime init to
// throw. The check does not model constinit, so the warning is a false positive.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
inline constinit length_tag_index const g_length_tag_table{};

NANOFIX_ALWAYS_INLINE bool is_tag_a_data_length(int tag) noexcept {
    return g_length_tag_table.contains(tag);
}

}  // namespace detail

/* @endcond */

/**
 * \brief Caller-owned backing store for `build_field_index`; `N` is the field
 * capacity.
 *
 * \warning All-or-nothing. More than `N` fields yields `truncated() == true`
 * and an empty index, not a partial one — fall back to the iterator for that
 * message. Never grows. Size `N` for the largest message a venue sends;
 * undersizing it is a tail-latency cliff (full iterator re-parse), not a bug.
 */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // tail padding from alignas is intentional (SIMD)
#endif
template <std::size_t N>
struct field_index_buffer {
    static_assert(N > 0 && N <= 16384, "field_index_buffer<N>: N must be in (0, 16384]");

    alignas(32) int tags[N];
    alignas(32) std::uint64_t pos_len[N];  // (pos << 32) | len
    static constexpr std::size_t capacity = N;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/**
 * \brief Random-access view of a parsed FIX message, built by
 * `build_field_index` into a caller-supplied `field_index_buffer<N>`.
 *
 * Parallel insertion-order arrays: `tags[i]` and packed
 * `pos_len[i] = (uint32_t pos << 32) | uint32_t len`, offsets relative
 * to `message_begin()`. When `truncated()` is true the index is empty;
 * caller must fall back to iterator-based access. This happens when the
 * message holds more than `N` fields or exceeds `kMaxIndexableMessageBytes`.
 */
class indexed_message {
public:
    static constexpr std::size_t kMaxIndexableMessageBytes =
        std::numeric_limits<std::uint32_t>::max();

    indexed_message() = default;
    indexed_message(indexed_message const&) = default;
    indexed_message& operator=(indexed_message const&) = default;

    char const* message_begin() const noexcept { return msg_begin_; }

    [[nodiscard]] std::size_t field_count() const noexcept { return tags_.size(); }

    /**
     * True if the index could not hold the whole message (buffer capacity
     * exhausted or message too large for the packed offset format). On
     * `true` the index is empty and `find_with_hint()` always returns the
     * empty `field_value`; fall back to iterator-based access.
     */
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }

    /// \warning Unchecked: `i` must be `< field_count()` (as is `value_at`).
    int tag_at(std::size_t i) const noexcept { return tags_[i]; }

    field_value value_at(std::size_t i) const noexcept {
        if (msg_begin_ == nullptr) [[unlikely]]  // empty / unbuilt index
            return {};
        std::uint64_t const pl = pos_len_[i];
        field_value v;
        v.begin_ = msg_begin_ + (pl >> 32);
        v.end_ = v.begin_ + static_cast<std::uint32_t>(pl);
        return v;
    }

    /**
     * \brief Find `tag` by index, scanning `[hint, field_count())` then wrapping
     * to `[0, hint)`. On hit sets `hint` to the found index and returns its
     * value; on miss returns an empty `field_value` and leaves `hint` unchanged
     * — except a `hint` past `field_count()`, which is reset to `0` up front.
     *
     * `hint == 0` gives first-occurrence. `hint > 0` gives the nearest
     * occurrence at-or-after `hint` — for a tag repeated across group entries
     * the result depends on `hint`. Use `has(tag)` for presence-only.
     */
    inline field_value find_with_hint(int tag, std::size_t& hint) const noexcept {
        std::size_t const n = tags_.size();
        if (hint > n)
            hint = 0;
        std::size_t const suffix_n = n - hint;
        std::size_t const i =
            ::nanofix::detail::find_tag_in_index(tags_.data() + hint, suffix_n, tag);
        if (i != suffix_n) {
            hint += i;
            return value_at(hint);
        }
        std::size_t const j = ::nanofix::detail::find_tag_in_index(tags_.data(), hint, tag);
        if (j != hint) {
            hint = j;
            return value_at(hint);
        }
        return {};
    }

    /// Typed lookup by a `tag::` handle; empty() on miss. Fresh scan (hint 0).
    template <int Tag, fix_type Type>
    [[nodiscard]] NANOFIX_ALWAYS_INLINE typed_value<Type> find(field_tag<Tag, Type>) const noexcept {
        std::size_t hint = 0;
        return typed_value<Type>{find_with_hint(Tag, hint)};
    }

    /// Typed hinted lookup; carries the caller's `hint` index (advanced as the
    /// int overload does), returns a category-restricted typed_value.
    template <int Tag, fix_type Type>
    [[nodiscard]] NANOFIX_ALWAYS_INLINE typed_value<Type> find_with_hint(
        field_tag<Tag, Type>, std::size_t& hint) const noexcept {
        return typed_value<Type>{find_with_hint(Tag, hint)};
    }

    [[nodiscard]] bool has(int tag) const noexcept {
        return ::nanofix::detail::find_tag_in_index(tags_.data(), tags_.size(), tag) != tags_.size();
    }

private:
    template <std::size_t N>
    friend indexed_message build_field_index(message_reader const&, field_index_buffer<N>&) noexcept;
    template <std::size_t N>
    friend indexed_message build_field_index(group_entry const&, field_index_buffer<N>&) noexcept;

    char const* msg_begin_ = nullptr;
    std::span<int const> tags_;
    std::span<std::uint64_t const> pos_len_;
    bool truncated_ = false;
};

// Skips the iterator's per-field current_/value_ writes; data-length and
// truncated() semantics match the iterator path.
template <std::size_t N>
NANOFIX_HOT indexed_message build_field_index(message_reader const& r,
                                              field_index_buffer<N>& idx_buffer) noexcept {
    indexed_message out;
    if (!r.is_complete() || !r.is_valid())
        return out;
    out.msg_begin_ = r.message_begin();
    if (r.message_size() > indexed_message::kMaxIndexableMessageBytes) [[unlikely]] {
        out.truncated_ = true;
        return out;
    }

    char const* const msg_begin = r.message_begin();
    auto const begin_field = *r.begin();
    char const* const stop = r.end().buffer_begin();

    // restrict: the index arrays live in the caller's buffer, never in the
    // message, but char reads may alias any type — without it the compiler
    // must assume each store clobbers the bytes the scan reads next.
    int* NANOFIX_RESTRICT const tags = idx_buffer.tags;
    std::uint64_t* NANOFIX_RESTRICT const pos_len = idx_buffer.pos_len;
    std::size_t count = 0;

    auto write_slot = [&](int tag, char const* vb, char const* ve) {
        std::uint64_t const pos = static_cast<std::uint64_t>(vb - msg_begin);
        std::uint64_t const len = static_cast<std::uint64_t>(ve - vb);
        tags[count] = tag;
        pos_len[count] = (pos << 32) | static_cast<std::uint32_t>(len);
        ++count;
    };

    // Bulk find_all_soh sweep over a fixed look-ahead window. A data-length
    // field invalidates the window: find_all_soh also flags SOH bytes inside
    // the binary span, which are not field delimiters. Output matches the
    // per-field scan exactly.
    constexpr std::size_t kSohWindow = 256;
    std::uint32_t soh_window[kSohWindow];
    char const* win_base = stop;
    std::size_t win_n = 0;
    std::size_t win_i = 0;

    auto next_soh = [&](char const* q) -> char const* {
        if (win_i == win_n) {
            win_base = q;
            win_n = detail::find_all_soh(q, stop, soh_window, kSohWindow);
            win_i = 0;
            if (win_n == 0) [[unlikely]]
                return stop;
        }
        return win_base + soh_window[win_i];
    };

    write_slot(begin_field.tag(), begin_field.value().begin(), begin_field.value().end());

    char const* p = begin_field.value().end() + 1;

    while (p < stop) {
        if (count == N) [[unlikely]] {
            out.truncated_ = true;
            return out;
        }

        int tag = 0;
        p = detail::scan_tag_digits<true>(p, stop, tag);
        if (p >= stop) [[unlikely]]
            break;

        bool const empty_value = (*p == '\x01');
        char const* const vb = empty_value ? p : p + 1;
        char const* const ve = next_soh(vb);  // empty_value: ve == vb == p

        if (!empty_value && detail::is_tag_a_data_length(tag)) [[unlikely]] {
            // Iterator surfaces only the data field, not the length tag.
            std::size_t data_len = 0;
            if (!detail::try_atou<std::size_t>(vb, ve, data_len)) [[unlikely]]
                break;
            p = ve + 1;
            int next = 0;
            p = detail::scan_tag_digits<false>(p, stop, next);
            if (p >= stop) [[unlikely]]
                break;
            ++p;
            // Mirror the iterator: the length must fit inside the body and be
            // SOH-terminated, else the offset cannot be trusted — resuming
            // mid-value would fabricate fields. `>=` keeps the probe in bounds.
            if (data_len >= static_cast<std::size_t>(stop - p) || p[data_len] != '\x01') [[unlikely]]
                break;
            char const* const dvb = p;
            char const* const dve = p + data_len;
            write_slot(next, dvb, dve);
            win_i = win_n;  // invalidate window past the binary span
            p = dve + 1;
            continue;
        }

        write_slot(tag, vb, ve);
        ++win_i;  // consume this field's terminator
        p = ve + 1;
    }

    out.tags_ = std::span<int const>(idx_buffer.tags, count);
    out.pos_len_ = std::span<std::uint64_t const>(idx_buffer.pos_len, count);
    return out;
}

/** Group-entry overload. Offsets are packed against the entry's first byte. */
template <std::size_t N>
indexed_message build_field_index(group_entry const& entry,
                                  field_index_buffer<N>& idx_buffer) noexcept {
    indexed_message out;
    auto it = entry.begin();
    auto end = entry.end();
    if (it == end)
        return out;
    out.msg_begin_ = it.buffer_begin();

    constexpr std::uint64_t kMax32 = std::numeric_limits<std::uint32_t>::max();
    // restrict: see the message overload above — index arrays never alias
    // the entry bytes the scan reads.
    int* NANOFIX_RESTRICT const tags = idx_buffer.tags;
    std::uint64_t* NANOFIX_RESTRICT const pos_len = idx_buffer.pos_len;
    std::size_t count = 0;
    for (; it != end; ++it) {
        if (count == N) [[unlikely]] {
            out.truncated_ = true;
            return out;
        }
        auto const& v = it->value();
        std::ptrdiff_t const pos = v.begin() - out.msg_begin_;
        std::ptrdiff_t const len = v.end() - v.begin();
        if (pos < 0 || static_cast<std::uint64_t>(pos) > kMax32 || len < 0 ||
            static_cast<std::uint64_t>(len) > kMax32) [[unlikely]] {
            out.truncated_ = true;
            return out;
        }
        tags[count] = it->tag();
        pos_len[count] = (static_cast<std::uint64_t>(pos) << 32) |
                         static_cast<std::uint64_t>(static_cast<std::uint32_t>(len));
        ++count;
    }
    out.tags_ = std::span<int const>(idx_buffer.tags, count);
    out.pos_len_ = std::span<std::uint64_t const>(idx_buffer.pos_len, count);
    return out;
}

/**
 * \brief SIMD-index field reader. `find(tag)` is a pure indexed lookup with a
 * reusable hint — no per-call `truncated()` branch.
 *
 * Construct from a non-truncated `build_field_index` result. A `truncated()`
 * index here means an empty one, so `find()` would always miss; use
 * `iter_fields` (or `with_fields`, which picks between the two once per
 * message) when the message may exceed the buffer's `N` fields.
 */
template <std::size_t N>
class indexed_fields {
public:
    explicit indexed_fields(indexed_message idx) noexcept : idx_(idx) {}

    /** \brief Value for `tag`, or an empty `field_value` if absent. */
    [[nodiscard]] NANOFIX_ALWAYS_INLINE field_value find(int tag) noexcept {
        return idx_.find_with_hint(tag, hint_);
    }

    /// Typed lookup by a `tag::` handle; empty() on miss.
    template <int Tag, fix_type Type>
    [[nodiscard]] NANOFIX_ALWAYS_INLINE typed_value<Type> find(field_tag<Tag, Type>) noexcept {
        return typed_value<Type>{find(Tag)};
    }

    [[nodiscard]] indexed_message const& index() const noexcept { return idx_; }

private:
    indexed_message idx_;
    std::size_t hint_ = 0;
};

/**
 * \brief Iterator field reader. `find(tag)` is a hinted forward scan with no
 * index — the fallback path when a message exceeds the index buffer. Carries a
 * reusable cursor, so reading tags in wire order stays cheap.
 */
class iter_fields {
public:
    explicit iter_fields(message_reader const& r) noexcept
        : r_(r),
          usable_(r.is_complete() && r.is_valid()),
          it_(usable_ ? r.begin() : message_reader_const_iterator{}) {}

    /** \brief Value for `tag`, or an empty `field_value` if absent. */
    [[nodiscard]] NANOFIX_ALWAYS_INLINE field_value find(int tag) noexcept {
        if (!usable_) [[unlikely]]  // incomplete/invalid reader: every lookup misses
            return {};
        message_reader_const_iterator it = it_;
        if (r_.find_with_hint(tag, it)) {
            it_ = it;
            return it->value();
        }
        return {};
    }

    /// Typed lookup by a `tag::` handle; empty() on miss.
    template <int Tag, fix_type Type>
    [[nodiscard]] NANOFIX_ALWAYS_INLINE typed_value<Type> find(field_tag<Tag, Type>) noexcept {
        return typed_value<Type>{find(Tag)};
    }

private:
    message_reader r_;
    bool usable_;
    message_reader_const_iterator it_{};
};

/**
 * \brief Index-or-iterator field reading, dispatched once per message.
 *
 * Builds the index, checks `truncated()` exactly once, then calls
 * `fn(accessor)` with a concrete `indexed_fields<N>&` (message fit the buffer)
 * or `iter_fields&` (it did not). Each accessor's `find(tag)` is dispatch-free
 * — the index↔iterator decision lives here, not in the per-lookup hot path.
 * `fn` is instantiated for both accessor types, so write it as a generic
 * lambda:
 * \code
 * nanofix::field_index_buffer<64> buf;
 * for (auto const& m : nanofix::messages(wire))
 *     nanofix::with_fields(m, buf, [&](auto& f) {
 *         long qty_m = 0, qty_e = 0;
 *         if (f.find(nanofix::tag::OrderQty).try_as_decimal(qty_m, qty_e))
 *             consume(qty_m, qty_e);                       // OrderQty is a Qty
 *         auto px = f.find(nanofix::tag::Price);           // empty() if absent
 *     });
 * \endcode
 * The buffer is caller-owned and reusable across messages. Returns whatever
 * `fn` returns (both branches must deduce the same type).
 */
template <std::size_t N, class Fn>
NANOFIX_ALWAYS_INLINE auto with_fields(message_reader const& r, field_index_buffer<N>& buf, Fn&& fn) {
    indexed_message idx = build_field_index(r, buf);
    if (!idx.truncated()) [[likely]] {
        indexed_fields<N> f(idx);
        return fn(f);
    }
    iter_fields f(r);
    return fn(f);
}

/**
 * \brief Range over consecutive FIX messages packed into a single buffer.
 *
 * Iterates contiguous messages produced by typical FIX wire delivery
 * (e.g. one `recv()` returning N concatenated frames). Yields only
 * `is_complete() && is_valid()` readers. Invalid frames between valid
 * ones are skipped via `next_message_reader()` resync. Iteration stops
 * at the first incomplete frame; the position of that frame is
 * available from `iterator::remainder()` for callers that buffer the
 * tail across reads.
 *
 * Thread-safety: the range itself holds two pointers and is trivially
 * copyable. Iterators hold a `message_reader` by value and are
 * independent across copies.
 */
class message_range {
public:
    using value_type = message_reader;

    explicit message_range(std::span<char const> buf) noexcept
        : begin_(buf.data()), end_(buf.data() + buf.size()) {}

    message_range(char const* begin, char const* end) noexcept : begin_(begin), end_(end) {}

    message_range(char const* buf, std::size_t n) noexcept : begin_(buf), end_(buf + n) {}

    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = message_reader;
        using reference = message_reader const&;
        using pointer = message_reader const*;
        using difference_type = std::ptrdiff_t;

        iterator(char const* b, char const* e) noexcept : r_(b, e) { skip_invalid(); }

        reference operator*() const noexcept { return r_; }

        pointer operator->() const noexcept { return &r_; }

        iterator& operator++() noexcept {
            NANOFIX_ASSERT(r_.is_complete(), "message_range::iterator: ++ past end of range.");
            r_ = r_.next_message_reader();
            skip_invalid();
            return *this;
        }

        iterator operator++(int) noexcept {
            iterator tmp = *this;
            ++(*this);
            return tmp;
        }

        friend bool operator==(iterator const& a, iterator const& b) noexcept {
            bool const a_end = !a.r_.is_complete();
            bool const b_end = !b.r_.is_complete();
            if (a_end && b_end)
                return true;
            if (a_end != b_end)
                return false;
            return a.r_.buffer_begin() == b.r_.buffer_begin();
        }

        friend bool operator!=(iterator const& a, iterator const& b) noexcept { return !(a == b); }

        /**
         * \brief Buffer position where iteration stopped.
         *
         * On the past-the-end iterator (`rng.end()` or one advanced
         * past the final complete frame), this points at the first
         * byte of the unconsumed tail (an incomplete frame, or the
         * buffer end). Callers using stream re-feeding copy
         * `[remainder(), buffer_end)` into the next read buffer.
         */
        char const* remainder() const noexcept { return r_.buffer_begin(); }

    private:
        void skip_invalid() noexcept {
            while (r_.is_complete() && !r_.is_valid()) {
                r_ = r_.next_message_reader();
            }
        }

        message_reader r_;
    };

    iterator begin() const noexcept { return iterator(begin_, end_); }

    iterator end() const noexcept { return iterator(end_, end_); }

private:
    char const* begin_;
    char const* end_;
};

/**
 * \brief Construct a range over consecutive FIX messages in `buf`.
 */
inline message_range messages(std::span<char const> buf) noexcept {
    return message_range(buf);
}

inline message_range messages(char const* begin, char const* end) noexcept {
    return message_range(begin, end);
}

inline message_range messages(char const* buf, std::size_t n) noexcept {
    return message_range(buf, n);
}

/**
 * \brief Callback-style batched parse. Invokes `fn` once per complete,
 * valid FIX message in `buf`.
 *
 * Equivalent to a loop over `messages(buf)` but threads the
 * tail-position return through the call, which is the common shape in
 * a feed-handler dispatch loop. Invalid frames are skipped via
 * resync; iteration stops at the first incomplete frame.
 *
 * \param buf Span over the byte range to scan.
 * \param fn Callable invoked as `fn(message_reader const&)`
 *   for each yielded message.
 * \return Pointer to the first byte of the unconsumed tail. Equals
 *   `buf.data() + buf.size()` when the buffer drained cleanly,
 *   otherwise points at an incomplete frame the caller should keep
 *   for the next read.
 */
template <class Fn>
NANOFIX_HOT char const* for_each_message(std::span<char const> buf, Fn&& fn) {
    message_reader r(buf);
    while (r.is_complete()) {
        if (r.is_valid()) {
            NANOFIX_PREFETCH(r.message_end());
            fn(static_cast<message_reader const&>(r));
        }
        r = r.next_message_reader();
    }
    return r.buffer_begin();
}

template <class Fn>
char const* for_each_message(char const* begin, char const* end, Fn&& fn) {
    return for_each_message(std::span<char const>(begin, static_cast<std::size_t>(end - begin)),
                            std::forward<Fn>(fn));
}
}  // namespace nanofix
