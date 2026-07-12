#pragma once

#include <atomic>
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

/** \brief Count of `NANOFIX_ASSERT` failures since start (or last reset).
 *
 * \warning The counter and handler slot are `inline` function-local statics:
 * under hidden visibility each DSO gets its own — poll and install per DSO. */
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
}  // namespace nanofix
