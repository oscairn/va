// SPDX-License-Identifier: AGPL-3.0-or-later
// The small vector type the voice kernels are written against: NEON or SSE2 (4 voices a lane), or
// scalar (1). W is the lane width; everything else in the core uses only this API, so an 8-wide
// AVX2 variant is a new branch here plus a second compilation of core.cpp behind a runtime check
// (docs/design.md); nothing else would change.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(_MSC_VER) && !defined(__clang__)
#define VC_INLINE __forceinline
#define VC_NOINLINE __declspec(noinline)
#else
#define VC_INLINE inline __attribute__((always_inline))
#define VC_NOINLINE __attribute__((noinline))
#endif

#if defined(VCORE_SCALAR)
#define VC_IMPL "scalar"
#elif defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
#define VC_NEON 1
#define VC_IMPL "neon"
#include <arm_neon.h>
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define VC_SSE2 1
#define VC_IMPL "sse2"
#include <emmintrin.h>
#else
#define VCORE_SCALAR 1
#define VC_IMPL "scalar"
#endif

namespace vcore {

#if VC_NEON
constexpr int W = 4;
struct vf { float32x4_t v; };
struct vm { uint32x4_t v; };
VC_INLINE vf splat(float x) { return {vdupq_n_f32(x)}; }
VC_INLINE vf load(const float* p) { return {vld1q_f32(p)}; }
VC_INLINE void store(float* p, vf a) { vst1q_f32(p, a.v); }
VC_INLINE vf operator+(vf a, vf b) { return {vaddq_f32(a.v, b.v)}; }
VC_INLINE vf operator-(vf a, vf b) { return {vsubq_f32(a.v, b.v)}; }
VC_INLINE vf operator*(vf a, vf b) { return {vmulq_f32(a.v, b.v)}; }
VC_INLINE vf operator/(vf a, vf b) { return {vdivq_f32(a.v, b.v)}; }
VC_INLINE vf fma(vf a, vf b, vf c) { return {vfmaq_f32(c.v, a.v, b.v)}; }  // a*b+c
VC_INLINE vf vmin(vf a, vf b) { return {vminq_f32(a.v, b.v)}; }
VC_INLINE vf vmax(vf a, vf b) { return {vmaxq_f32(a.v, b.v)}; }
VC_INLINE vm operator<(vf a, vf b) { return {vcltq_f32(a.v, b.v)}; }
VC_INLINE vm operator>=(vf a, vf b) { return {vcgeq_f32(a.v, b.v)}; }
VC_INLINE vf sel(vm m, vf a, vf b) { return {vbslq_f32(m.v, a.v, b.v)}; }
VC_INLINE vf and0(vm m, vf a) { return {vreinterpretq_f32_u32(vandq_u32(m.v, vreinterpretq_u32_f32(a.v)))}; }
VC_INLINE vf vfloor(vf a) { return {vrndmq_f32(a.v)}; }
VC_INLINE vf vabs(vf a) { return {vabsq_f32(a.v)}; }
VC_INLINE vf ldexpi(vf a, vf n) { return {vreinterpretq_f32_s32(vaddq_s32(vreinterpretq_s32_f32(a.v), vshlq_n_s32(vcvtq_s32_f32(n.v), 23)))}; }
VC_INLINE float hsum(vf a) { return vaddvq_f32(a.v); }
// t[i] and t[i + 1] per lane; i holds non-negative whole numbers (a table read: one 8-byte load a lane)
VC_INLINE void gather2(const float* t, vf i, vf& lo, vf& hi) {
    const int32x4_t n = vcvtq_s32_f32(i.v);
    const float32x2_t a = vld1_f32(t + vgetq_lane_s32(n, 0)), b = vld1_f32(t + vgetq_lane_s32(n, 1));
    const float32x2_t c = vld1_f32(t + vgetq_lane_s32(n, 2)), d = vld1_f32(t + vgetq_lane_s32(n, 3));
    const float32x4x2_t u = vuzpq_f32(vcombine_f32(a, b), vcombine_f32(c, d));
    lo.v = u.val[0]; hi.v = u.val[1];
}
#elif VC_SSE2
constexpr int W = 4;
struct vf { __m128 v; };
struct vm { __m128 v; };
VC_INLINE vf splat(float x) { return {_mm_set1_ps(x)}; }
VC_INLINE vf load(const float* p) { return {_mm_load_ps(p)}; }
VC_INLINE void store(float* p, vf a) { _mm_store_ps(p, a.v); }
VC_INLINE vf operator+(vf a, vf b) { return {_mm_add_ps(a.v, b.v)}; }
VC_INLINE vf operator-(vf a, vf b) { return {_mm_sub_ps(a.v, b.v)}; }
VC_INLINE vf operator*(vf a, vf b) { return {_mm_mul_ps(a.v, b.v)}; }
VC_INLINE vf operator/(vf a, vf b) { return {_mm_div_ps(a.v, b.v)}; }
VC_INLINE vf fma(vf a, vf b, vf c) { return {_mm_add_ps(_mm_mul_ps(a.v, b.v), c.v)}; }
VC_INLINE vf vmin(vf a, vf b) { return {_mm_min_ps(a.v, b.v)}; }
VC_INLINE vf vmax(vf a, vf b) { return {_mm_max_ps(a.v, b.v)}; }
VC_INLINE vm operator<(vf a, vf b) { return {_mm_cmplt_ps(a.v, b.v)}; }
VC_INLINE vm operator>=(vf a, vf b) { return {_mm_cmpge_ps(a.v, b.v)}; }
VC_INLINE vf sel(vm m, vf a, vf b) { return {_mm_or_ps(_mm_and_ps(m.v, a.v), _mm_andnot_ps(m.v, b.v))}; }
VC_INLINE vf and0(vm m, vf a) { return {_mm_and_ps(m.v, a.v)}; }
VC_INLINE vf vabs(vf a) { return {_mm_andnot_ps(_mm_set1_ps(-0.f), a.v)}; }
VC_INLINE vf vfloor(vf a) {
    __m128 t = _mm_cvtepi32_ps(_mm_cvttps_epi32(a.v));
    return {_mm_sub_ps(t, _mm_and_ps(_mm_cmpgt_ps(t, a.v), _mm_set1_ps(1.f)))};
}
VC_INLINE vf ldexpi(vf a, vf n) { return {_mm_castsi128_ps(_mm_add_epi32(_mm_castps_si128(a.v), _mm_slli_epi32(_mm_cvttps_epi32(n.v), 23)))}; }
VC_INLINE float hsum(vf a) {
    __m128 s = _mm_add_ps(a.v, _mm_movehl_ps(a.v, a.v));
    return _mm_cvtss_f32(_mm_add_ss(s, _mm_shuffle_ps(s, s, 1)));
}
VC_INLINE void gather2(const float* t, vf i, vf& lo, vf& hi) {
    alignas(16) int32_t n[4]; _mm_store_si128(reinterpret_cast<__m128i*>(n), _mm_cvttps_epi32(i.v));
    const __m128 ab = _mm_loadh_pi(_mm_loadl_pi(_mm_setzero_ps(), reinterpret_cast<const __m64*>(t + n[0])), reinterpret_cast<const __m64*>(t + n[1]));
    const __m128 cd = _mm_loadh_pi(_mm_loadl_pi(_mm_setzero_ps(), reinterpret_cast<const __m64*>(t + n[2])), reinterpret_cast<const __m64*>(t + n[3]));
    lo.v = _mm_shuffle_ps(ab, cd, _MM_SHUFFLE(2, 0, 2, 0)); hi.v = _mm_shuffle_ps(ab, cd, _MM_SHUFFLE(3, 1, 3, 1));
}
#else
constexpr int W = 1;
struct vf { float v; };
struct vm { bool v; };
VC_INLINE vf splat(float x) { return {x}; }
VC_INLINE vf load(const float* p) { return {*p}; }
VC_INLINE void store(float* p, vf a) { *p = a.v; }
VC_INLINE vf operator+(vf a, vf b) { return {a.v + b.v}; }
VC_INLINE vf operator-(vf a, vf b) { return {a.v - b.v}; }
VC_INLINE vf operator*(vf a, vf b) { return {a.v * b.v}; }
VC_INLINE vf operator/(vf a, vf b) { return {a.v / b.v}; }
VC_INLINE vf fma(vf a, vf b, vf c) { return {a.v * b.v + c.v}; }
VC_INLINE vf vmin(vf a, vf b) { return {a.v < b.v ? a.v : b.v}; }
VC_INLINE vf vmax(vf a, vf b) { return {a.v > b.v ? a.v : b.v}; }
VC_INLINE vm operator<(vf a, vf b) { return {a.v < b.v}; }
VC_INLINE vm operator>=(vf a, vf b) { return {a.v >= b.v}; }
VC_INLINE vf sel(vm m, vf a, vf b) { return {m.v ? a.v : b.v}; }
VC_INLINE vf and0(vm m, vf a) { return {m.v ? a.v : 0.f}; }
VC_INLINE vf vfloor(vf a) { return {std::floor(a.v)}; }
VC_INLINE vf vabs(vf a) { return {std::fabs(a.v)}; }
VC_INLINE vf ldexpi(vf a, vf n) {
    int32_t i; std::memcpy(&i, &a.v, 4); i += int32_t(n.v) * (1 << 23); float r; std::memcpy(&r, &i, 4); return {r};
}
VC_INLINE float hsum(vf a) { return a.v; }
VC_INLINE void gather2(const float* t, vf i, vf& lo, vf& hi) { const int32_t n = int32_t(i.v); lo.v = t[n]; hi.v = t[n + 1]; }
#endif

#if VC_NEON
VC_INLINE vf vsqrt(vf a) { return {vsqrtq_f32(a.v)}; }
// x = m 2^e with m in [1, 2), for x > 0 (normal)
VC_INLINE void vsplit(vf x, vf& e, vf& m) {
    const int32x4_t b = vreinterpretq_s32_f32(x.v);
    e.v = vcvtq_f32_s32(vsubq_s32(vshrq_n_s32(b, 23), vdupq_n_s32(127)));
    m.v = vreinterpretq_f32_s32(vorrq_s32(vandq_s32(b, vdupq_n_s32(0x7fffff)), vdupq_n_s32(0x3f800000)));
}
#elif VC_SSE2
VC_INLINE vf vsqrt(vf a) { return {_mm_sqrt_ps(a.v)}; }
VC_INLINE void vsplit(vf x, vf& e, vf& m) {
    const __m128i b = _mm_castps_si128(x.v);
    e.v = _mm_cvtepi32_ps(_mm_sub_epi32(_mm_srli_epi32(b, 23), _mm_set1_epi32(127)));
    m.v = _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(b, _mm_set1_epi32(0x7fffff)), _mm_set1_epi32(0x3f800000)));
}
#else
VC_INLINE vf vsqrt(vf a) { return {std::sqrt(a.v)}; }
VC_INLINE void vsplit(vf x, vf& e, vf& m) {
    int32_t b; std::memcpy(&b, &x.v, 4);
    e.v = float((b >> 23) - 127); b = (b & 0x7fffff) | 0x3f800000; std::memcpy(&m.v, &b, 4);
}
#endif

// 2^x, x clamped to +-30 (degree-5 polynomial on the fraction: 1.6e-4 relative, 0.3 cent). Control rate.
VC_INLINE vf exp2v(vf x) {
    x = vmax(vmin(x, splat(30.f)), splat(-30.f));
    vf n = vfloor(x), f = x - n;
    vf p = fma(f, splat(1.3333558e-3f), splat(9.6181291e-3f));
    p = fma(p, f, splat(5.5504109e-2f));
    p = fma(p, f, splat(2.4022650e-1f));
    p = fma(p, f, splat(6.9314718e-1f));
    p = fma(p, f, splat(1.f));
    return ldexpi(p, n);
}
inline float exp2s(float x) { alignas(16) float r[W]; store(r, exp2v(splat(x))); return r[0]; }
// tan(x) for 0 <= x < 1.45 (Pade 5/4). Control rate.
VC_INLINE vf tanv(vf x) {
    vf x2 = x * x;
    vf num = x * fma(x2, fma(x2, splat(1.f), splat(-105.f)), splat(945.f));
    vf den = fma(x2, fma(x2, splat(15.f), splat(-420.f)), splat(945.f));
    return num / den;
}

// log2(x) for x > 0 normal (degree-6 polynomial on the mantissa, 4e-6). Control rate.
VC_INLINE vf log2v(vf x) {
    vf e, m; vsplit(x, e, m);
    const vf t = m - splat(1.f);
    vf p = fma(t, splat(-0.0260663f), splat(0.12191414f));
    p = fma(p, t, splat(-0.27736505f));
    p = fma(p, t, splat(0.45689417f));
    p = fma(p, t, splat(-0.71789838f));
    p = fma(p, t, splat(1.44251703f));
    return fma(p, t, e);
}
// acos(x), |x| <= 1 (Abramowitz-Stegun 4.4.45: 7e-5 rad). Control rate.
VC_INLINE vf acosv(vf x) {
    const vf a = vabs(x);
    vf p = fma(a, splat(-0.0187293f), splat(0.0742610f));
    p = fma(p, a, splat(-0.2121144f));
    p = fma(p, a, splat(1.5707288f));
    p = p * vsqrt(vmax(splat(1.f) - a, splat(0.f)));
    return sel(x < splat(0.f), splat(3.14159265f) - p, p);
}
// cos(x) for 0 <= x <= pi: sin(pi/2 - x) by its odd polynomial (9e-5 absolute, exact in its leading term). Control rate.
VC_INLINE vf cosv(vf x) {
    const vf y = splat(1.5707963f) - x, y2 = y * y;
    return y * fma(y2, fma(y2, fma(y2, splat(-1.9515296e-4f), splat(8.3321608e-3f)), splat(-0.16666654f)), splat(1.f));
}

}  // namespace vcore
