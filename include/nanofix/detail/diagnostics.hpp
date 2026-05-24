#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <nanofix/detail/config.hpp>

/**
\brief Namespace for all nanofix types and functions.
*/
namespace nanofix {

namespace detail {
[[nodiscard]] inline std::atomic<std::uint64_t>& assert_failure_counter() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

using assert_handler_t = void (*)(char const* msg) noexcept;

[[nodiscard]] inline std::atomic<assert_handler_t>& assert_handler_slot() noexcept {
    static std::atomic<assert_handler_t> handler{nullptr};
    return handler;
}

inline void assert_fail([[maybe_unused]] char const* msg) noexcept {
    assert_failure_counter().fetch_add(1, std::memory_order_relaxed);
    if (auto* h = assert_handler_slot().load(std::memory_order_acquire))
        h(msg);
#ifdef NANOFIX_ASSERT_FAILFAST
    // abort(), not __builtin_trap(): SIGABRT is caught by every sanitizer and
    // libFuzzer; __builtin_trap raises SIGTRAP, which they ignore by default —
    // a fuzzer would then miss the assertion failure entirely.
    std::abort();
#endif
}
}  // namespace detail

/** \brief Monotonic count of `NANOFIX_ASSERT` failures. */
[[nodiscard]] inline std::uint64_t assert_failure_count() noexcept {
    return detail::assert_failure_counter().load(std::memory_order_relaxed);
}

inline void reset_assert_failure_count() noexcept {
    detail::assert_failure_counter().store(0, std::memory_order_relaxed);
}

/** \brief Install a handler called on every `NANOFIX_ASSERT` failure (log, raise
 *  an alert, abort — caller's choice). Pass `nullptr` to clear. Build with
 *  `-DNANOFIX_ASSERT_FAILFAST` to also trap after the handler. The handler runs
 *  on the cold failure path only. */
inline void set_assert_handler(detail::assert_handler_t handler) noexcept {
    detail::assert_handler_slot().store(handler, std::memory_order_release);
}

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
}  // namespace nanofix
