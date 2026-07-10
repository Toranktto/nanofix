#pragma once

#if !defined(NANOFIX_DISABLE_SIMD)
#if defined(__aarch64__) || defined(_M_ARM64)
#define NANOFIX_HAS_NEON 1
#endif
#if defined(__x86_64__) || defined(_M_X64)
#if !defined(__AVX2__)
#error \
    "nanofix requires AVX2 on x86-64 (Haswell+, 2013). Compile with -mavx2 / -march=haswell (MSVC: /arch:AVX2) -- the nanofix CMake/Conan targets add it -- or define NANOFIX_DISABLE_SIMD=1 for scalar-only."
#endif
#define NANOFIX_HAS_AVX2 1
#endif
#endif

#ifdef NANOFIX_HAS_NEON
#include <arm_neon.h>
#endif
#ifdef NANOFIX_HAS_AVX2
#include <immintrin.h>
#endif

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
