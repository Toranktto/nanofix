#pragma once

#if defined(__GNUC__) || defined(__clang__)
#define NANOFIX_ALWAYS_INLINE inline __attribute__((always_inline))
#define NANOFIX_PREFETCH(p) __builtin_prefetch((p))
#define NANOFIX_HOT __attribute__((hot))
#define NANOFIX_RESTRICT __restrict__
#elif defined(_MSC_VER)
#define NANOFIX_ALWAYS_INLINE __forceinline
#define NANOFIX_PREFETCH(p) ((void)0)
#define NANOFIX_HOT
#define NANOFIX_RESTRICT __restrict
#else
#define NANOFIX_ALWAYS_INLINE inline
#define NANOFIX_PREFETCH(p) ((void)0)
#define NANOFIX_HOT
#define NANOFIX_RESTRICT
#endif

#define NANOFIX_ASSERT(cond, msg)                \
    do {                                         \
        if (!(cond)) [[unlikely]]                \
            ::nanofix::detail::assert_fail(msg); \
    } while (0)

#ifndef NANOFIX_MAX_BODY_LENGTH
#define NANOFIX_MAX_BODY_LENGTH 999'999'999
#endif
