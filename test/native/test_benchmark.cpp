/*
 * test_benchmark.cpp — Measure process_audio throughput on native
 *
 * Boots the engine in headless mode, stops the HeadlessDriver's real-time
 * tick loop, then calls process_audio() in a tight loop to measure pure
 * DSP throughput without timer jitter.
 *
 * Run with:  ./SuperSonicNativeTests "[benchmark]" 2>&1
 *
 * All times use std::chrono::steady_clock for nanosecond precision.
 * Results are displayed in both nanoseconds and microseconds.
 * "Headroom" is how many times faster than real-time: 10x means the DSP
 * uses 10% of the available budget.
 */
#include "EngineFixture.h"
#include "SupersonicEngine.h"
#include <cstdio>
#include <algorithm>
#include <vector>
#include <numeric>
#include <cmath>
#include <chrono>
#include <thread>

extern "C" {
    bool process_audio(double current_time, uint32_t active_output_channels,
                       uint32_t active_input_channels);
}

// ---------------------------------------------------------------------------
// Nanosecond clock helper
// ---------------------------------------------------------------------------

static inline int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static osc_test::Packet sNewWithParam(const char* def, int32_t id,
                                       int32_t addAction, int32_t target,
                                       const char* param, float value) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << def << id << addAction << target << param << value;
    return b.end();
}

// Create a synth with a long sustain so it stays alive for the entire benchmark.
// sonic-pi-beep default envelope: attack=0, sustain=0, release=1 = ~375 blocks.
// We set sustain=3600 (1 hour) to keep synths alive indefinitely.
static osc_test::Packet sNewSustained(const char* def, int32_t id,
                                       int32_t addAction, int32_t target,
                                       const char* param, float value) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << def << id << addAction << target << param << value << "sustain" << 3600.0f;
    return b.end();
}

// ---------------------------------------------------------------------------
// CPU frequency helpers — ensure consistent measurement on governors like
// "ondemand" that scale frequency with load (e.g. RPi4: 600MHz→1500MHz)
// ---------------------------------------------------------------------------

static int readCpuFreqKHz() {
#ifdef __linux__
    FILE* f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    if (!f) return -1;
    int khz = 0;
    if (fscanf(f, "%d", &khz) != 1) khz = -1;
    fclose(f);
    return khz;
#else
    return -1;  // CPU frequency reading only available on Linux
#endif
}

// Brief busy-spin to nudge CPU governor to ramp up frequency.
// Keep it short (20ms) to avoid triggering thermal throttling.
static void spinUpCpu() {
    volatile double x = 1.0;
    int64_t deadline = nowNs() + 20'000'000LL;  // 20ms
    while (nowNs() < deadline) {
        for (int i = 0; i < 1000; i++) x *= 1.0000001;
    }
}

// ---------------------------------------------------------------------------
// Benchmark runner — collects per-block timings for statistical analysis
// ---------------------------------------------------------------------------

struct BenchResult {
    double avgNs;
    double medianNs;
    double p95Ns;
    double p99Ns;
    double maxNs;
    double minNs;
    double stddevNs;
    double budgetNs;
    int    numBlocks;
};

static BenchResult runBenchmark(const char* label, int numBlocks, int warmupBlocks = 500) {
    const double budgetNs = (128.0 / 48000.0) * 1e9;  // ~2,666,667 ns

    double fakeNTP = 4000000000.0;
    const double blockIncrement = 128.0 / 48000.0;

    // Warmup — let caches settle and CPU frequency stabilise
    for (int i = 0; i < warmupBlocks; i++) {
        process_audio(fakeNTP, 2, 0);
        fakeNTP += blockIncrement;
    }

    // Measure individual block times in nanoseconds
    std::vector<double> timings(numBlocks);

    for (int i = 0; i < numBlocks; i++) {
        int64_t t0 = nowNs();
        process_audio(fakeNTP, 2, 0);
        int64_t t1 = nowNs();
        timings[i] = (double)(t1 - t0);
        fakeNTP += blockIncrement;
    }

    // Statistics
    std::sort(timings.begin(), timings.end());
    double sum = std::accumulate(timings.begin(), timings.end(), 0.0);
    double avg = sum / numBlocks;

    double sqSum = 0;
    for (double t : timings) sqSum += (t - avg) * (t - avg);
    double stddev = std::sqrt(sqSum / numBlocks);

    BenchResult r;
    r.avgNs     = avg;
    r.medianNs  = timings[numBlocks / 2];
    r.p95Ns     = timings[(int)(numBlocks * 0.95)];
    r.p99Ns     = timings[(int)(numBlocks * 0.99)];
    r.maxNs     = timings.back();
    r.minNs     = timings.front();
    r.stddevNs  = stddev;
    r.budgetNs  = budgetNs;
    r.numBlocks = numBlocks;

    double avgUs    = avg / 1000.0;
    double headroom = budgetNs / avg;
    double util     = (avg / budgetNs) * 100.0;

    fprintf(stderr,
        "\n  %-40s  %8.0f ns  (%5.1f us)  %5.2f%% util  %5.1fx RT"
        "  (med %.0f  p95 %.0f  p99 %.0f  max %.0f  sd %.0f ns)\n",
        label, avg, avgUs, util, headroom,
        r.medianNs, r.p95Ns, r.p99Ns, r.maxNs, stddev);

    return r;
}

// ---------------------------------------------------------------------------
// Stop the HeadlessDriver so we own process_audio exclusively
// ---------------------------------------------------------------------------

static void stopHeadlessDriver(EngineFixture& fx) {
    fx.stopHeadlessDriver();
}

// Manually tick the engine to process pending OSC messages
static void tickBlocks(int n) {
    double fakeNTP = 4500000000.0;
    const double inc = 128.0 / 48000.0;
    for (int i = 0; i < n; i++) {
        process_audio(fakeNTP, 2, 0);
        fakeNTP += inc;
    }
}

// ---------------------------------------------------------------------------
// Verify synths are alive by comparing block cost against idle baseline.
// A block with live synths takes measurably longer than an idle block.
// This is more reliable than /status when the HeadlessDriver is stopped
// (ReplyReader thread needs processCount notifications to drain replies).
// ---------------------------------------------------------------------------

static double measureBlockCostNs(int numBlocks = 200) {
    double fakeNTP = 4600000000.0;
    const double inc = 128.0 / 48000.0;
    // Small warmup
    for (int i = 0; i < 50; i++) { process_audio(fakeNTP, 2, 0); fakeNTP += inc; }

    int64_t t0 = nowNs();
    for (int i = 0; i < numBlocks; i++) {
        process_audio(fakeNTP, 2, 0);
        fakeNTP += inc;
    }
    int64_t t1 = nowNs();
    return (double)(t1 - t0) / numBlocks;
}

// Wait for synths to be created (give the HeadlessDriver time to process
// the /s_new messages before we stop it)
static void waitForSynths(EngineFixture& fx, int expectedCount) {
    for (int attempt = 0; attempt < 50; attempt++) {
        fx.send(osc_test::message("/status"));
        OscReply r;
        if (fx.waitForReply("/status.reply", r, 200)) {
            if (r.parsed().argInt(2) >= expectedCount) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// =========================================================================
// Benchmark cases
// =========================================================================

TEST_CASE("benchmark: idle engine", "[benchmark]") {
    EngineFixture fx;

    fprintf(stderr, "\n");
    fprintf(stderr, "  ╔══════════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "  ║  SUPERSONIC NATIVE BENCHMARK                                    ║\n");
    fprintf(stderr, "  ║  128 samples/block @ 48kHz = 2,666,667 ns budget                ║\n");
    fprintf(stderr, "  ╚══════════════════════════════════════════════════════════════════╝\n");

    // Report CPU frequency before and after spin-up
    int freqBefore = readCpuFreqKHz();
    spinUpCpu();
    int freqAfter = readCpuFreqKHz();
    if (freqBefore > 0) {
        fprintf(stderr, "  CPU: %d MHz → %d MHz (after spin-up)\n", freqBefore / 1000, freqAfter / 1000);
    }

    stopHeadlessDriver(fx);

    auto r = runBenchmark("idle engine (no synths)", 10000);
    CHECK(r.avgNs < r.budgetNs);  // Must be faster than real-time
}

TEST_CASE("benchmark: beep synths", "[benchmark]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNewSustained("sonic-pi-beep", 1000, 0, 1, "note", 60.0f));
    waitForSynths(fx, 1);
    stopHeadlessDriver(fx);
    auto r1 = runBenchmark("1x sonic-pi-beep", 10000);
    // Verify synth is alive: block cost should be above idle (~80-400ns)
    CHECK(r1.avgNs > 200);  // 1 beep should cost measurably more than idle
    CHECK(r1.avgNs < r1.budgetNs);
}

TEST_CASE("benchmark: 10 beep synths", "[benchmark]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    for (int i = 0; i < 10; i++)
        fx.send(sNewSustained("sonic-pi-beep", 1000 + i, 0, 1, "note", 60.0f + i));
    waitForSynths(fx, 10);
    stopHeadlessDriver(fx);
    auto r = runBenchmark("10x sonic-pi-beep", 10000);
    CHECK(r.avgNs < r.budgetNs);
}

TEST_CASE("benchmark: 50 beep synths", "[benchmark]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    for (int i = 0; i < 50; i++)
        fx.send(sNewSustained("sonic-pi-beep", 1000 + i, 0, 1, "note", 60.0f + (i % 12)));
    waitForSynths(fx, 50);
    stopHeadlessDriver(fx);
    auto r = runBenchmark("50x sonic-pi-beep", 5000);
    CHECK(r.avgNs < r.budgetNs);
}

TEST_CASE("benchmark: prophet synths", "[benchmark]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-prophet"));

    fx.send(sNewSustained("sonic-pi-prophet", 1000, 0, 1, "note", 60.0f));
    waitForSynths(fx, 1);
    stopHeadlessDriver(fx);
    auto r = runBenchmark("1x sonic-pi-prophet", 10000);
    CHECK(r.avgNs < r.budgetNs);
}

TEST_CASE("benchmark: 10 prophet synths", "[benchmark]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-prophet"));

    for (int i = 0; i < 10; i++)
        fx.send(sNewSustained("sonic-pi-prophet", 1000 + i, 0, 1, "note", 60.0f + i));
    waitForSynths(fx, 10);
    stopHeadlessDriver(fx);
    auto r = runBenchmark("10x sonic-pi-prophet", 5000);
    CHECK(r.avgNs < r.budgetNs);
}

TEST_CASE("benchmark: mixed load", "[benchmark]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));
    REQUIRE(fx.loadSynthDef("sonic-pi-prophet"));

    for (int i = 0; i < 20; i++)
        fx.send(sNewSustained("sonic-pi-beep", 2000 + i, 0, 1, "note", 60.0f + (i % 12)));
    for (int i = 0; i < 5; i++)
        fx.send(sNewSustained("sonic-pi-prophet", 3000 + i, 0, 1, "note", 48.0f + i * 7));
    waitForSynths(fx, 25);
    stopHeadlessDriver(fx);
    auto r = runBenchmark("20x beep + 5x prophet", 5000);
    CHECK(r.avgNs < r.budgetNs);
}

TEST_CASE("benchmark: reproducibility check", "[benchmark]") {
    // Run the same benchmark twice and verify results are within 15%
    // If this fails, the benchmark environment is too noisy
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    for (int i = 0; i < 10; i++)
        fx.send(sNewSustained("sonic-pi-beep", 1000 + i, 0, 1, "note", 60.0f + i));
    waitForSynths(fx, 10);
    stopHeadlessDriver(fx);

    fprintf(stderr, "\n  --- Reproducibility check (two runs, same config) ---\n");
    auto r1 = runBenchmark("10x beep (run A)", 5000);
    auto r2 = runBenchmark("10x beep (run B)", 5000);

    double diff = std::abs(r1.avgNs - r2.avgNs) / std::min(r1.avgNs, r2.avgNs) * 100.0;
    fprintf(stderr, "    delta: %.1f%%\n", diff);
    CHECK(diff < 15.0);  // Runs should be within 15%
}

TEST_CASE("benchmark: linearity check", "[benchmark]") {
    // Verify DSP cost scales roughly linearly with synth count.
    // Uses 10 vs 100 synths (not 1 vs 10) so the signal is well above
    // the ~400ns timer/overhead noise floor on fast machines.
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    stopHeadlessDriver(fx);

    // 10 synths (sustained so they stay alive through warmup + measurement)
    for (int i = 0; i < 10; i++)
        fx.send(sNewSustained("sonic-pi-beep", 1000 + i, 0, 1, "note", 60.0f + i));
    tickBlocks(200);
    auto lo = runBenchmark("10x beep (for linearity)", 5000);

    // Verify synths were alive: cost should be well above idle
    CHECK(lo.avgNs > 2000);  // 10 beeps should cost measurably more than idle

    // Verify still alive: measure again, should be similar
    double postCost10 = measureBlockCostNs();
    fprintf(stderr, "    post-benchmark block cost: %.0f ns (should be ~%.0f ns)\n",
            postCost10, lo.avgNs);
    CHECK(postCost10 > lo.avgNs * 0.5);  // within 2x of benchmark

    // Free them
    for (int i = 0; i < 10; i++)
        fx.send(osc_test::message("/n_free", 1000 + i));
    tickBlocks(100);

    // 100 synths
    for (int i = 0; i < 100; i++)
        fx.send(sNewSustained("sonic-pi-beep", 2000 + i, 0, 1, "note", 60.0f + (i % 24)));
    tickBlocks(200);
    auto hi = runBenchmark("100x beep (for linearity)", 5000);

    // Verify synths were alive: cost should be well above 10x
    CHECK(hi.avgNs > lo.avgNs * 3);  // at least 3x more expensive

    double ratio = hi.avgNs / lo.avgNs;

    fprintf(stderr, "\n    10-synth avg:  %.0f ns\n", lo.avgNs);
    fprintf(stderr, "    100-synth avg: %.0f ns\n", hi.avgNs);
    fprintf(stderr, "    ratio: %.1fx (ideal: 10.0x)\n", ratio);

    // Should be roughly linear. Super-linear scaling (>10x) is expected due
    // to L1/L2 cache pressure at high synth counts. Sub-linear (<10x) would
    // indicate beneficial batching. Both are acceptable.
    CHECK(ratio > 5.0);
    CHECK(ratio < 35.0);
}

TEST_CASE("benchmark: callback jitter (simulated real-time)", "[benchmark]") {
    // Simulate real-time callback scheduling: sleep between blocks just like
    // the HeadlessDriver does, and measure the actual inter-callback interval.
    // This tells us how much OS scheduling jitter we experience.

    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create a moderate load (10 beeps, sustained)
    for (int i = 0; i < 10; i++)
        fx.send(sNewSustained("sonic-pi-beep", 1000 + i, 0, 1, "note", 60.0f + i));
    waitForSynths(fx, 10);
    stopHeadlessDriver(fx);

    const int numBlocks = 3000;
    const int64_t blockNs = 1'000'000'000LL * 128 / 48000;  // ~2666667 ns
    const double blockUs = blockNs / 1000.0;

    std::vector<double> intervals(numBlocks);
    int64_t prevNs = nowNs();

    double fakeNTP = 4300000000.0;

    // Use steady_clock for portable absolute-deadline sleeping
    auto nextDeadline = std::chrono::steady_clock::now();

    for (int i = 0; i < numBlocks; i++) {
        nextDeadline += std::chrono::nanoseconds(blockNs);
        std::this_thread::sleep_until(nextDeadline);

        int64_t now = nowNs();
        intervals[i] = (double)(now - prevNs) / 1000.0;  // store as microseconds
        prevNs = now;

        process_audio(fakeNTP, 2, 0);
        fakeNTP += 128.0 / 48000.0;
    }

    // Skip first few intervals (warmup)
    std::vector<double> jitter(numBlocks - 10);
    for (int i = 10; i < numBlocks; i++)
        jitter[i - 10] = std::abs(intervals[i] - blockUs);

    std::sort(jitter.begin(), jitter.end());
    double avgJitter = std::accumulate(jitter.begin(), jitter.end(), 0.0) / jitter.size();

    fprintf(stderr, "\n  --- Callback jitter (10x beep, simulated RT) ---\n");
    fprintf(stderr, "    expected interval:  %.0f us\n", blockUs);
    fprintf(stderr, "    jitter avg:         %.0f us\n", avgJitter);
    fprintf(stderr, "    jitter median:      %.0f us\n", jitter[jitter.size() / 2]);
    fprintf(stderr, "    jitter p95:         %.0f us\n", jitter[(int)(jitter.size() * 0.95)]);
    fprintf(stderr, "    jitter p99:         %.0f us\n", jitter[(int)(jitter.size() * 0.99)]);
    fprintf(stderr, "    jitter max:         %.0f us\n", jitter.back());
    fprintf(stderr, "    blocks measured:    %d\n", (int)jitter.size());

    // No assertion — jitter depends entirely on the OS scheduler and
    // CI VMs have no RT guarantees. The printed stats above are the value.
    SUCCEED();
}

TEST_CASE("benchmark: scaling test", "[benchmark]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    spinUpCpu();
    int freqKHz = readCpuFreqKHz();
    if (freqKHz > 0)
        fprintf(stderr, "\n  --- Scaling: beep synths (CPU @ %d MHz) ---\n", freqKHz / 1000);
    else
        fprintf(stderr, "\n  --- Scaling: beep synths ---\n");

    int counts[] = {1, 5, 10, 25, 50, 75, 100};
    for (int count : counts) {
        for (int i = 0; i < count; i++)
            fx.send(sNewSustained("sonic-pi-beep", 5000 + i, 0, 1, "note", 60.0f + (i % 24)));

        waitForSynths(fx, count);

        // Stop headless driver on first iteration
        if (count == 1) stopHeadlessDriver(fx);

        char label[64];
        snprintf(label, sizeof(label), "%dx sonic-pi-beep", count);
        auto r = runBenchmark(label, 3000, 200);

        // Verify synths are alive: cost should scale with count
        if (count >= 10) {
            CHECK(r.avgNs > 200 * count);  // ~200ns per synth minimum (ARM64 is fast)
        }

        // Free all synths for next round
        for (int i = 0; i < count; i++)
            fx.send(osc_test::message("/n_free", 5000 + i));
        tickBlocks(100);
    }

    freqKHz = readCpuFreqKHz();
    if (freqKHz > 0)
        fprintf(stderr, "  CPU @ %d MHz (end of scaling test)\n", freqKHz / 1000);
    SUCCEED();
}
