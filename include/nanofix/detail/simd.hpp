#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <nanofix/detail/config.hpp>

namespace nanofix {

/* @cond EXCLUDE */

namespace detail {

NANOFIX_ALWAYS_INLINE std::size_t find_all_soh_scalar(char const* begin,
                                                      char const* end,
                                                      std::uint32_t* NANOFIX_RESTRICT out,
                                                      std::size_t cap) noexcept {
    char const* const base = begin;
    std::size_t n = 0;
    for (; begin < end && n < cap; ++begin)
        if (*begin == '\x01')
            out[n++] = static_cast<std::uint32_t>(begin - base);
    return n;
}

#ifdef NANOFIX_HAS_NEON
NANOFIX_ALWAYS_INLINE std::size_t find_all_soh_neon(char const* begin,
                                                    char const* end,
                                                    std::uint32_t* NANOFIX_RESTRICT out,
                                                    std::size_t cap) noexcept {
    char const* const base = begin;
    std::size_t n = 0;
    static constexpr std::uint8_t kBits[16] = {
        1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    uint8x16_t const bitmask = vld1q_u8(kBits);
    while (end - begin >= 16 && n + 16 <= cap) {
        uint8x16_t const v = vld1q_u8(reinterpret_cast<std::uint8_t const*>(begin));
        uint8x16_t const anded = vandq_u8(vceqq_u8(v, vdupq_n_u8(0x01)), bitmask);
        unsigned mask = vaddv_u8(vget_low_u8(anded)) |
                        (static_cast<unsigned>(vaddv_u8(vget_high_u8(anded))) << 8);
        auto const offbase = static_cast<std::uint32_t>(begin - base);
        while (mask) {
            out[n++] = offbase + static_cast<std::uint32_t>(std::countr_zero(mask));
            mask &= mask - 1;
        }
        begin += 16;
    }
    for (; begin < end && n < cap; ++begin)
        if (*begin == '\x01')
            out[n++] = static_cast<std::uint32_t>(begin - base);
    return n;
}
#endif

#ifdef NANOFIX_HAS_AVX2
NANOFIX_ALWAYS_INLINE std::size_t find_all_soh_avx2(char const* begin,
                                                    char const* end,
                                                    std::uint32_t* NANOFIX_RESTRICT out,
                                                    std::size_t cap) noexcept {
    char const* const base = begin;
    std::size_t n = 0;
    __m256i const soh = _mm256_set1_epi8(0x01);
    while (end - begin >= 32 && n + 32 <= cap) {
        __m256i const v = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(begin));
        auto mask = static_cast<std::uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, soh)));
        auto const offbase = static_cast<std::uint32_t>(begin - base);
        while (mask) {
            out[n++] = offbase + static_cast<std::uint32_t>(std::countr_zero(mask));
            mask &= mask - 1;
        }
        begin += 32;
    }
    for (; begin < end && n < cap; ++begin)
        if (*begin == '\x01')
            out[n++] = static_cast<std::uint32_t>(begin - base);
    return n;
}
#endif

NANOFIX_ALWAYS_INLINE std::size_t find_all_soh(char const* begin,
                                               char const* end,
                                               std::uint32_t* out,
                                               std::size_t cap) noexcept {
#if defined(NANOFIX_HAS_NEON)
    return find_all_soh_neon(begin, end, out, cap);
#elif defined(NANOFIX_HAS_AVX2)
    return find_all_soh_avx2(begin, end, out, cap);
#else
    return find_all_soh_scalar(begin, end, out, cap);
#endif
}

NANOFIX_ALWAYS_INLINE std::size_t find_tag_in_index_scalar(int const* tags,
                                                           std::size_t n,
                                                           int tag) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        if (tags[i] == tag)
            return i;
    }
    return n;
}

#ifdef NANOFIX_HAS_NEON
NANOFIX_ALWAYS_INLINE std::size_t find_tag_in_index_neon(int const* tags,
                                                         std::size_t n,
                                                         int tag) noexcept {
    std::size_t i = 0;
    int32x4_t const target = vdupq_n_s32(tag);
    while (i + 8 <= n) {
        int32x4_t const v0 = vld1q_s32(tags + i);
        int32x4_t const v1 = vld1q_s32(tags + i + 4);
        uint32x4_t const eq0 = vceqq_s32(v0, target);
        uint32x4_t const eq1 = vceqq_s32(v1, target);
        uint16x8_t const combined = vcombine_u16(vshrn_n_u32(eq0, 16), vshrn_n_u32(eq1, 16));
        std::uint64_t const bits = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(combined, 4)), 0);
        if (bits) [[unlikely]]
            return i + (std::countr_zero(bits) >> 3);
        i += 8;
    }
    while (i + 4 <= n) {
        int32x4_t const v = vld1q_s32(tags + i);
        uint32x4_t const eq = vceqq_s32(v, target);
        std::uint64_t const bits = vget_lane_u64(vreinterpret_u64_u16(vshrn_n_u32(eq, 16)), 0);
        if (bits) [[unlikely]]
            return i + (std::countr_zero(bits) >> 4);
        i += 4;
    }
    for (; i < n; ++i) {
        if (tags[i] == tag)
            return i;
    }
    return n;
}
#endif

#ifdef NANOFIX_HAS_AVX2
NANOFIX_ALWAYS_INLINE std::size_t find_tag_in_index_avx2(int const* tags,
                                                         std::size_t n,
                                                         int tag) noexcept {
    std::size_t i = 0;
    __m256i const target = _mm256_set1_epi32(tag);
    while (i + 8 <= n) {
        __m256i const v = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(tags + i));
        __m256i const eq = _mm256_cmpeq_epi32(v, target);
        std::uint32_t const mask =
            static_cast<std::uint32_t>(_mm256_movemask_ps(_mm256_castsi256_ps(eq)));
        if (mask) [[unlikely]]
            return i + std::countr_zero(mask);
        i += 8;
    }
    __m128i const target128 = _mm_set1_epi32(tag);
    while (i + 4 <= n) {
        __m128i const v = _mm_loadu_si128(reinterpret_cast<__m128i const*>(tags + i));
        __m128i const eq = _mm_cmpeq_epi32(v, target128);
        std::uint32_t const mask = static_cast<std::uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(eq)));
        if (mask) [[unlikely]]
            return i + std::countr_zero(mask);
        i += 4;
    }
    for (; i < n; ++i) {
        if (tags[i] == tag)
            return i;
    }
    return n;
}
#endif

NANOFIX_ALWAYS_INLINE std::size_t find_tag_in_index(int const* tags, std::size_t n, int tag) noexcept {
#if defined(NANOFIX_HAS_NEON)
    return find_tag_in_index_neon(tags, n, tag);
#elif defined(NANOFIX_HAS_AVX2)
    return find_tag_in_index_avx2(tags, n, tag);
#else
    return find_tag_in_index_scalar(tags, n, tag);
#endif
}

NANOFIX_ALWAYS_INLINE std::uint8_t checksum_bytes_scalar(char const* begin, char const* end) noexcept {
    std::uint8_t sum = 0;
    while (begin != end)
        sum = static_cast<std::uint8_t>(sum + std::uint8_t(*begin++));
    return sum;
}

#ifdef NANOFIX_HAS_NEON
NANOFIX_ALWAYS_INLINE std::uint8_t checksum_bytes_neon(char const* begin, char const* end) noexcept {
    if (end - begin < 1024)
        return checksum_bytes_scalar(begin, end);
    uint8x16_t a0 = vdupq_n_u8(0), a1 = vdupq_n_u8(0);
    uint8x16_t a2 = vdupq_n_u8(0), a3 = vdupq_n_u8(0);
    while (end - begin >= 64) {
        auto const* p = reinterpret_cast<std::uint8_t const*>(begin);
        a0 = vaddq_u8(a0, vld1q_u8(p));
        a1 = vaddq_u8(a1, vld1q_u8(p + 16));
        a2 = vaddq_u8(a2, vld1q_u8(p + 32));
        a3 = vaddq_u8(a3, vld1q_u8(p + 48));
        begin += 64;
    }
    a0 = vaddq_u8(a0, a1);
    a2 = vaddq_u8(a2, a3);
    a0 = vaddq_u8(a0, a2);
    while (end - begin >= 16) {
        a0 = vaddq_u8(a0, vld1q_u8(reinterpret_cast<std::uint8_t const*>(begin)));
        begin += 16;
    }
    std::uint8_t sum = vaddvq_u8(a0);
    while (begin != end)
        sum = static_cast<std::uint8_t>(sum + std::uint8_t(*begin++));
    return sum;
}
#endif

#ifdef NANOFIX_HAS_AVX2
NANOFIX_ALWAYS_INLINE std::uint8_t checksum_bytes_avx2(char const* begin, char const* end) noexcept {
    __m256i a0 = _mm256_setzero_si256(), a1 = _mm256_setzero_si256();
    __m256i a2 = _mm256_setzero_si256(), a3 = _mm256_setzero_si256();
    while (end - begin >= 128) {
        auto const* p = reinterpret_cast<__m256i const*>(begin);
        a0 = _mm256_add_epi8(a0, _mm256_loadu_si256(p));
        a1 = _mm256_add_epi8(a1, _mm256_loadu_si256(p + 1));
        a2 = _mm256_add_epi8(a2, _mm256_loadu_si256(p + 2));
        a3 = _mm256_add_epi8(a3, _mm256_loadu_si256(p + 3));
        begin += 128;
    }
    a0 = _mm256_add_epi8(a0, a1);
    a2 = _mm256_add_epi8(a2, a3);
    a0 = _mm256_add_epi8(a0, a2);
    while (end - begin >= 32) {
        a0 = _mm256_add_epi8(a0, _mm256_loadu_si256(reinterpret_cast<__m256i const*>(begin)));
        begin += 32;
    }
    __m128i lo = _mm256_castsi256_si128(a0);
    __m128i hi = _mm256_extracti128_si256(a0, 1);
    __m128i v128 = _mm_add_epi8(lo, hi);
    __m128i sad = _mm_sad_epu8(v128, _mm_setzero_si128());
    std::uint64_t const horiz = static_cast<std::uint64_t>(_mm_extract_epi64(sad, 0)) +
                                static_cast<std::uint64_t>(_mm_extract_epi64(sad, 1));
    std::uint8_t sum = static_cast<std::uint8_t>(horiz);
    while (begin != end)
        sum = static_cast<std::uint8_t>(sum + std::uint8_t(*begin++));
    return sum;
}
#endif

NANOFIX_ALWAYS_INLINE std::uint8_t checksum_bytes(char const* begin, char const* end) noexcept {
#if defined(NANOFIX_HAS_NEON)
    return checksum_bytes_neon(begin, end);
#elif defined(NANOFIX_HAS_AVX2)
    return checksum_bytes_avx2(begin, end);
#else
    return checksum_bytes_scalar(begin, end);
#endif
}

}  // namespace detail

/* @endcond*/
}  // namespace nanofix
