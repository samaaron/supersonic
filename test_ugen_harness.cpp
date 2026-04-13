/**
 * Native UGen unit test harness
 *
 * Directly instantiates and calls UGen processing functions to verify
 * their behavior in isolation. No synthdefs, no OSC, no audio graph.
 *
 * Compile:
 *   g++ -std=c++17 -O2 \
 *     -I src/scsynth/include/plugin_interface \
 *     -I src/scsynth/include/common \
 *     -I src/scsynth/include/server \
 *     -DNDEBUG \
 *     -o test_ugen_harness test_ugen_harness.cpp
 */

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>

// ── Minimal SC type stubs ────────────────────────────────────────────────────
// We only need enough to make Unit, Rate, Wire, and the UGen functions compile.

#include "SC_Types.h"
#include "SC_Rate.h"
#include "SC_BoundsMacros.h"

// Stub Wire
struct Wire {
    float* mFromUnit;
    int32  mCalcRate;
    float  mScalarValue;
    float  mBuffer[1]; // placeholder
};

// Stub World (UGens that call RTAlloc need this)
struct World {
    void* hw;
    float mSampleRate;
};

// Forward-declare what we need from SC_Unit.h manually to avoid pulling in everything
struct UnitDef;
struct Graph;
struct SC_Unit_Extensions;

struct Unit {
    World* mWorld;
    UnitDef* mUnitDef;
    Graph* mParent;
    uint32 mNumInputs, mNumOutputs;
    int16 mCalcRate;
    int16 mSpecialIndex;
    int16 mParentIndex;
    int16 mDone;
    Wire **mInput, **mOutput;
    Rate* mRate;
    SC_Unit_Extensions* mExtensions;
    float **mInBuf, **mOutBuf;
    void (*mCalcFunc)(Unit*, int32);
    int32 mBufLength;
};

typedef void (*UnitCalcFunc)(Unit* inUnit, int32 inNumSamples);
#define SETCALC(func) (unit->mCalcFunc = (UnitCalcFunc)func)
#define IN(index) (unit->mInBuf[index])
#define OUT(index) (unit->mOutBuf[index])
#define IN0(index) (IN(index)[0])
#define OUT0(index) (OUT(index)[0])
#define SAMPLERATE (unit->mRate->mSampleRate)

// ZOFF is the pointer offset trick SC uses for its LOOP macros
#define ZOFF (1)
#define ZIN(i) (IN(i) - ZOFF)
#define ZOUT(i) (OUT(i) - ZOFF)
#define ZIN0(i) (IN(i)[0])
#define ZOUT0(i) (OUT(i)[0])
#define ZXP(z) (*(++(z)))

// Loop macros from Unroll.h
#define LOOP1(n, body) { int zz = (n); while(zz--) { body; } }
#define LOOP(n, body) { int zz = (n); while(zz--) { body; } }

// zapgremlins: suppress denormals
static inline float zapgremlins(double x) {
    float ax = std::abs(x);
    if (ax < (float)1e-15 || ax > (float)1e15) return 0.f;
    return (float)x;
}

// uninitializedControl sentinel
static const float uninitializedControl = -1e30f;

// CALCSLOPE
#define CALCSLOPE(next, cur) ((next - cur) * unit->mRate->mSlopeFactor)

// ClearUnitIfMemFailed - stub
#define ClearUnitIfMemFailed(ptr) if(!(ptr)) return

// RTAlloc/RTFree stubs
static void* RTAlloc(World*, size_t sz) { return malloc(sz); }
static void  RTFree(World*, void* p) { free(p); }

// ── Inline the actual UGen structs and functions ─────────────────────────────

// Clip UGen
struct Clip : public Unit {
    float m_lo, m_hi;
};

// RLPF UGen
struct RLPF : public Unit {
    float m_freq, m_reson;
    double m_y1, m_y2, m_a0, m_b1, m_b2;
};

// Clip_next_k: control-rate Clip (1 sample)
void Clip_next_k(Clip* unit, int inNumSamples) {
    float* out = ZOUT(0);
    float* in = ZIN(0);
    float lo = ZIN0(1);
    float hi = ZIN0(2);
    ZXP(out) = sc_clip(ZXP(in), lo, hi);
}

// Clip_Ctor
void Clip_next_ii(Clip* unit, int inNumSamples) {
    float* out = ZOUT(0);
    float* in = ZIN(0);
    float lo = unit->m_lo;
    float hi = unit->m_hi;
    LOOP1(inNumSamples, ZXP(out) = sc_clip(ZXP(in), lo, hi););
}

void Clip_Ctor(Clip* unit) {
    unit->mCalcFunc = (UnitCalcFunc)Clip_next_k;  // simplified
    unit->m_lo = ZIN0(1);
    unit->m_hi = ZIN0(2);
    Clip_next_ii(unit, 1);
}

// RLPF_next_1: scalar RLPF (1 sample at a time)
void RLPF_next_1(RLPF* unit, int inNumSamples) {
    float in = ZIN0(0);
    float freq = ZIN0(1);
    float reson = ZIN0(2);

    double y0;
    double y1 = unit->m_y1;
    double y2 = unit->m_y2;
    double a0 = unit->m_a0;
    double b1 = unit->m_b1;
    double b2 = unit->m_b2;

    if (freq != unit->m_freq || reson != unit->m_reson) {
        double qres = sc_max(0.001f, reson);
        double pfreq = freq * unit->mRate->mRadiansPerSample;

        double D = tan(pfreq * qres * 0.5);
        double C = ((1.0 - D) / (1.0 + D));
        double cosf = cos(pfreq);

        b1 = (1.0 + C) * cosf;
        b2 = -C;
        a0 = (1.0 + C - b1) * .25;

        y0 = a0 * in + b1 * y1 + b2 * y2;
        ZOUT0(0) = y0 + 2.0 * y1 + y2;
        y2 = y1;
        y1 = y0;

        unit->m_freq = freq;
        unit->m_reson = reson;
        unit->m_a0 = a0;
        unit->m_b1 = b1;
        unit->m_b2 = b2;
    } else {
        y0 = a0 * in + b1 * y1 + b2 * y2;
        ZOUT0(0) = y0 + 2.0 * y1 + y2;
        y2 = y1;
        y1 = y0;
    }
    unit->m_y1 = zapgremlins(y1);
    unit->m_y2 = zapgremlins(y2);
}

void RLPF_Ctor(RLPF* unit) {
    SETCALC(RLPF_next_1);
    unit->m_a0 = 0.f;
    unit->m_b1 = 0.f;
    unit->m_b2 = 0.f;
    unit->m_y1 = 0.f;
    unit->m_y2 = 0.f;
    unit->m_freq = uninitializedControl;
    unit->m_reson = uninitializedControl;
    RLPF_next_1(unit, 1);
}

// ── Test harness helpers ─────────────────────────────────────────────────────

struct UGenTestBed {
    Rate rate;
    World world;
    std::vector<float*> inBufs;
    std::vector<float*> outBufs;
    std::vector<Wire*> inWires;
    std::vector<Wire*> outWires;

    void initRate(double sampleRate, int bufLen) {
        rate.mSampleRate = sampleRate;
        rate.mSampleDur = 1.0 / sampleRate;
        rate.mRadiansPerSample = 2.0 * M_PI / sampleRate;
        rate.mBufLength = bufLen;
        rate.mBufDuration = bufLen / sampleRate;
        rate.mBufRate = sampleRate / bufLen;
        rate.mSlopeFactor = 1.0 / bufLen;
        rate.mFilterLoops = (bufLen / 3);
        rate.mFilterRemain = bufLen - (rate.mFilterLoops * 3);
        rate.mFilterSlope = 1.0 / bufLen;
    }

    // Set up a Unit with N inputs and M outputs
    void setupUnit(Unit* u, int numIn, int numOut, size_t structSize = sizeof(Unit)) {
        memset(u, 0, structSize);
        u->mRate = &rate;
        u->mWorld = &world;
        u->mNumInputs = numIn;
        u->mNumOutputs = numOut;
        u->mBufLength = 1;  // control rate = 1 sample
        u->mCalcRate = calc_BufRate;

        inBufs.resize(numIn);
        outBufs.resize(numOut);
        inWires.resize(numIn);
        outWires.resize(numOut);

        for (int i = 0; i < numIn; i++) {
            // +1 for ZOFF indexing
            inBufs[i] = new float[2]();
            inWires[i] = new Wire();
        }
        for (int i = 0; i < numOut; i++) {
            outBufs[i] = new float[2]();
            outWires[i] = new Wire();
        }

        u->mInBuf = inBufs.data();
        u->mOutBuf = outBufs.data();
        u->mInput = inWires.data();
        u->mOutput = outWires.data();
    }

    void cleanup() {
        for (auto p : inBufs) delete[] p;
        for (auto p : outBufs) delete[] p;
        for (auto p : inWires) delete p;
        for (auto p : outWires) delete p;
        inBufs.clear(); outBufs.clear(); inWires.clear(); outWires.clear();
    }
};

// ── Tests ────────────────────────────────────────────────────────────────────

int failures = 0;

void CHECK(bool cond, const char* msg) {
    if (!cond) { printf("  FAIL: %s\n", msg); failures++; }
    else       { printf("  PASS: %s\n", msg); }
}

void test_clip_normal_bounds() {
    printf("\n=== Test: Clip UGen with normal bounds (lo=0, hi=1) ===\n");
    UGenTestBed bed;
    bed.initRate(48000, 1);

    Clip clip;
    bed.setupUnit((Unit*)&clip, 3, 1, sizeof(Clip));

    for (float x : {-0.5f, 0.0f, 0.3f, 0.7f, 1.0f, 1.5f}) {
        bed.inBufs[0][0] = x;    // signal
        bed.inBufs[1][0] = 0.0f; // lo
        bed.inBufs[2][0] = 1.0f; // hi
        Clip_Ctor(&clip);
        float result = bed.outBufs[0][0];
        float expected = std::max(0.0f, std::min(x, 1.0f));
        printf("  Clip(%.2f, 0, 1) = %.4f (expected %.4f)\n", x, result, expected);
        CHECK(std::abs(result - expected) < 1e-6f, "normal bounds correct");
    }
    bed.cleanup();
}

void test_clip_inverted_bounds() {
    printf("\n=== Test: Clip UGen with INVERTED bounds (lo=1, hi=0) ===\n");
    printf("  This is what linlin(1, 0, 0, 1) generates in compiled synthdefs\n");
    UGenTestBed bed;
    bed.initRate(48000, 1);

    Clip clip;
    bed.setupUnit((Unit*)&clip, 3, 1, sizeof(Clip));

    for (float x : {0.01f, 0.3f, 0.5f, 0.7f, 0.99f}) {
        bed.inBufs[0][0] = x;    // signal
        bed.inBufs[1][0] = 1.0f; // lo (inverted!)
        bed.inBufs[2][0] = 0.0f; // hi (inverted!)
        Clip_Ctor(&clip);
        float result = bed.outBufs[0][0];
        // What we WANT: clip to [0, 1] = x unchanged (since 0 <= x <= 1)
        // What we GET: max(min(x, 0), 1) = 1.0 always
        printf("  Clip(%.2f, lo=1, hi=0) = %.4f  (want: %.4f for correct linlin)\n",
               x, result, x);
        // Check current (broken?) behavior
        if (std::abs(result - 1.0f) < 1e-6f) {
            printf("    → always returns lo=1.0 regardless of input\n");
        }
    }
    // The overall check: does the res parameter actually matter?
    bed.inBufs[0][0] = 0.7f; bed.inBufs[1][0] = 1.0f; bed.inBufs[2][0] = 0.0f;
    Clip_Ctor(&clip);
    float r1 = bed.outBufs[0][0];
    bed.inBufs[0][0] = 0.01f;
    Clip_Ctor(&clip);
    float r2 = bed.outBufs[0][0];
    CHECK(std::abs(r1 - r2) > 0.1f,
          "Clip should return different values for different inputs with inverted bounds");
    bed.cleanup();
}

void test_rlpf_rq_effect() {
    printf("\n=== Test: RLPF resonance varies with rq ===\n");
    UGenTestBed bed;
    bed.initRate(48000, 1);

    float freq = 2637.0f; // cutoff=100 MIDI in Hz

    // Feed a broadband signal (white noise-ish via simple hash) through RLPF
    // and measure output energy at different rq values
    for (float rq : {0.001f, 0.01f, 0.1f, 0.3f, 0.7f, 0.99f}) {
        RLPF rlpf;
        bed.setupUnit((Unit*)&rlpf, 3, 1, sizeof(RLPF));

        // Set freq and rq
        bed.inBufs[1][0] = freq;
        bed.inBufs[2][0] = rq;

        // Init
        bed.inBufs[0][0] = 0.0f;
        RLPF_Ctor(&rlpf);

        // Process 4800 samples (0.1s) of impulse-like input
        double energy = 0;
        double peak = 0;
        for (int i = 0; i < 4800; i++) {
            // Input: impulse at sample 0, then silence
            bed.inBufs[0][0] = (i == 0) ? 1.0f : 0.0f;
            bed.inBufs[1][0] = freq;
            bed.inBufs[2][0] = rq;
            RLPF_next_1(&rlpf, 1);
            float out = bed.outBufs[0][0];
            energy += out * out;
            if (std::abs(out) > peak) peak = std::abs(out);
        }
        printf("  rq=%.3f: energy=%.6f peak=%.6f\n", rq, energy, peak);
        bed.cleanup();
    }

    // Key test: rq=0.001 should ring much longer (more energy) than rq=0.99
    // Re-run just two extremes
    double energy_narrow, energy_wide;
    for (int pass = 0; pass < 2; pass++) {
        float rq = (pass == 0) ? 0.001f : 0.99f;
        RLPF rlpf;
        bed.setupUnit((Unit*)&rlpf, 3, 1, sizeof(RLPF));
        bed.inBufs[1][0] = freq;
        bed.inBufs[2][0] = rq;
        bed.inBufs[0][0] = 0.0f;
        RLPF_Ctor(&rlpf);
        double energy = 0;
        for (int i = 0; i < 4800; i++) {
            bed.inBufs[0][0] = (i == 0) ? 1.0f : 0.0f;
            bed.inBufs[1][0] = freq;
            bed.inBufs[2][0] = rq;
            RLPF_next_1(&rlpf, 1);
            float out = bed.outBufs[0][0];
            energy += out * out;
        }
        if (pass == 0) energy_narrow = energy; else energy_wide = energy;
        bed.cleanup();
    }
    float ratio = energy_narrow / (energy_wide > 0 ? energy_wide : 1e-10);
    printf("  Energy ratio narrow/wide (rq=0.001 / rq=0.99): %.2f\n", ratio);
    CHECK(ratio > 5.0f, "Narrow rq should ring much longer than wide rq");
}

void test_rlpf_with_broken_clip() {
    printf("\n=== Test: RLPF with Clip-produced rq (simulating tech_saws) ===\n");
    UGenTestBed bed;
    bed.initRate(48000, 1);

    float freq = 2637.0f;

    // Simulate the tech_saws chain: Clip(res, 1.0, 0.0) → 1.0 - result → rq
    for (float res : {0.01f, 0.3f, 0.7f, 0.99f}) {
        // What Clip(res, lo=1.0, hi=0.0) produces:
        float clipped = sc_clip(res, 1.0f, 0.0f);
        float rq = 1.0f - clipped;

        RLPF rlpf;
        bed.setupUnit((Unit*)&rlpf, 3, 1, sizeof(RLPF));
        bed.inBufs[1][0] = freq;
        bed.inBufs[2][0] = rq;
        bed.inBufs[0][0] = 0.0f;
        RLPF_Ctor(&rlpf);

        double energy = 0;
        for (int i = 0; i < 4800; i++) {
            bed.inBufs[0][0] = (i == 0) ? 1.0f : 0.0f;
            bed.inBufs[1][0] = freq;
            bed.inBufs[2][0] = rq;
            RLPF_next_1(&rlpf, 1);
            energy += bed.outBufs[0][0] * bed.outBufs[0][0];
        }
        printf("  res=%.2f → Clip=%.4f → rq=%.4f → RLPF energy=%.6f\n",
               res, clipped, rq, energy);
        bed.cleanup();
    }

    // Now with CORRECT clip (swap lo/hi):
    printf("\n  With corrected Clip (lo/hi swapped when inverted):\n");
    for (float res : {0.01f, 0.3f, 0.7f, 0.99f}) {
        float lo = 1.0f, hi = 0.0f;
        float corrected_lo = std::min(lo, hi);
        float corrected_hi = std::max(lo, hi);
        float clipped = sc_clip(res, corrected_lo, corrected_hi);
        float rq = 1.0f - clipped;

        RLPF rlpf;
        bed.setupUnit((Unit*)&rlpf, 3, 1, sizeof(RLPF));
        bed.inBufs[1][0] = freq;
        bed.inBufs[2][0] = rq;
        bed.inBufs[0][0] = 0.0f;
        RLPF_Ctor(&rlpf);

        double energy = 0;
        for (int i = 0; i < 4800; i++) {
            bed.inBufs[0][0] = (i == 0) ? 1.0f : 0.0f;
            bed.inBufs[1][0] = freq;
            bed.inBufs[2][0] = rq;
            RLPF_next_1(&rlpf, 1);
            energy += bed.outBufs[0][0] * bed.outBufs[0][0];
        }
        printf("  res=%.2f → Clip=%.4f → rq=%.4f → RLPF energy=%.6f\n",
               res, clipped, rq, energy);
        bed.cleanup();
    }
}

int main() {
    printf("SuperSonic UGen Unit Test Harness\n");
    printf("=================================\n");

    test_clip_normal_bounds();
    test_clip_inverted_bounds();
    test_rlpf_rq_effect();
    test_rlpf_with_broken_clip();

    printf("\n=================================\n");
    printf("Failures: %d\n", failures);
    return failures > 0 ? 1 : 0;
}
