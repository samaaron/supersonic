// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * FrontRig.h — supersonic::Commands around an in-process engine, wired as the
 * server wires it (SuperSonicMain.cpp): the engine's transport is clockwork's
 * FrontedTransport, so every reply is offered to the front first and every
 * notice (setup) is heard by it, in the order the engine delivers them, and a
 * recording sink stands in for the socket.
 *
 * When the test is the host (EngineFixture::testIsTheHost, the default), the
 * front starts no threads (Commands::Work::Host): its file jobs and a
 * recording's copying are the fixture's host work, run after each block's
 * control pass. The test thread is then the one thing that moves, and a wait
 * here is a count of blocks. With a driver rendering, the front has its own
 * threads, as in the server.
 */
#pragma once

#include "CallbackTransport.h"
#include "EngineFixture.h"
#include "IOscTransport.h"
#include "OscFront.h"
#include "OscTestUtils.h"
#include "supersonic_commands.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The socket, as the front sees it: every send is kept, by token.
struct RecordingSink final : IOscTransport {
    struct Sent { uint32_t token; OscReply reply; };
    std::mutex        mu;
    std::vector<Sent> sent;

    bool send(uint32_t token, const uint8_t* d, uint32_t n, bool) override {
        std::lock_guard<std::mutex> lock(mu);
        sent.push_back({ token, OscReply{ osc_test::parseAddress(d, n), std::vector<uint8_t>(d, d + n) } });
        return true;
    }
    void broadcastNotify(const uint8_t*, uint32_t) override {}
    void broadcastLink(const uint8_t*, uint32_t) override {}
    bool hasNotifySubscribers() const override { return false; }
    bool subscribeNotify(uint32_t) override { return false; }
    bool subscribeNotifyPort(int) override { return false; }
    void unsubscribeNotify(uint32_t) override {}
    void clearNotify() override {}
    bool subscribeLink(uint32_t) override { return false; }
    void unsubscribeLink(uint32_t) override {}
    void broadcastMidi(const uint8_t*, uint32_t) override {}
    bool subscribeMidi(uint32_t) override { return false; }
    void unsubscribeMidi(uint32_t) override {}
    void broadcastGamepad(const uint8_t*, uint32_t) override {}
    bool subscribeGamepad(uint32_t) override { return false; }
    void unsubscribeGamepad(uint32_t) override {}

    // The first reply at `addr` for `token`, and with `cmd`, the first whose
    // first string argument is `cmd` (scsynth's /done and /fail name the
    // command).
    bool find(const char* addr, uint32_t token, const char* cmd, OscReply* out) {
        std::lock_guard<std::mutex> lock(mu);
        for (const auto& s : sent) {
            if (s.token != token || s.reply.address != addr) continue;
            if (cmd && (s.reply.parsed().argCount() < 1 || s.reply.parsed().argString(0) != cmd)) continue;
            if (out) *out = s.reply;
            return true;
        }
        return false;
    }
    size_t count(const char* addr, uint32_t token) {
        std::lock_guard<std::mutex> lock(mu);
        size_t n = 0;
        for (const auto& s : sent) if (s.token == token && s.reply.address == addr) ++n;
        return n;
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mu);
        sent.clear();
    }
};

struct FrontRig {
    using Work = supersonic::Commands::Work;

    RecordingSink    sink;
    FrontedTransport fronted;
    const Work       work;
private:
    // Made over the engine before it boots, as the server makes them: the
    // transport the engine's replies leave by has to be in place by then.
    std::unique_ptr<CallbackTransport>    mSocket;   // what passes the front: the fixture's own callbacks
    std::unique_ptr<supersonic::Commands> mFront;
public:
    EngineFixture         fx;
    supersonic::Commands& front;

    // Host work when the test is the host; the front's own threads otherwise.
    explicit FrontRig(const ClockworkEngine::Config& cfg = EngineFixture::defaultConfig())
        : FrontRig(cfg, cfg.manualAudioPump && cfg.hostDrivesControl ? Work::Host : Work::Own) {}
    // A case that needs the front on threads of its own whoever renders: one
    // timing its blocks, which must not include the front's work.
    FrontRig(const ClockworkEngine::Config& cfg, Work w)
        : work(w),
          fx(cfg, [this](ClockworkEngine& engine) {
              mSocket = std::make_unique<CallbackTransport>(&engine.onReply, &engine.onReplyRouted);
              mFront  = std::make_unique<supersonic::Commands>(engine, &sink, work);
              fronted.attach(mSocket.get(), mFront.get());
              engine.setTransport(&fronted);
          }),
          front(*mFront) {
        // What passes the front reaches the socket, as the front's own answers do.
        fx.setRoutedObserver([this](uint32_t origin, uint32_t, const uint8_t* d, uint32_t n) {
            sink.send(origin, d, n, false);
        });
        if (work == Work::Host) fx.setHostWork([this] { front.runPending(); });
    }
    ~FrontRig() {
        fx.setHostWork(nullptr);
        fx.setRoutedObserver(nullptr);   // under the fixture's lock: no call runs after this
        // The engine stops calling into the front before the front goes; the
        // fixture's own shutdown after this is a second, harmless one.
        fx.engine().shutdown();
        fronted.detachFront();
        mFront.reset();
    }
    FrontRig(const FrontRig&) = delete;
    FrontRig& operator=(const FrontRig&) = delete;

    // Whether this thread moves everything a wait waits for: the engine's
    // blocks and passes, and the front's work.
    bool testMovesAll() const { return work == Work::Host && fx.testIsTheHost(); }

    // The first reply at `addr` for `token` (RecordingSink::find) within
    // `timeoutMs`: of audio, a block a turn, when the test moves everything;
    // of wall clock otherwise. `turn`, given, is what a turn does instead: a
    // case timing its blocks pumps them itself.
    bool waitFor(const char* addr, uint32_t token, const char* cmd, OscReply* out,
                 int timeoutMs = 5000, const std::function<void()>& turn = {}) {
        if (testMovesAll()) {
            for (uint32_t owed = fx.blocksOfAudio(timeoutMs);; --owed) {
                if (sink.find(addr, token, cmd, out)) return true;
                if (!owed) return false;
                if (turn) turn(); else fx.pumpBlock();
            }
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (sink.find(addr, token, cmd, out)) return true;
            if (turn) turn(); else idle();
        }
        return sink.find(addr, token, cmd, out);
    }

    // A wait's pause between looks: a block (with its control pass and the
    // host work) when the test pumps, and a moment for the other threads when
    // they move things too.
    void idle() {
        if (fx.manualPump()) fx.pumpBlock();
        if (!testMovesAll()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
};
