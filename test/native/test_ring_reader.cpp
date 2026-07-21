/*
 * test_ring_reader.cpp — Tests for the NRT egress gateway reader (RingReader).
 *
 * RingReader runs a thread that blocks on an external wake word
 * (std::atomic<uint32_t>, in production the audio callback's processCount) and
 * drains one or more Message-framed SPSC rings on each wake.
 *
 * The contract these tests pin down:
 *
 *   1. Stopping the reader must NOT depend on anyone bumping the wake word.
 *      std::atomic::wait(old) only returns when the value DIFFERS from `old`,
 *      so a bare notify_all() (value unchanged) cannot break the wait — the
 *      stop path must change the value. Without that, stop() times out
 *      and JUCE falls through to killThread()/pthread_cancel(), which can
 *      wedge under ThreadSanitizer. Under the headless driver, once it stops
 *      ticking processCount nothing else wakes the gateway.
 *
 *   2. The happy path still works: a message written to a drained ring and
 *      followed by a wake-word bump is delivered to the onMessage callback.
 *
 *   3. pause() is quiescent and safe from any thread: once it returns, no
 *      drain runs until resume() (cold swap resets the ring/drain state the
 *      drains walk), and calling it from the reader's own thread (an OSC
 *      handler in a drain triggering a swap) is a no-op rather than a
 *      self-deadlock.
 */
#include <catch2/catch_test_macros.hpp>
#include "src/workers/RingReader.h"
#include "src/workers/RingBufferWriter.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono;

TEST_CASE("RingReader stops promptly when its wake word is never bumped",
          "[RingReader]") {
    std::atomic<uint32_t> wake{0};

    auto reader = std::make_unique<RingReader>("test-ringreader-stop");
    reader->setWake(&wake);
    reader->start();

    // Give the run loop time to reach its blocking wait on `wake`.
    std::this_thread::sleep_for(milliseconds(50));

    // Nobody ever changes `wake`. Destroying the reader must still stop the
    // thread quickly. The broken path (notify_all without a value change)
    // leaves the thread parked in atomic::wait, so the destructor blocks for
    // the join timeout.
    const auto t0 = steady_clock::now();
    reader.reset();
    const auto elapsedMs =
        duration_cast<milliseconds>(steady_clock::now() - t0).count();

    INFO("RingReader destructor took " << elapsedMs << " ms");
    REQUIRE(elapsedMs < 1000);
}

TEST_CASE("RingReader delivers a message after its wake word is bumped",
          "[RingReader]") {
    constexpr uint32_t kSize = 4096;
    std::vector<uint8_t> buffer(kSize, 0);
    std::atomic<int32_t> head{0}, tail{0}, sequence{0}, writeLock{0};
    std::atomic<uint32_t> wake{0};

    std::mutex rxMutex;
    std::string received;
    std::atomic<uint32_t> gotSource{0};
    std::atomic<int> gotCount{0};

    RingReader reader("test-ringreader-drain");
    reader.setWake(&wake);
    reader.addDrain(
        buffer.data(), kSize, &head, &tail,
        [&](uint32_t sourceId, const uint8_t* p, uint32_t n, uint32_t) {
            std::lock_guard<std::mutex> lock(rxMutex);
            received.assign(reinterpret_cast<const char*>(p), n);
            gotSource.store(sourceId, std::memory_order_release);
            gotCount.fetch_add(1, std::memory_order_release);
        },
        RingReader::Metrics{});
    reader.start();

    const std::string payload = "hello-gateway";
    const uint32_t kSourceId = 7;
    REQUIRE(RingBufferWriter::write(
        buffer.data(), kSize, &head, &tail, &sequence, &writeLock,
        payload.data(), static_cast<uint32_t>(payload.size()), kSourceId));

    // Wake the reader the way the audio callback does: bump the value and
    // notify. The real callback does this every audio block, so keep bumping
    // until the message is drained (a single bump can be missed if the reader
    // latches the post-bump value before parking — a benign lost-wakeup that
    // the continuous tick self-corrects).
    const auto deadline = steady_clock::now() + seconds(2);
    while (gotCount.load(std::memory_order_acquire) == 0
           && steady_clock::now() < deadline) {
        wake.fetch_add(1, std::memory_order_release);
        wake.notify_all();
        std::this_thread::sleep_for(milliseconds(2));
    }

    REQUIRE(gotCount.load(std::memory_order_acquire) == 1);
    REQUIRE(gotSource.load(std::memory_order_acquire) == kSourceId);
    {
        std::lock_guard<std::mutex> lock(rxMutex);
        REQUIRE(received == payload);
    }
}

TEST_CASE("RingReader pause parks draining until resume", "[RingReader]") {
    constexpr uint32_t kSize = 4096;
    std::vector<uint8_t> buffer(kSize, 0);
    std::atomic<int32_t> head{0}, tail{0}, sequence{0}, writeLock{0};
    std::atomic<uint32_t> wake{0};
    std::atomic<int> gotCount{0};

    RingReader reader("test-ringreader-pause");

    // pause() before start() is a no-op, not a hang.
    reader.pause();
    reader.resume();

    reader.setWake(&wake);
    reader.addDrain(
        buffer.data(), kSize, &head, &tail,
        [&](uint32_t, const uint8_t*, uint32_t, uint32_t) {
            gotCount.fetch_add(1, std::memory_order_release);
        },
        RingReader::Metrics{});
    reader.start();

    reader.pause();

    // While parked, a written message plus continuous wake ticks must not
    // reach the callback.
    const std::string payload = "parked";
    REQUIRE(RingBufferWriter::write(
        buffer.data(), kSize, &head, &tail, &sequence, &writeLock,
        payload.data(), static_cast<uint32_t>(payload.size()), 0));
    for (int i = 0; i < 50; i++) {
        wake.fetch_add(1, std::memory_order_release);
        wake.notify_all();
        std::this_thread::sleep_for(milliseconds(2));
    }
    REQUIRE(gotCount.load(std::memory_order_acquire) == 0);

    // After resume the pending message is drained.
    reader.resume();
    const auto deadline = steady_clock::now() + seconds(2);
    while (gotCount.load(std::memory_order_acquire) == 0
           && steady_clock::now() < deadline) {
        wake.fetch_add(1, std::memory_order_release);
        wake.notify_all();
        std::this_thread::sleep_for(milliseconds(2));
    }
    REQUIRE(gotCount.load(std::memory_order_acquire) == 1);
}

TEST_CASE("RingReader pause from its own thread is a no-op, not a deadlock",
          "[RingReader]") {
    std::atomic<uint32_t> wake{0};
    std::atomic<bool> selfPaused{false};
    std::atomic<int> passes{0};

    RingReader reader("test-ringreader-selfpause");
    reader.setWake(&wake);
    // Mirrors a drain handler that triggers a cold swap: switchDevice parks
    // the readers, including the one whose handler is running.
    reader.addTask([&]() {
        if (!selfPaused.exchange(true)) reader.pause();
        passes.fetch_add(1, std::memory_order_release);
    });
    reader.start();

    // If self-pause deadlocked, the task would never complete a pass and the
    // reader would stop servicing wakes.
    const auto deadline = steady_clock::now() + seconds(2);
    while (passes.load(std::memory_order_acquire) < 2
           && steady_clock::now() < deadline) {
        wake.fetch_add(1, std::memory_order_release);
        wake.notify_all();
        std::this_thread::sleep_for(milliseconds(2));
    }
    REQUIRE(selfPaused.load());
    REQUIRE(passes.load(std::memory_order_acquire) >= 2);
}

// ── Blocking observability (sonic-pi#3551) ───────────────────────────────────
//
// One thread drains control commands AND the egress behind them, so a handler
// that blocks stops the server answering anything while its socket stays up and
// its audio keeps ticking. That failure produced no log line, no metric and no
// test signal — the user's only evidence was a 30 s silence.
//
// These pin the measurement rather than any particular slow caller: whatever
// blocks, the gateway must be able to say how long for, and say it WHILE stuck
// (a high-water mark alone only speaks once the stall is over).

TEST_CASE("RingReader reports how long a blocking handler held the thread",
          "[RingReader][blocking]") {
    constexpr uint32_t kSize = 4096;
    std::vector<uint8_t> buffer(kSize, 0);
    std::atomic<int32_t> head{0}, tail{0}, sequence{0}, writeLock{0};
    std::atomic<uint32_t> wake{0};
    std::atomic<int> gotCount{0};

    RingReader reader("test-ringreader-slow");
    reader.setWake(&wake);
    reader.addDrain(
        buffer.data(), kSize, &head, &tail,
        [&](uint32_t, const uint8_t*, uint32_t, uint32_t) {
            std::this_thread::sleep_for(milliseconds(120));
            gotCount.fetch_add(1, std::memory_order_release);
        },
        RingReader::Metrics{});
    reader.start();

    // Nothing has run yet, so nothing has blocked.
    REQUIRE(reader.maxPassUs() == 0);

    const std::string payload = "slow";
    REQUIRE(RingBufferWriter::write(
        buffer.data(), kSize, &head, &tail, &sequence, &writeLock,
        payload.data(), static_cast<uint32_t>(payload.size()), 0));

    const auto deadline = steady_clock::now() + seconds(5);
    while (gotCount.load(std::memory_order_acquire) == 0
           && steady_clock::now() < deadline) {
        wake.fetch_add(1, std::memory_order_release);
        wake.notify_all();
        std::this_thread::sleep_for(milliseconds(5));
    }
    REQUIRE(gotCount.load(std::memory_order_acquire) >= 1);

    // The engine measures itself, so this holds however starved the test
    // process was — unlike a wall-clock deadline in the test thread.
    REQUIRE(reader.maxPassUs() >= 100'000);

    reader.resetMaxPassUs();
    REQUIRE(reader.maxPassUs() == 0);
}

TEST_CASE("RingReader exposes a stall while it is still stuck",
          "[RingReader][blocking]") {
    constexpr uint32_t kSize = 4096;
    std::vector<uint8_t> buffer(kSize, 0);
    std::atomic<int32_t> head{0}, tail{0}, sequence{0}, writeLock{0};
    std::atomic<uint32_t> wake{0};
    std::atomic<bool> release{false};
    std::atomic<bool> inHandler{false};

    RingReader reader("test-ringreader-inflight");
    reader.setWake(&wake);
    reader.addDrain(
        buffer.data(), kSize, &head, &tail,
        [&](uint32_t, const uint8_t*, uint32_t, uint32_t) {
            inHandler.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire))
                std::this_thread::sleep_for(milliseconds(2));
        },
        RingReader::Metrics{});
    reader.start();

    // Idle: not blocked.
    REQUIRE(reader.inFlightUs() == 0);

    const std::string payload = "wedge";
    REQUIRE(RingBufferWriter::write(
        buffer.data(), kSize, &head, &tail, &sequence, &writeLock,
        payload.data(), static_cast<uint32_t>(payload.size()), 0));

    auto deadline = steady_clock::now() + seconds(5);
    while (!inHandler.load(std::memory_order_acquire)
           && steady_clock::now() < deadline) {
        wake.fetch_add(1, std::memory_order_release);
        wake.notify_all();
        std::this_thread::sleep_for(milliseconds(5));
    }
    REQUIRE(inHandler.load(std::memory_order_acquire));

    // Wedged right now: this is what a watchdog or the metrics panel reads to
    // distinguish "server is busy" from "server can never answer you".
    std::this_thread::sleep_for(milliseconds(60));
    REQUIRE(reader.inFlightUs() >= 50'000);
    REQUIRE(reader.maxPassUs() == 0);   // the pass has not ended yet

    release.store(true, std::memory_order_release);

    deadline = steady_clock::now() + seconds(5);
    while (reader.inFlightUs() != 0 && steady_clock::now() < deadline)
        std::this_thread::sleep_for(milliseconds(5));
    REQUIRE(reader.inFlightUs() == 0);       // released
    REQUIRE(reader.maxPassUs() >= 50'000);   // and recorded on the way out
}

TEST_CASE("RingReader reports a slow pass to its observer",
          "[RingReader][blocking]") {
    constexpr uint32_t kSize = 4096;
    std::vector<uint8_t> buffer(kSize, 0);
    std::atomic<int32_t> head{0}, tail{0}, sequence{0}, writeLock{0};
    std::atomic<uint32_t> wake{0};
    std::atomic<uint32_t> reportedUs{0};
    std::atomic<int> reports{0};

    RingReader reader("test-ringreader-report");
    reader.setWake(&wake);
    reader.setSlowPassThresholdUs(50'000);
    reader.onSlowPass([&](uint32_t us) {
        reportedUs.store(us, std::memory_order_release);
        reports.fetch_add(1, std::memory_order_release);
    });
    reader.addDrain(
        buffer.data(), kSize, &head, &tail,
        [&](uint32_t, const uint8_t*, uint32_t, uint32_t) {
            std::this_thread::sleep_for(milliseconds(80));
        },
        RingReader::Metrics{});
    reader.start();

    const std::string payload = "report";
    REQUIRE(RingBufferWriter::write(
        buffer.data(), kSize, &head, &tail, &sequence, &writeLock,
        payload.data(), static_cast<uint32_t>(payload.size()), 0));

    const auto deadline = steady_clock::now() + seconds(5);
    while (reports.load(std::memory_order_acquire) == 0
           && steady_clock::now() < deadline) {
        wake.fetch_add(1, std::memory_order_release);
        wake.notify_all();
        std::this_thread::sleep_for(milliseconds(5));
    }

    REQUIRE(reports.load(std::memory_order_acquire) >= 1);
    REQUIRE(reportedUs.load(std::memory_order_acquire) >= 50'000);
}

TEST_CASE("RingReader stays quiet when passes are quick",
          "[RingReader][blocking]") {
    constexpr uint32_t kSize = 4096;
    std::vector<uint8_t> buffer(kSize, 0);
    std::atomic<int32_t> head{0}, tail{0}, sequence{0}, writeLock{0};
    std::atomic<uint32_t> wake{0};
    std::atomic<int> reports{0};
    std::atomic<int> gotCount{0};

    RingReader reader("test-ringreader-quiet");
    reader.setWake(&wake);
    reader.setSlowPassThresholdUs(250'000);
    reader.onSlowPass([&](uint32_t) { reports.fetch_add(1, std::memory_order_release); });
    reader.addDrain(
        buffer.data(), kSize, &head, &tail,
        [&](uint32_t, const uint8_t*, uint32_t, uint32_t) {
            gotCount.fetch_add(1, std::memory_order_release);
        },
        RingReader::Metrics{});
    reader.start();

    for (int i = 0; i < 20; i++) {
        const std::string payload = "quick";
        REQUIRE(RingBufferWriter::write(
            buffer.data(), kSize, &head, &tail, &sequence, &writeLock,
            payload.data(), static_cast<uint32_t>(payload.size()), 0));
        wake.fetch_add(1, std::memory_order_release);
        wake.notify_all();
        std::this_thread::sleep_for(milliseconds(2));
    }

    REQUIRE(reports.load(std::memory_order_acquire) == 0);
}
