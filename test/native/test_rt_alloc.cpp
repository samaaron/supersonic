/*
 * test_rt_alloc.cpp — RT-thread allocation detector hooks + tests
 *
 * Defines the global operator new/delete overrides that read the
 * thread-local flag from rt_alloc.h. The flag is set inside
 * process_audio() (always-on in production, immeasurable cost) and by
 * explicit Guards in tests below; allocations under the guard bump an
 * atomic counter that must stay at zero.
 *
 * Tests drive process_audio() directly on the test thread, so they call
 * fx.stopHeadlessDriver() first — otherwise the autonomous audio thread
 * would be racing the test thread on every process_audio() invocation
 * (TSan flagged this on Linux where scheduling actually overlaps).
 *
 * Catches operator new/delete only — bare malloc from C code isn't
 * intercepted. scsynth's RT path goes through new in supersonic_heap,
 * so most audio-path allocations are covered.
 */

#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "rt_alloc.h"
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstdint>
#include <new>

extern "C" {
    bool process_audio(double current_time, uint32_t active_output_channels,
                       uint32_t active_input_channels);
}

// ─── Allocation hooks ──────────────────────────────────────────────────────

static void* rt_new(std::size_t n) {
    if (rt_alloc::g_in_rt) rt_alloc::g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
static void rt_delete(void* p) noexcept {
    if (p && rt_alloc::g_in_rt) rt_alloc::g_frees.fetch_add(1, std::memory_order_relaxed);
    std::free(p);
}

void* operator new(std::size_t n)                                   { return rt_new(n); }
void* operator new[](std::size_t n)                                 { return rt_new(n); }
void* operator new(std::size_t n,   const std::nothrow_t&) noexcept { try { return rt_new(n); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { try { return rt_new(n); } catch (...) { return nullptr; } }
void  operator delete(void* p)                noexcept              { rt_delete(p); }
void  operator delete[](void* p)              noexcept              { rt_delete(p); }
void  operator delete(void* p, std::size_t)   noexcept              { rt_delete(p); }
void  operator delete[](void* p, std::size_t) noexcept              { rt_delete(p); }
void  operator delete(void* p,   const std::nothrow_t&) noexcept    { rt_delete(p); }
void  operator delete[](void* p, const std::nothrow_t&) noexcept    { rt_delete(p); }

// ─── Synthdef coverage matrix ──────────────────────────────────────────────
//
// Each entry exercises a different UGen family. If a UGen Ctor or Calc
// function does a stray new/delete, the test catches it the first time
// that UGen runs.

namespace {

// Synth UGens: oscillators, filters, FM, granular, formant, etc.
constexpr const char* kSynthDefs[] = {
    "sonic-pi-beep",            // SinOsc + EnvGen
    "sonic-pi-dsaw",            // detuned Saw stack
    "sonic-pi-dpulse",          // detuned Pulse
    "sonic-pi-dtri",            // detuned Tri
    "sonic-pi-fm",              // FM (SinOsc with feedback)
    "sonic-pi-prophet",         // multi-osc + LPF + LFO
    "sonic-pi-hollow",          // resonant filter chain
    "sonic-pi-blade",           // square wave stack
    "sonic-pi-chiplead",        // chiptune square
    "sonic-pi-bass_foundation", // bass stack
    "sonic-pi-dull_bell",       // SinOsc bank (additive)
};

// FX UGens: dynamics, EQ, time-domain, distortion. These often have
// large delay buffers / coefficient tables initialised at /s_new time.
constexpr const char* kFxDefs[] = {
    "sonic-pi-fx_compressor",
    "sonic-pi-fx_gverb",        // GVerb — large reverb buffers
    "sonic-pi-fx_distortion",
    "sonic-pi-fx_bitcrusher",
    "sonic-pi-fx_band_eq",
    "sonic-pi-fx_lpf",
    "sonic-pi-fx_hpf",
    "sonic-pi-fx_echo",         // delay line
    "sonic-pi-fx_flanger",
};

osc_test::Packet sNewSustained(const char* def, int32_t id,
                               int32_t addAction = 0, int32_t target = 1) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << def << id << addAction << target << "sustain" << 3600.0f;
    return b.end();
}

struct AllocSnapshot { int64_t allocs; int64_t frees; };

AllocSnapshot runGuarded(int blocks, double startNTP = 4'000'000'000.0) {
    constexpr double blockSecs = 128.0 / 48000.0;
    double ntp = startNTP;

    rt_alloc::reset();
    {
        rt_alloc::Guard g;
        for (int i = 0; i < blocks; ++i) {
            process_audio(ntp, 2, 0);
            ntp += blockSecs;
        }
    }
    return { rt_alloc::g_allocs.load(std::memory_order_relaxed),
             rt_alloc::g_frees.load(std::memory_order_relaxed) };
}

void warmup(int blocks = 200) {
    constexpr double startNTP  = 3'000'000'000.0;
    constexpr double blockSecs = 128.0 / 48000.0;
    double ntp = startNTP;
    for (int i = 0; i < blocks; ++i) {
        process_audio(ntp, 2, 0);
        ntp += blockSecs;
    }
}

} // namespace

// ─── Tests ─────────────────────────────────────────────────────────────────

// Noinline shims so the compiler can't see the new/delete pair across the
// function boundary and elide it. C++14 [expr.new]/p10 allows compilers to
// omit replaceable allocation calls when the result is never observably
// used; GCC's RelWithDebInfo build does this aggressively, Apple clang
// does not. Without these shims the self-test passes locally and fails on
// Linux CI because the allocation never happens.
#if defined(__clang__) || defined(__GNUC__)
__attribute__((noinline))
#endif
static int* rt_alloc_self_test_alloc() { return new int(42); }

#if defined(__clang__) || defined(__GNUC__)
__attribute__((noinline))
#endif
static void rt_alloc_self_test_free(int* p) { delete p; }

TEST_CASE("RT-alloc: detector counts allocations under guard", "[rt_alloc]") {
    // Self-test — guards against silent breakage of the hooks.
    rt_alloc::reset();
    {
        rt_alloc::Guard g;
        int* p = rt_alloc_self_test_alloc();
        rt_alloc_self_test_free(p);
    }
    CHECK(rt_alloc::g_allocs.load() >= 1);
    CHECK(rt_alloc::g_frees.load() >= 1);
}

TEST_CASE("RT-alloc: empty world", "[rt_alloc]") {
    EngineFixture fx;
    fx.stopHeadlessDriver();
    warmup();

    auto snap = runGuarded(2000);
    INFO("allocs=" << snap.allocs << " frees=" << snap.frees);
    CHECK(snap.allocs == 0);
    CHECK(snap.frees == 0);
}

TEST_CASE("RT-alloc: variety of synths in steady state", "[rt_alloc]") {
    EngineFixture fx;
    for (auto def : kSynthDefs) REQUIRE(fx.loadSynthDef(def));

    int32_t id = 1000;
    for (auto def : kSynthDefs) {
        fx.send(sNewSustained(def, id++));
    }

    fx.stopHeadlessDriver();
    warmup();

    auto snap = runGuarded(2000);
    INFO("synths=" << std::size(kSynthDefs) << " allocs=" << snap.allocs << " frees=" << snap.frees);
    CHECK(snap.allocs == 0);
    CHECK(snap.frees == 0);

    for (int32_t i = 1000; i < (int32_t)(1000 + std::size(kSynthDefs)); ++i) {
        fx.send(osc_test::message("/n_free", i));
    }
}

TEST_CASE("RT-alloc: variety of FX in steady state", "[rt_alloc]") {
    EngineFixture fx;
    for (auto def : kFxDefs) REQUIRE(fx.loadSynthDef(def));

    int32_t id = 1100;
    for (auto def : kFxDefs) {
        fx.send(sNewSustained(def, id++));
    }

    fx.stopHeadlessDriver();
    warmup();

    auto snap = runGuarded(2000);
    INFO("fx=" << std::size(kFxDefs) << " allocs=" << snap.allocs << " frees=" << snap.frees);
    CHECK(snap.allocs == 0);
    CHECK(snap.frees == 0);

    for (int32_t i = 1100; i < (int32_t)(1100 + std::size(kFxDefs)); ++i) {
        fx.send(osc_test::message("/n_free", i));
    }
}

TEST_CASE("RT-alloc: many synths spanning multiple types", "[rt_alloc]") {
    EngineFixture fx;
    for (auto def : kSynthDefs) REQUIRE(fx.loadSynthDef(def));

    constexpr int N = 32;
    int32_t id = 1200;
    for (int i = 0; i < N; ++i) {
        const char* def = kSynthDefs[i % std::size(kSynthDefs)];
        fx.send(sNewSustained(def, id++));
    }

    fx.stopHeadlessDriver();
    warmup();

    auto snap = runGuarded(2000);
    INFO("synths=" << N << " allocs=" << snap.allocs << " frees=" << snap.frees);
    CHECK(snap.allocs == 0);
    CHECK(snap.frees == 0);

    for (int32_t i = 1200; i < (int32_t)(1200 + N); ++i) {
        fx.send(osc_test::message("/n_free", i));
    }
}

TEST_CASE("RT-alloc: synth construction inside guarded callback", "[rt_alloc]") {
    // The /s_new is processed (and the synth's UGen Ctors fire) WITHIN the
    // guarded process_audio call — this is where stray-new bugs tend to
    // hide, since UGen Ctors can call into helper code that allocates.
    EngineFixture fx;
    for (auto def : kSynthDefs) REQUIRE(fx.loadSynthDef(def));

    fx.stopHeadlessDriver();
    warmup();

    // Queue all /s_new before entering the guard. process_audio under the
    // guard will drain them, constructing each synth there.
    int32_t id = 1300;
    for (auto def : kSynthDefs) {
        fx.send(sNewSustained(def, id++));
    }

    // Need enough cycles to drain the IN ring buffer and run construction.
    auto snap = runGuarded(200);
    INFO("synths=" << std::size(kSynthDefs) << " allocs=" << snap.allocs << " frees=" << snap.frees);
    CHECK(snap.allocs == 0);
    CHECK(snap.frees == 0);

    for (int32_t i = 1300; i < (int32_t)(1300 + std::size(kSynthDefs)); ++i) {
        fx.send(osc_test::message("/n_free", i));
    }
}

TEST_CASE("RT-alloc: FX construction inside guarded callback", "[rt_alloc]") {
    // GVerb in particular allocates large reverb buffers via RTAlloc;
    // this catches it if RTAlloc ever falls through to system new.
    EngineFixture fx;
    for (auto def : kFxDefs) REQUIRE(fx.loadSynthDef(def));

    fx.stopHeadlessDriver();
    warmup();

    int32_t id = 1400;
    for (auto def : kFxDefs) {
        fx.send(sNewSustained(def, id++));
    }

    auto snap = runGuarded(200);
    INFO("fx=" << std::size(kFxDefs) << " allocs=" << snap.allocs << " frees=" << snap.frees);
    CHECK(snap.allocs == 0);
    CHECK(snap.frees == 0);

    for (int32_t i = 1400; i < (int32_t)(1400 + std::size(kFxDefs)); ++i) {
        fx.send(osc_test::message("/n_free", i));
    }
}

TEST_CASE("RT-alloc: synth /n_free inside guarded callback", "[rt_alloc]") {
    // /n_free runs the synth's Dtor, which may free RTAlloc'd memory —
    // through the world's pool, not the system heap.
    EngineFixture fx;
    for (auto def : kSynthDefs) REQUIRE(fx.loadSynthDef(def));

    int32_t id = 1500;
    for (auto def : kSynthDefs) {
        fx.send(sNewSustained(def, id++));
    }

    fx.stopHeadlessDriver();
    warmup();

    // Queue all /n_free before guard.
    for (int32_t i = 1500; i < (int32_t)(1500 + std::size(kSynthDefs)); ++i) {
        fx.send(osc_test::message("/n_free", i));
    }

    auto snap = runGuarded(200);
    INFO("freed=" << std::size(kSynthDefs) << " allocs=" << snap.allocs << " frees=" << snap.frees);
    CHECK(snap.allocs == 0);
    CHECK(snap.frees == 0);
}
