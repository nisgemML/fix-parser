#pragma once
// include/fix/simd.hpp — Vectorised delimiter scan and checksum.
//
// Two primitives the parser hot path needs:
//
//   find_byte(p, end, c)      → first pointer q in [p,end) with *q == c, or end
//   sum_bytes(p, end)         → (uint8_t) sum of bytes, i.e. the FIX checksum
//
// AVX2 path: compare 32 bytes per instruction (VPCMPEQB + VPMOVMSKB), and
// accumulate the checksum with VPSADBW against zero (8 lanes of 64-bit sums).
// Scalar path is the reference; tests assert both paths agree byte-for-byte
// on the full corpus, so a build without AVX2 is slower but never different.
//
// Why this matters for FIX specifically: a 171-byte NewOrderSingle has ~14
// SOH bytes. The scalar loop takes ~171 iterations with a mispredicting branch
// at each delimiter; the AVX2 loop takes 6 compares plus ~14 tzcnt.

#include <cstddef>
#include <cstdint>

#if defined(__AVX2__)
#  include <immintrin.h>
#  define FIX_HAVE_AVX2 1
#else
#  define FIX_HAVE_AVX2 0
#endif

namespace fix::simd {

// ── Scalar reference implementations ───────────────────────────────────────

[[nodiscard]] inline const char*
find_byte_scalar(const char* p, const char* end, char c) noexcept {
    while (p < end && *p != c) ++p;
    return p;
}

[[nodiscard]] inline uint8_t
sum_bytes_scalar(const char* p, const char* end) noexcept {
    uint8_t s = 0;
    while (p < end) s += uint8_t(*p++);
    return s;
}

// ── AVX2 implementations ────────────────────────────────────────────────────

#if FIX_HAVE_AVX2

[[nodiscard]] inline const char*
find_byte_avx2(const char* p, const char* end, char c) noexcept {
    const __m256i needle = _mm256_set1_epi8(c);
    while (end - p >= 32) {
        __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
        uint32_t mask = uint32_t(_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, needle)));
        if (mask) return p + __builtin_ctz(mask);
        p += 32;
    }
    return find_byte_scalar(p, end, c);
}

[[nodiscard]] inline uint8_t
sum_bytes_avx2(const char* p, const char* end) noexcept {
    __m256i acc = _mm256_setzero_si256();
    const __m256i zero = _mm256_setzero_si256();
    while (end - p >= 32) {
        __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
        acc = _mm256_add_epi64(acc, _mm256_sad_epu8(chunk, zero));
        p += 32;
    }
    alignas(32) uint64_t lanes[4];
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), acc);
    uint64_t total = lanes[0] + lanes[1] + lanes[2] + lanes[3];
    return uint8_t(total + sum_bytes_scalar(p, end));
}

#endif // FIX_HAVE_AVX2

// ── Dispatch ────────────────────────────────────────────────────────────────

[[nodiscard]] inline const char*
find_byte(const char* p, const char* end, char c) noexcept {
#if FIX_HAVE_AVX2
    return find_byte_avx2(p, end, c);
#else
    return find_byte_scalar(p, end, c);
#endif
}

[[nodiscard]] inline uint8_t
sum_bytes(const char* p, const char* end) noexcept {
#if FIX_HAVE_AVX2
    return sum_bytes_avx2(p, end);
#else
    return sum_bytes_scalar(p, end);
#endif
}

} // namespace fix::simd
