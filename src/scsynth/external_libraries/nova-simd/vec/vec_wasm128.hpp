//  wasm simd128 vector class for float
//
//  Replaces the generic scalar fallback when targeting WebAssembly.
//  Uses emscripten's <wasm_simd128.h> for native 128-bit SIMD operations.
//
//  2026 — created for SuperSonic (scsynth in AudioWorklet)

#ifndef VEC_WASM128_HPP
#define VEC_WASM128_HPP

#include <wasm_simd128.h>
#include <cmath>

#include "../detail/vec_math.hpp"
#include "vec_base.hpp"

#if defined(__GNUC__) && defined(NDEBUG)
#define always_inline inline  __attribute__((always_inline))
#else
#define always_inline inline
#endif

namespace nova
{

namespace detail {

struct int_vec_wasm128
{
    v128_t data_;

    explicit int_vec_wasm128(int arg):
        data_(wasm_i32x4_splat(arg))
    {}

    int_vec_wasm128(v128_t arg):
        data_(arg)
    {}

    int_vec_wasm128(int_vec_wasm128 const & arg):
        data_(arg.data_)
    {}

    int_vec_wasm128(void)
    {}

    operator v128_t (void) const
    {
        return data_;
    }

    friend int_vec_wasm128 operator+(int_vec_wasm128 const & lhs, int_vec_wasm128 const & rhs)
    {
        return int_vec_wasm128(wasm_i32x4_add(lhs.data_, rhs.data_));
    }

    friend int_vec_wasm128 operator-(int_vec_wasm128 const & lhs, int_vec_wasm128 const & rhs)
    {
        return int_vec_wasm128(wasm_i32x4_sub(lhs.data_, rhs.data_));
    }

    #define RELATIONAL_MASK_OPERATOR(op, opcode) \
    friend inline int_vec_wasm128 mask_##op(int_vec_wasm128 const & lhs, int_vec_wasm128 const & rhs) \
    { \
        return int_vec_wasm128(opcode(lhs.data_, rhs.data_)); \
    }

    RELATIONAL_MASK_OPERATOR(lt, wasm_i32x4_lt)
    RELATIONAL_MASK_OPERATOR(gt, wasm_i32x4_gt)
    RELATIONAL_MASK_OPERATOR(eq, wasm_i32x4_eq)

    #undef RELATIONAL_MASK_OPERATOR

    friend int_vec_wasm128 operator&(int_vec_wasm128 const & lhs, int_vec_wasm128 const & rhs)
    {
        return int_vec_wasm128(wasm_v128_and(lhs.data_, rhs.data_));
    }

    friend inline int_vec_wasm128 andnot(int_vec_wasm128 const & lhs, int_vec_wasm128 const & rhs)
    {
        // SSE _mm_andnot(a,b) = ~a & b. WASM wasm_v128_andnot(a,b) = a & ~b.
        // Swap operands to match SSE semantics expected by vec_math.hpp.
        return int_vec_wasm128(wasm_v128_andnot(rhs.data_, lhs.data_));
    }

    friend inline int_vec_wasm128 slli(int_vec_wasm128 const & arg, int count)
    {
        return int_vec_wasm128(wasm_i32x4_shl(arg.data_, count));
    }

    friend inline int_vec_wasm128 srli(int_vec_wasm128 const & arg, int count)
    {
        return int_vec_wasm128(wasm_u32x4_shr(arg.data_, count));
    }

    inline v128_t convert_to_float(void) const
    {
        return wasm_f32x4_convert_i32x4(data_);
    }
};

} // namespace detail

template <>
struct vec<float>:
    vec_base<float, v128_t, 4>
{
    typedef v128_t internal_vector_type;
    typedef float float_type;
    typedef vec_base<float, v128_t, 4> base;

    static inline v128_t gen_sign_mask(void)
    {
        return wasm_u32x4_splat(0x80000000);
    }

    static inline v128_t gen_abs_mask(void)
    {
        return wasm_u32x4_splat(0x7FFFFFFF);
    }

    static inline v128_t gen_one(void)
    {
        return wasm_f32x4_splat(1.0f);
    }

    static inline v128_t gen_05(void)
    {
        return wasm_f32x4_splat(0.5f);
    }

    static inline v128_t gen_exp_mask(void)
    {
        return wasm_u32x4_splat(0x7F800000);
    }

    static inline v128_t gen_exp_mask_1(void)
    {
        return wasm_u32x4_splat(0x3F000000);
    }

    static inline v128_t gen_ones(void)
    {
        return wasm_u32x4_splat(0xFFFFFFFF);
    }

    static inline v128_t gen_zero(void)
    {
        return wasm_f32x4_splat(0.0f);
    }

    vec(v128_t const & arg):
        base(arg)
    {}

public:
    static const int size = 4;
    static const int objects_per_cacheline = 64/sizeof(float);
    static const bool has_compare_bitmask = true;

    static bool is_aligned(float* ptr)
    {
        return ((intptr_t)(ptr) & (intptr_t)(size * sizeof(float) - 1)) == 0;
    }

    /* @{ */
    /** constructors */
    vec(void)
    {}

    vec(float f)
    {
        set_vec(f);
    }

    vec(vec const & rhs):
        base(rhs)
    {
    }
    /* @} */

    /* @{ */
    /** io */
    void load(const float * data)
    {
        data_ = wasm_v128_load(data);
    }

    void load_aligned(const float * data)
    {
        data_ = wasm_v128_load(data); // WASM has no alignment requirement
    }

    void load_first(const float * data)
    {
        data_ = wasm_f32x4_make(*data, 0.0f, 0.0f, 0.0f);
    }

    void store(float * dest) const
    {
        wasm_v128_store(dest, data_);
    }

    void store_aligned(float * dest) const
    {
        wasm_v128_store(dest, data_); // WASM has no alignment requirement
    }

    void store_aligned_stream(float * dest) const
    {
        wasm_v128_store(dest, data_); // no streaming stores in WASM
    }

    /* @} */

    /** cast to internal type */
    operator v128_t (void) const
    {
        return data_;
    }

    void set_vec (float value)
    {
        data_ = wasm_f32x4_splat(value);
    }

    float set_slope(float start, float slope)
    {
        float v0 = start;
        float v1 = start + slope;
        float v2 = start + slope + slope;
        float v3 = start + slope + slope + slope;
        data_ = wasm_f32x4_make(v0, v1, v2, v3);
        return slope * size; // return total advance (matches vec_base behavior)
    }

    float set_exp(float start, float curve)
    {
        float v0 = start;
        float v1 = v0 * curve;
        float v2 = v1 * curve;
        float v3 = v2 * curve;
        data_ = wasm_f32x4_make(v0, v1, v2, v3);
        return v3 * curve;
    }

    float get (int index) const
    {
        // WASM SIMD lane extraction
        switch(index) {
            case 0: return wasm_f32x4_extract_lane(data_, 0);
            case 1: return wasm_f32x4_extract_lane(data_, 1);
            case 2: return wasm_f32x4_extract_lane(data_, 2);
            case 3: return wasm_f32x4_extract_lane(data_, 3);
            default: return 0.0f;
        }
    }

    float get_first(void) const
    {
        return wasm_f32x4_extract_lane(data_, 0);
    }

    /* @{ */
    /** arithmetic operators */
#define OPERATOR_ASSIGNMENT(op, opfn) \
    vec & operator op(vec const & rhs) \
    { \
        data_ = opfn(data_, rhs.data_); \
        return *this; \
    }

    OPERATOR_ASSIGNMENT(+=, wasm_f32x4_add)
    OPERATOR_ASSIGNMENT(-=, wasm_f32x4_sub)
    OPERATOR_ASSIGNMENT(*=, wasm_f32x4_mul)
    OPERATOR_ASSIGNMENT(/=, wasm_f32x4_div)

#undef OPERATOR_ASSIGNMENT

#define ARITHMETIC_OPERATOR(op, opfn) \
    vec operator op(vec const & rhs) const \
    { \
        return opfn(data_, rhs.data_); \
    } \
    \
    friend vec operator op(vec const & lhs, float f) \
    { \
        return opfn(lhs.data_, wasm_f32x4_splat(f)); \
    } \
    \
    friend vec operator op(float f, vec const & rhs) \
    { \
        return opfn(wasm_f32x4_splat(f), rhs.data_); \
    }

    ARITHMETIC_OPERATOR(+, wasm_f32x4_add)
    ARITHMETIC_OPERATOR(-, wasm_f32x4_sub)
    ARITHMETIC_OPERATOR(*, wasm_f32x4_mul)
    ARITHMETIC_OPERATOR(/, wasm_f32x4_div)

#undef ARITHMETIC_OPERATOR

    friend vec operator -(const vec & arg)
    {
        return wasm_f32x4_neg(arg.data_);
    }

    friend vec fast_reciprocal(const vec & arg)
    {
        return wasm_f32x4_div(wasm_f32x4_splat(1.0f), arg.data_);
    }

    friend vec reciprocal(const vec & arg)
    {
        return wasm_f32x4_div(wasm_f32x4_splat(1.0f), arg.data_);
    }

    inline friend vec madd(vec const & arg1, vec const & arg2, vec const & arg3)
    {
        // arg1 * arg2 + arg3
        return wasm_f32x4_add(wasm_f32x4_mul(arg1.data_, arg2.data_), arg3.data_);
    }

    /* @{ */
    /** comparison operators */

#define RELATIONAL_OPERATOR(op, opfn) \
    vec operator op(vec const & rhs) const \
    { \
        return opfn(data_, rhs.data_); \
    }

    RELATIONAL_OPERATOR(<, wasm_f32x4_lt)
    RELATIONAL_OPERATOR(<=, wasm_f32x4_le)
    RELATIONAL_OPERATOR(>, wasm_f32x4_gt)
    RELATIONAL_OPERATOR(>=, wasm_f32x4_ge)
    RELATIONAL_OPERATOR(==, wasm_f32x4_eq)
    RELATIONAL_OPERATOR(!=, wasm_f32x4_ne)

#undef RELATIONAL_OPERATOR

    /* @} */

    /* @{ */
    /** bitwise operators */
    vec operator &(vec const & rhs) const
    {
        return wasm_v128_and(data_, rhs.data_);
    }

    vec operator |(vec const & rhs) const
    {
        return wasm_v128_or(data_, rhs.data_);
    }

    vec operator ^(vec const & rhs) const
    {
        return wasm_v128_xor(data_, rhs.data_);
    }

    friend inline vec andnot(vec const & lhs, vec const & rhs)
    {
        // SSE _mm_andnot(a,b) = ~a & b. WASM wasm_v128_andnot(a,b) = a & ~b.
        return wasm_v128_andnot(rhs.data_, lhs.data_);
    }

    /* @} */

    /* @{ */
    /** mask operators */

#define MASK_OPERATOR(op, opfn) \
    friend vec mask_##op(vec const & lhs, vec const & rhs) \
    { \
        return opfn(lhs.data_, rhs.data_); \
    }

    MASK_OPERATOR(lt, wasm_f32x4_lt)
    MASK_OPERATOR(le, wasm_f32x4_le)
    MASK_OPERATOR(gt, wasm_f32x4_gt)
    MASK_OPERATOR(ge, wasm_f32x4_ge)
    MASK_OPERATOR(eq, wasm_f32x4_eq)
    MASK_OPERATOR(neq, wasm_f32x4_ne)

#undef MASK_OPERATOR

    friend inline vec select(vec lhs, vec rhs, vec bitmask)
    {
        return wasm_v128_bitselect(rhs.data_, lhs.data_, bitmask.data_);
    }
    /* @} */

    /* @{ */
    /** unary functions */
    friend inline vec abs(vec const & arg)
    {
        return wasm_f32x4_abs(arg.data_);
    }

    friend always_inline vec sign(vec const & arg)
    {
        // Returns -1, 0, or 1 based on sign
        v128_t zero = wasm_f32x4_splat(0.0f);
        v128_t pos = wasm_f32x4_gt(arg.data_, zero);
        v128_t neg = wasm_f32x4_lt(arg.data_, zero);
        v128_t one = wasm_f32x4_splat(1.0f);
        v128_t neg_one = wasm_f32x4_splat(-1.0f);
        v128_t result = wasm_v128_and(pos, one);
        return wasm_v128_or(result, wasm_v128_and(neg, neg_one));
    }

    friend inline vec square(vec const & arg)
    {
        return wasm_f32x4_mul(arg.data_, arg.data_);
    }

    friend inline vec sqrt(vec const & arg)
    {
        return wasm_f32x4_sqrt(arg.data_);
    }

    friend inline vec cube(vec const & arg)
    {
        v128_t sq = wasm_f32x4_mul(arg.data_, arg.data_);
        return wasm_f32x4_mul(sq, arg.data_);
    }
    /* @} */

    /* @{ */
    /** binary functions */
    friend inline vec max_(vec const & lhs, vec const & rhs)
    {
        return wasm_f32x4_max(lhs.data_, rhs.data_);
    }

    friend inline vec min_(vec const & lhs, vec const & rhs)
    {
        return wasm_f32x4_min(lhs.data_, rhs.data_);
    }
    /* @} */

    /* @{ */
    /** rounding functions */
    friend inline vec round(vec const & arg)
    {
        return wasm_f32x4_nearest(arg.data_);
    }

    friend inline vec frac(vec const & arg)
    {
        v128_t fl = wasm_f32x4_floor(arg.data_);
        return wasm_f32x4_sub(arg.data_, fl);
    }

    friend inline vec floor(vec const & arg)
    {
        return wasm_f32x4_floor(arg.data_);
    }

    friend inline vec ceil(vec const & arg)
    {
        return wasm_f32x4_ceil(arg.data_);
    }

    friend inline vec trunc(vec const & arg)
    {
        return wasm_f32x4_trunc(arg.data_);
    }
    /* @} */

    /* @{ */
    /** math functions — SIMD polynomial approximations from vec_math.hpp */
    friend inline vec signed_sqrt(vec const & arg) { return detail::vec_signed_sqrt(arg); }
    friend inline vec exp(vec const & arg) { return detail::vec_exp_float(arg); }
    friend inline vec log(vec const & arg) { return detail::vec_log_float(arg); }
    friend inline vec pow(vec const & arg1, vec const & arg2) { return detail::vec_pow(arg1, arg2); }
    friend inline vec sin(vec const & arg) { return detail::vec_sin_float(arg); }
    friend inline vec cos(vec const & arg) { return detail::vec_cos_float(arg); }
    friend inline vec tan(vec const & arg) { return detail::vec_tan_float(arg); }
    friend inline vec asin(vec const & arg) { return detail::vec_asin_float(arg); }
    friend inline vec acos(vec const & arg) {
        // vec_acos_float has a sign-handling bug for negative inputs on WASM.
        float tmp[4];
        wasm_v128_store(tmp, arg.data_);
        for (int i = 0; i < 4; i++) tmp[i] = std::acos(tmp[i]);
        return wasm_v128_load(tmp);
    }
    friend inline vec atan(vec const & arg) { return detail::vec_atan_float(arg); }
    friend inline vec tanh(vec const & arg) { return detail::vec_tanh_float(arg); }
    friend inline vec signed_pow(vec const & lhs, vec const & rhs) { return detail::vec_signed_pow(lhs, rhs); }
    friend inline vec log2(vec const & arg) { return detail::vec_log2(arg); }
    friend inline vec log10(vec const & arg) { return detail::vec_log10(arg); }
    /* @} */

    /* @{ */
    /** horizontal functions */
    inline float horizontal_min(void) const
    {
        float a = wasm_f32x4_extract_lane(data_, 0);
        float b = wasm_f32x4_extract_lane(data_, 1);
        float c = wasm_f32x4_extract_lane(data_, 2);
        float d = wasm_f32x4_extract_lane(data_, 3);
        float ab = a < b ? a : b;
        float cd = c < d ? c : d;
        return ab < cd ? ab : cd;
    }

    inline float horizontal_max(void) const
    {
        float a = wasm_f32x4_extract_lane(data_, 0);
        float b = wasm_f32x4_extract_lane(data_, 1);
        float c = wasm_f32x4_extract_lane(data_, 2);
        float d = wasm_f32x4_extract_lane(data_, 3);
        float ab = a > b ? a : b;
        float cd = c > d ? c : d;
        return ab > cd ? ab : cd;
    }

    inline float horizontal_sum(void) const
    {
        float a = wasm_f32x4_extract_lane(data_, 0);
        float b = wasm_f32x4_extract_lane(data_, 1);
        float c = wasm_f32x4_extract_lane(data_, 2);
        float d = wasm_f32x4_extract_lane(data_, 3);
        return a + b + c + d;
    }
    /* @} */

    /* @{ */
    friend inline vec undenormalize(vec const & arg)
    {
        return detail::vec_undenormalize(arg);
    }
    /* @} */

    /* @{ */
    /** integer vector support (needed by vec_math.hpp transcendentals) */
    typedef nova::detail::int_vec_wasm128 int_vec;

    vec (int_vec const & rhs):
        base(rhs.data_)
    {}

    int_vec truncate_to_int(void) const
    {
        return int_vec(wasm_i32x4_trunc_sat_f32x4(data_));
    }
    /* @} */
};

} /* namespace nova */

#undef always_inline

#endif /* VEC_WASM128_HPP */
