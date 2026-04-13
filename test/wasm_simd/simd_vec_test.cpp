/**
 * WASM SIMD128 nova-simd backend correctness tests.
 *
 * Tests every vec_wasm128 operation against scalar reference.
 * Compiled to WASM via emcc, run from Node.js (run_tests.mjs).
 */

#include "vec.hpp"
#include "simd_memory.hpp"
#include "simd_binary_arithmetic.hpp"
#include "simd_mix.hpp"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cfloat>
#include <limits>

using namespace nova;
typedef vec<float> fvec;

static int failures = 0;
static int tests = 0;

static void check(const char* name, bool ok) {
    tests++;
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", name);
    }
}

static bool approx(float a, float b, float tol = 1e-5f) {
    if (a != a && b != b) return true; // both NaN
    if (std::isinf(a) && std::isinf(b)) return (a > 0) == (b > 0);
    return std::fabs(a - b) <= tol;
}

static void check_vec(const char* name, fvec const& result, float expected[4], float tol = 1e-5f) {
    for (int i = 0; i < 4; i++) {
        tests++;
        if (!approx(result.get(i), expected[i], tol)) {
            failures++;
            printf("FAIL: %s[%d] expected=%g got=%g\n", name, i, expected[i], result.get(i));
        }
    }
}

extern "C" {

int run_simd_tests() {
    failures = 0;
    tests = 0;

    // ========== CONSTRUCTORS & ACCESSORS ==========

    // get/set
    {
        fvec v;
        v.set(0, 1.0f); v.set(1, 2.0f); v.set(2, 3.0f); v.set(3, 4.0f);
        float exp[] = {1, 2, 3, 4};
        check_vec("get_set", v, exp);
    }

    // broadcast constructor
    {
        fvec v(3.14f);
        float exp[] = {3.14f, 3.14f, 3.14f, 3.14f};
        check_vec("broadcast", v, exp);
    }

    // set_slope
    {
        fvec v;
        v.set_slope(1.0f, 0.5f);
        float exp[] = {1.0f, 1.5f, 2.0f, 2.5f};
        check_vec("set_slope", v, exp);
    }

    // set_exp
    {
        fvec v;
        v.set_exp(1.0f, 2.0f);
        float exp[] = {1.0f, 2.0f, 4.0f, 8.0f};
        check_vec("set_exp", v, exp);
    }

    // get_first
    {
        fvec v; v.set(0, 42.0f); v.set(1, 99.0f);
        check("get_first", v.get_first() == 42.0f);
    }

    // ========== LOAD / STORE ==========

    {
        float data[] = {1.5f, 2.5f, 3.5f, 4.5f};
        fvec v;
        v.load(data);
        float exp[] = {1.5f, 2.5f, 3.5f, 4.5f};
        check_vec("load", v, exp);

        float out[4] = {};
        v.store(out);
        for (int i = 0; i < 4; i++)
            check("store", out[i] == data[i]);
    }

    // load_first
    {
        float data[] = {7.0f, 99.0f};
        fvec v;
        v.load_first(data);
        check("load_first[0]", v.get(0) == 7.0f);
        check("load_first[1]", v.get(1) == 0.0f);
    }

    // ========== ARITHMETIC ==========

    {
        fvec a; a.set(0,1); a.set(1,2); a.set(2,3); a.set(3,4);
        fvec b; b.set(0,10); b.set(1,20); b.set(2,30); b.set(3,40);

        { fvec r = a + b; float e[] = {11,22,33,44}; check_vec("add", r, e); }
        { fvec r = a - b; float e[] = {-9,-18,-27,-36}; check_vec("sub", r, e); }
        { fvec r = a * b; float e[] = {10,40,90,160}; check_vec("mul", r, e); }
        { fvec r = b / a; float e[] = {10,10,10,10}; check_vec("div", r, e); }
        { fvec r = -a; float e[] = {-1,-2,-3,-4}; check_vec("negate", r, e); }
    }

    // scalar-vec arithmetic
    {
        fvec a; a.set(0,2); a.set(1,4); a.set(2,6); a.set(3,8);
        { fvec r = a + 1.0f; float e[] = {3,5,7,9}; check_vec("add_scalar_r", r, e); }
        { fvec r = 1.0f + a; float e[] = {3,5,7,9}; check_vec("add_scalar_l", r, e); }
        { fvec r = a * 0.5f; float e[] = {1,2,3,4}; check_vec("mul_scalar_r", r, e); }
        { fvec r = 10.0f - a; float e[] = {8,6,4,2}; check_vec("sub_scalar_l", r, e); }
    }

    // compound assignment
    {
        fvec a; a.set(0,1); a.set(1,2); a.set(2,3); a.set(3,4);
        fvec b(10.0f);
        fvec c = a; c += b; float e1[] = {11,12,13,14}; check_vec("op+=", c, e1);
        fvec d = a; d -= b; float e2[] = {-9,-8,-7,-6}; check_vec("op-=", d, e2);
        fvec f = a; f *= b; float e3[] = {10,20,30,40}; check_vec("op*=", f, e3);
        fvec g = b; g /= a; float e4[] = {10,5,10.0f/3,2.5f}; check_vec("op/=", g, e4, 1e-4f);
    }

    // madd
    {
        fvec a(2.0f), b(3.0f), c(1.0f);
        fvec r = madd(a, b, c); // a*b+c
        float exp[] = {7,7,7,7};
        check_vec("madd", r, exp);
    }

    // reciprocal
    {
        fvec a; a.set(0,2); a.set(1,4); a.set(2,5); a.set(3,10);
        fvec r = reciprocal(a);
        float exp[] = {0.5f, 0.25f, 0.2f, 0.1f};
        check_vec("reciprocal", r, exp, 1e-4f);
    }

    // ========== UNARY MATH ==========

    { fvec a; a.set(0,-1); a.set(1,2); a.set(2,-3); a.set(3,4);
      fvec r = abs(a); float e[] = {1,2,3,4}; check_vec("abs", r, e); }

    { fvec a; a.set(0,4); a.set(1,9); a.set(2,16); a.set(3,25);
      fvec r = sqrt(a); float e[] = {2,3,4,5}; check_vec("sqrt", r, e); }

    { fvec a; a.set(0,3); a.set(1,4); a.set(2,5); a.set(3,6);
      fvec r = square(a); float e[] = {9,16,25,36}; check_vec("square", r, e); }

    { fvec a; a.set(0,2); a.set(1,3); a.set(2,4); a.set(3,5);
      fvec r = cube(a); float e[] = {8,27,64,125}; check_vec("cube", r, e); }

    // sign
    { fvec a; a.set(0,-5); a.set(1,0); a.set(2,3); a.set(3,-0.1f);
      fvec r = sign(a); float e[] = {-1,0,1,-1}; check_vec("sign", r, e); }

    // signed_sqrt
    { fvec a; a.set(0,4); a.set(1,-4); a.set(2,9); a.set(3,-9);
      fvec r = signed_sqrt(a);
      float e[] = {2.0f, -2.0f, 3.0f, -3.0f};
      check_vec("signed_sqrt", r, e); }

    // ========== BINARY MATH ==========

    { fvec a; a.set(0,1); a.set(1,5); a.set(2,3); a.set(3,7);
      fvec b; b.set(0,4); b.set(1,2); b.set(2,6); b.set(3,0);
      float emin[] = {1,2,3,0}; float emax[] = {4,5,6,7};
      check_vec("min", min_(a,b), emin);
      check_vec("max", max_(a,b), emax); }

    // ========== ROUNDING ==========

    { fvec a; a.set(0,1.3f); a.set(1,2.7f); a.set(2,-1.3f); a.set(3,-2.7f);
      float er[] = {1,3,-1,-3};   check_vec("round", round(a), er);
      float ef[] = {1,2,-2,-3};   check_vec("floor", floor(a), ef);
      float ec[] = {2,3,-1,-2};   check_vec("ceil", ceil(a), ec);
      float et[] = {1,2,-1,-2};   check_vec("trunc", trunc(a), et); }

    { fvec a; a.set(0,1.25f); a.set(1,2.75f); a.set(2,-1.7f); a.set(3,4.0f);
      // frac uses floor: frac(-1.7) = -1.7 - floor(-1.7) = -1.7 - (-2) = 0.3
      float e[] = {0.25f,0.75f,0.3f,0.0f}; check_vec("frac", frac(a), e, 1e-4f); }

    // ========== COMPARISON ==========

    { fvec a; a.set(0,1); a.set(1,2); a.set(2,3); a.set(3,4);
      fvec b(2.0f);
      check("lt[0]", (a < b).get(0) != 0);
      check("lt[1]", (a < b).get(1) == 0);
      check("le[1]", (a <= b).get(1) != 0);
      check("gt[2]", (a > b).get(2) != 0);
      check("ge[1]", (a >= b).get(1) != 0);
      check("eq[1]", (a == b).get(1) != 0);
      check("eq[0]", (a == b).get(0) == 0);
      check("ne[0]", (a != b).get(0) != 0);
      check("ne[1]", (a != b).get(1) == 0); }

    // ========== BITWISE ==========

    { // Use comparison masks as bit patterns
      fvec a(1.0f), b(2.0f);
      fvec mask_a = (a == a); // all ones
      fvec mask_b = (a > b);  // all zeros
      fvec r_and = mask_a & mask_b;
      fvec r_or  = mask_a | mask_b;
      fvec r_xor = mask_a ^ mask_b;
      // and(ones, zeros) = zeros, or(ones, zeros) = ones, xor(ones, zeros) = ones
      check("bitand[0]", r_and.get(0) == 0.0f);
      check("bitor[0]", r_or.get(0) != 0.0f);
      check("bitxor[0]", r_xor.get(0) != 0.0f); }

    // ========== SELECT ==========

    { fvec a(1.0f), b(2.0f);
      fvec mask; mask.set(0,0); mask.set(1,0); mask.set(2,0); mask.set(3,0);
      // mask all zeros → select a
      fvec r1 = select(a, b, mask);
      check("select_all_a", r1.get(0) == 1.0f);
      // mask all ones → select b
      fvec ones_mask = a == a;
      fvec r2 = select(a, b, ones_mask);
      check("select_all_b", r2.get(0) == 2.0f); }

    // ========== HORIZONTAL ==========

    { fvec v; v.set(0,1); v.set(1,5); v.set(2,3); v.set(3,7);
      check("h_min", v.horizontal_min() == 1.0f);
      check("h_max", v.horizontal_max() == 7.0f);
      check("h_sum", approx(v.horizontal_sum(), 16.0f)); }

    // ========== TRANSCENDENTALS (scalar cmath fallback) ==========

    { float vals[] = {0.5f, 1.0f, 1.5f, 2.0f};
      fvec a; for (int i = 0; i < 4; i++) a.set(i, vals[i]);

      #define CHECK_TRANSCENDENTAL(fn)                                    \
        { fvec r = fn(a);                                                 \
          for (int i = 0; i < 4; i++)                                     \
            check(#fn, approx(r.get(i), std::fn(vals[i]), 1e-5f)); }

      CHECK_TRANSCENDENTAL(exp)
      CHECK_TRANSCENDENTAL(log)
      CHECK_TRANSCENDENTAL(sin)
      CHECK_TRANSCENDENTAL(cos)
      CHECK_TRANSCENDENTAL(tan)
      CHECK_TRANSCENDENTAL(tanh)
      #undef CHECK_TRANSCENDENTAL
    }

    // asin/acos/atan need inputs in [-1,1] or similar
    { float vals[] = {-0.5f, 0.0f, 0.5f, 0.9f};
      fvec a; for (int i = 0; i < 4; i++) a.set(i, vals[i]);
      { fvec r = asin(a); for (int i = 0; i < 4; i++)
          check("asin", approx(r.get(i), std::asin(vals[i]), 1e-5f)); }
      { fvec r = acos(a); for (int i = 0; i < 4; i++)
          check("acos", approx(r.get(i), std::acos(vals[i]), 1e-5f)); }
      { fvec r = atan(a); for (int i = 0; i < 4; i++)
          check("atan", approx(r.get(i), std::atan(vals[i]), 1e-5f)); }
    }

    // pow
    { fvec a; a.set(0,2); a.set(1,3); a.set(2,4); a.set(3,5);
      fvec b; b.set(0,3); b.set(1,2); b.set(2,0.5f); b.set(3,1);
      fvec r = pow(a, b);
      float exp[] = {8.0f, 9.0f, 2.0f, 5.0f};
      check_vec("pow", r, exp, 1e-4f); }

    // log2, log10
    { fvec a; a.set(0,1); a.set(1,2); a.set(2,4); a.set(3,8);
      fvec r2 = log2(a);
      float e2[] = {0, 1, 2, 3};
      check_vec("log2", r2, e2, 1e-5f);

      fvec b; b.set(0,1); b.set(1,10); b.set(2,100); b.set(3,1000);
      fvec r10 = log10(b);
      float e10[] = {0, 1, 2, 3};
      check_vec("log10", r10, e10, 1e-4f); }

    // ========== MEMORY OPS (via load/store) ==========

    { float src[4] = {1.5f, 2.5f, 3.5f, 4.5f};
      float dst[4] = {};
      fvec v; v.load(src); v.store(dst);
      for (int i = 0; i < 4; i++) check("load_store_roundtrip", dst[i] == src[i]);

      // copy via load+store (this is what copyvec_simd does internally)
      float src2[8] = {10,20,30,40,50,60,70,80};
      float dst2[8] = {};
      fvec v1, v2;
      v1.load(src2); v1.store(dst2);
      v2.load(src2+4); v2.store(dst2+4);
      for (int i = 0; i < 8; i++) check("vec_copy_8", dst2[i] == src2[i]);
    }

    // ========== UNDENORMALIZE ==========

    { fvec v; v.set(0, 1e-40f); v.set(1, 0.5f); v.set(2, -1e-40f); v.set(3, 0.0f);
      fvec r = undenormalize(v);
      check("undenorm[0]", r.get(0) == 0.0f); // denormal → 0
      check("undenorm[1]", r.get(1) == 0.5f); // normal preserved
      check("undenorm[2]", r.get(2) == 0.0f); // negative denormal → 0
      check("undenorm[3]", r.get(3) == 0.0f); // zero preserved
    }

    // ========== EDGE CASES ==========

    // NaN propagation
    { float nan = std::numeric_limits<float>::quiet_NaN();
      fvec a(nan); fvec b(1.0f);
      fvec r = a + b;
      check("nan_add", r.get(0) != r.get(0)); // result is NaN
    }

    // Infinity
    { float inf = std::numeric_limits<float>::infinity();
      fvec a(inf); fvec b(1.0f);
      check("inf_add", std::isinf((a + b).get(0)));
      check("inf_neg", std::isinf((-a).get(0)) && (-a).get(0) < 0);
    }

    // Negative zero
    { fvec a(-0.0f);
      check("neg_zero_abs", abs(a).get(0) == 0.0f);
      check("neg_zero_sign", std::signbit(a.get(0))); // preserves -0
    }

    // ========== INTEGER VECTOR ==========

    { using ivec = nova::detail::int_vec_wasm128;
      ivec a(10); ivec b(3);
      ivec sum = a + b;
      ivec diff = a - b;
      // Extract via convert_to_float
      float sf = wasm_f32x4_extract_lane(sum.convert_to_float(), 0);
      float df = wasm_f32x4_extract_lane(diff.convert_to_float(), 0);
      check("ivec_add", sf == 13.0f);
      check("ivec_sub", df == 7.0f);

      // Shift
      ivec s = slli(ivec(1), 3); // 1 << 3 = 8
      float sv = wasm_f32x4_extract_lane(s.convert_to_float(), 0);
      check("ivec_slli", sv == 8.0f);

      ivec r = srli(ivec(16), 2); // 16 >> 2 = 4
      float rv = wasm_f32x4_extract_lane(r.convert_to_float(), 0);
      check("ivec_srli", rv == 4.0f);
    }

    // ========== SIGNED_POW ==========

    { fvec a; a.set(0,2); a.set(1,-2); a.set(2,3); a.set(3,-3);
      fvec b(2.0f);
      fvec r = signed_pow(a, b);
      float exp[] = {4.0f, -4.0f, 9.0f, -9.0f};
      check_vec("signed_pow", r, exp, 1e-4f); }

    // ========== FAST_RECIPROCAL ==========

    { fvec a; a.set(0,2); a.set(1,4); a.set(2,5); a.set(3,10);
      fvec r = fast_reciprocal(a);
      float exp[] = {0.5f, 0.25f, 0.2f, 0.1f};
      check_vec("fast_reciprocal", r, exp, 1e-4f); }

    // ========== MASK OPERATORS ==========

    { fvec a; a.set(0,1); a.set(1,2); a.set(2,3); a.set(3,4);
      fvec b(2.5f);
      fvec mlt = mask_lt(a, b);
      fvec mgt = mask_gt(a, b);
      fvec mle = mask_le(a, b);
      fvec mge = mask_ge(a, b);
      fvec meq = mask_eq(a, a);
      fvec mne = mask_neq(a, b);
      check("mask_lt[0]", mlt.get(0) != 0); // 1 < 2.5
      check("mask_lt[2]", mlt.get(2) == 0); // 3 < 2.5 false
      check("mask_gt[2]", mgt.get(2) != 0); // 3 > 2.5
      check("mask_gt[0]", mgt.get(0) == 0); // 1 > 2.5 false
      check("mask_le[1]", mle.get(1) != 0); // 2 <= 2.5
      check("mask_ge[2]", mge.get(2) != 0); // 3 >= 2.5
      check("mask_eq_self", meq.get(0) != 0);
      check("mask_neq[0]", mne.get(0) != 0); // 1 != 2.5
    }

    // ========== ANDNOT (float) ==========
    // andnot(a,b) = ~a & b (matches SSE _mm_andnot semantics)

    { fvec all_ones = fvec(1.0f) == fvec(1.0f); // all bits set
      fvec val(3.14f);
      // andnot(all_ones, val) = ~all_ones & val = 0
      fvec r1 = andnot(all_ones, val);
      check("andnot_ones_val", r1.get(0) == 0.0f);
      // andnot(zero, val) = ~zero & val = val (all bits pass through)
      fvec zero(0.0f);
      fvec r2 = andnot(zero, val);
      check("andnot_zero_val", r2.get(0) == 3.14f); }

    // ========== TRUNCATE_TO_INT ==========

    { fvec a; a.set(0,1.9f); a.set(1,-2.7f); a.set(2,3.1f); a.set(3,0.0f);
      auto iv = a.truncate_to_int();
      float f0 = wasm_f32x4_extract_lane(iv.convert_to_float(), 0);
      float f1 = wasm_f32x4_extract_lane(iv.convert_to_float(), 1);
      float f2 = wasm_f32x4_extract_lane(iv.convert_to_float(), 2);
      float f3 = wasm_f32x4_extract_lane(iv.convert_to_float(), 3);
      check("trunc_to_int[0]", f0 == 1.0f);
      check("trunc_to_int[1]", f1 == -2.0f);
      check("trunc_to_int[2]", f2 == 3.0f);
      check("trunc_to_int[3]", f3 == 0.0f); }

    // ========== GEN STATICS ==========

    { fvec z(fvec::gen_zero());
      check("gen_zero", z.get(0) == 0.0f && z.get(3) == 0.0f);
      fvec o(fvec::gen_one());
      check("gen_one", o.get(0) == 1.0f && o.get(3) == 1.0f);
      fvec h(fvec::gen_05());
      check("gen_05", h.get(0) == 0.5f && h.get(3) == 0.5f);
      // gen_ones: all bits set (NaN as float)
      fvec ones(fvec::gen_ones());
      check("gen_ones", ones.get(0) != ones.get(0)); // NaN
    }

    // ========== INT_VEC MASK OPS ==========

    { using ivec = nova::detail::int_vec_wasm128;
      ivec a(5); ivec b(3);
      ivec mgt = mask_gt(a, b);
      ivec mlt = mask_lt(a, b);
      ivec meq = mask_eq(a, a);
      // mgt should be all-ones (5 > 3)
      float gt_f = wasm_f32x4_extract_lane(mgt.convert_to_float(), 0);
      check("ivec_mask_gt", gt_f != 0);
      float lt_f = wasm_f32x4_extract_lane(mlt.convert_to_float(), 0);
      check("ivec_mask_lt", lt_f == 0); // 5 < 3 is false
      float eq_f = wasm_f32x4_extract_lane(meq.convert_to_float(), 0);
      check("ivec_mask_eq", eq_f != 0);

      // ivec AND
      ivec r_and = a & b;
      float and_f = wasm_f32x4_extract_lane(r_and.convert_to_float(), 0);
      check("ivec_and", and_f == (float)(5 & 3)); // 5&3=1

      // ivec andnot: andnot(a,b) = ~a & b
      ivec all(0xFFFFFFFF);
      ivec val(42);
      ivec r_andnot = andnot(all, val); // ~all & val = 0
      float an_f = wasm_f32x4_extract_lane(r_andnot.convert_to_float(), 0);
      check("ivec_andnot_ones", an_f == 0);
      ivec zero2(0);
      ivec r_an2 = andnot(zero2, val); // ~0 & val = val
      float an2_f = wasm_f32x4_extract_lane(r_an2.convert_to_float(), 0);
      check("ivec_andnot_zero", an2_f == 42.0f);
    }

    // ========== BUFFER-LEVEL SIMD OPS (simd_memory.hpp) ==========
    // Use size 128 (matching scsynth's mBufLength) — the unrolled SIMD
    // functions expect buffers large enough for their unroll factor.

    { const int N = 128;
      static float src[N], dst[N], a[N], b[N], clip_v[N];
      for (int i = 0; i < N; i++) { src[i] = (float)(i+1); a[i] = 1.0f; b[i] = 3.0f; clip_v[i] = 2.0f; }

      // copyvec_simd
      nova::copyvec_simd(dst, src, N);
      for (int i = 0; i < N; i++) check("copyvec_simd", dst[i] == src[i]);

      // zerovec_simd
      nova::zerovec_simd(dst, N);
      for (int i = 0; i < N; i++) check("zerovec_simd", dst[i] == 0.0f);

      // setvec_simd
      nova::setvec_simd(dst, 3.14f, N);
      for (int i = 0; i < N; i++) check("setvec_simd", approx(dst[i], 3.14f));

      // addvec_simd (dst += src)
      nova::setvec_simd(dst, 1.0f, N);
      nova::addvec_simd(dst, src, N);
      for (int i = 0; i < N; i++) check("addvec_simd", dst[i] == 1.0f + src[i]);

      // set_slope_vec_simd
      nova::set_slope_vec_simd(dst, 0.0f, 0.1f, N);
      for (int i = 0; i < N; i++)
        check("set_slope_vec", approx(dst[i], i * 0.1f, 1e-3f));

      // set_exp_vec_simd
      nova::set_exp_vec_simd(dst, 1.0f, 1.01f, N);
      float exp_val = 1.0f;
      for (int i = 0; i < N; i++) {
        check("set_exp_vec", approx(dst[i], exp_val, 1e-2f));
        exp_val *= 1.01f;
      }

      // clip2_vec_simd
      for (int i = 0; i < N; i++) src[i] = (float)(i - 64); // -64 to 63
      nova::clip2_vec_simd(dst, src, clip_v, N);
      for (int i = 0; i < N; i++) {
        float expected = src[i] > 2.0f ? 2.0f : (src[i] < -2.0f ? -2.0f : src[i]);
        check("clip2", approx(dst[i], expected));
      }

      // mix_vec_simd(out, sig0, factor0, sig1, factor1, n): out = sig0*factor0 + sig1*factor1
      nova::mix_vec_simd(dst, a, 0.5f, b, 0.5f, N);
      for (int i = 0; i < N; i++)
        check("mix_vec", approx(dst[i], 2.0f));
    }

    // ========== GEN BIT-MASK STATICS ==========

    { // gen_sign_mask should be 0x80000000 (sign bit only)
      v128_t sm = fvec::gen_sign_mask();
      uint32_t bits;
      memcpy(&bits, &sm, 4);
      check("gen_sign_mask", bits == 0x80000000);

      // gen_abs_mask should be 0x7FFFFFFF (all except sign)
      v128_t am = fvec::gen_abs_mask();
      memcpy(&bits, &am, 4);
      check("gen_abs_mask", bits == 0x7FFFFFFF);

      // gen_exp_mask should be 0x7F800000
      v128_t em = fvec::gen_exp_mask();
      memcpy(&bits, &em, 4);
      check("gen_exp_mask", bits == 0x7F800000);

      // gen_exp_mask_1 should be 0x3F000000
      v128_t em1 = fvec::gen_exp_mask_1();
      memcpy(&bits, &em1, 4);
      check("gen_exp_mask_1", bits == 0x3F000000);
    }

    // ========== IS_ALIGNED ==========

    { alignas(16) float aligned[4] = {};
      check("is_aligned_yes", fvec::is_aligned(aligned));
      // offset by 1 float (4 bytes) — not 16-byte aligned
      check("is_aligned_no", !fvec::is_aligned(aligned + 1));
    }

    printf("SIMD tests: %d tests, %d failures\n", tests, failures);
    return failures;
}

} // extern "C"
