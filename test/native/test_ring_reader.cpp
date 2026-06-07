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
 *      stop path must change the value. Without that, stopThread() times out
 *      and JUCE falls through to killThread()/pthread_cancel(), which can
 *      wedge under ThreadSanitizer. This is the Linux headless-teardown hazard
 *      from LINUX-TSAN-HANG-HANDOFF.md: once the headless driver stops ticking
 *      processCount, nothing else wakes the gateway.
 *
 *   2. The happy path still works: a message written to a drained ring and
 *      followed by a wake-word bump is delivered to the onMessage callback.
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
    reader->startThread();

    // Give the run loop time to reach its blocking wait on `wake`.
    std::this_thread::sleep_for(milliseconds(50));

    // Nobody ever changes `wake`. Destroying the reader must still stop the
    // thread quickly. The broken path (notify_all without a value change)
    // leaves the thread parked in atomic::wait, so the destructor blocks for
    // the full stopThread(2000) timeout and then resorts to killThread().
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
    reader.startThread();

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
