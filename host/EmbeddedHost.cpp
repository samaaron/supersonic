// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
#include "EmbeddedHost.h"

#include "IOscTransport.h"
#include "OscFront.h"
#include "clockwork_product.h"
#include "supersonic_commands.h"
#if CLOCKWORK_HAS_PLUGIN_TRACKS
#include "TrackControl.h"
#endif

#include <set>

#ifndef SUPERSONIC_VERSION_STRING
#define SUPERSONIC_VERSION_STRING "dev"
#endif

namespace supersonic {

namespace {

// The one client's token: non-zero, so the engine answers it directly (a
// reply to the sender, where 0 would be a broadcast to the notify audience).
constexpr uint32_t kClientToken = 1;

// The engine→application boundary: an IOscTransport whose every send and
// broadcast lands on one callback. The shape of the NIF's transport: a real
// peer (so Link, MIDI, gamepad and OSC pushes are delivered once subscribed)
// with no socket. Only the NRT gateway calls these; the flags are atomics so
// a shutdown on another thread reads them safely.
class CallbackPeerTransport final : public IOscTransport {
public:
    explicit CallbackPeerTransport(EmbeddedHost::Callbacks* cb) : mCb(cb) {}

    // The application is the one peer: a send reaches it or not, and a
    // broadcast meant for it misses it when there is no callback to take it.
    bool send(uint32_t, const uint8_t* data, uint32_t size, bool /*networkOnly*/) override { return deliver(data, size); }
    uint32_t broadcastNotify(const uint8_t* data, uint32_t size) override { return deliver(data, size) ? 0 : 1; }
    uint32_t broadcastLink(const uint8_t* data, uint32_t size) override { return broadcastIf(mLink, data, size); }
    bool hasNotifySubscribers() const override { std::lock_guard<std::mutex> lk(mMutex); return !mNotify.empty(); }
    bool subscribeNotify(uint32_t token) override { std::lock_guard<std::mutex> lk(mMutex); return mNotify.insert(token).second; }
    void unsubscribeNotify(uint32_t token) override { std::lock_guard<std::mutex> lk(mMutex); mNotify.erase(token); }
    void clearNotify() override { std::lock_guard<std::mutex> lk(mMutex); mNotify.clear(); }
    bool subscribeLink(uint32_t) override { mLink = true; return true; }
    void unsubscribeLink(uint32_t) override { mLink = false; }
    uint32_t broadcastMidi(const uint8_t* data, uint32_t size) override { return broadcastIf(mMidi, data, size); }
    bool subscribeMidi(uint32_t) override { mMidi = true; return true; }
    void unsubscribeMidi(uint32_t) override { mMidi = false; }
    uint32_t broadcastGamepad(const uint8_t* data, uint32_t size) override { return broadcastIf(mGamepad, data, size); }
    bool subscribeGamepad(uint32_t) override { mGamepad = true; return true; }
    void unsubscribeGamepad(uint32_t) override { mGamepad = false; }
    uint32_t broadcastOsc(const uint8_t* data, uint32_t size) override { return broadcastIf(mOsc, data, size); }
    bool subscribeOsc(uint32_t) override { mOsc = true; return true; }
    void unsubscribeOsc(uint32_t) override { mOsc = false; }

private:
    uint32_t broadcastIf(const std::atomic<bool>& subscribed, const uint8_t* data, uint32_t size) {
        return subscribed.load() && !deliver(data, size) ? 1u : 0u;
    }
    bool deliver(const uint8_t* data, uint32_t size) {
        if (!mCb || !mCb->onMessage) return false;
        mCb->onMessage(data, size);
        return true;
    }
    EmbeddedHost::Callbacks* mCb;
    mutable std::mutex mMutex;
    std::set<uint32_t> mNotify;
    std::atomic<bool> mLink { false }, mMidi { false }, mGamepad { false }, mOsc { false };
};

} // namespace

struct EmbeddedHost::Impl {
    Callbacks callbacks;
    // Declared in the order SuperSonicMain declares them: the transport
    // outlives the engine's teardown; the front is torn down first.
    CallbackPeerTransport transport { &callbacks };
    FrontedTransport fronted;
    ClockworkEngine engine;
    std::unique_ptr<Commands> front;
    std::string summary;
};

EmbeddedHost::EmbeddedHost() : mImpl(std::make_unique<Impl>()) {}

EmbeddedHost::~EmbeddedHost() { shutdown(); }

bool EmbeddedHost::boot(const ClockworkEngine::Config& cfg, Callbacks callbacks, std::string* err) {
    auto& I = *mImpl;
    if (mRunning.load()) {
        if (err) *err = "already running";
        return false;
    }
    I.callbacks = std::move(callbacks);
    I.engine.onDebug = [this](const std::string& s) {
        const size_t end = s.find_last_not_of("\r\n");
        if (end == std::string::npos) return;
        if (mImpl->callbacks.onLog) mImpl->callbacks.onLog(s.substr(0, end + 1));
    };

    // The front answers through the transport the engine's replies leave by,
    // and sees those replies first.
    I.front = std::make_unique<Commands>(I.engine, &I.transport);
    I.summary = std::string(CLOCKWORK_PRODUCT_NAME) + " " + SUPERSONIC_VERSION_STRING + " (embedded)";
    I.front->setSummary(I.summary);
    I.fronted.attach(&I.transport, I.front.get());
    I.engine.setTransport(&I.fronted);

#if CLOCKWORK_HAS_PLUGIN_TRACKS
    // The tracks' lanes are laid out when memory is: before the boot. Once
    // per process; a second boot in the same process finds them reserved.
    static std::once_flag lanes;
    std::call_once(lanes, [] { TrackControl::reserveLanes(); });
#endif

    auto fail = [&](const std::string& why) {
        if (err) *err = why;
        I.fronted.detachFront();
        I.front.reset();
        return false;
    };
    try {
        I.engine.init(cfg);
    } catch (const std::exception& e) {
        return fail(e.what());
    } catch (...) {
        return fail("unknown exception during the engine's init");
    }
    if (I.engine.engineState() == EngineState::Error) {
        std::string why = I.engine.errorReason();
        if (why.empty()) why = "the engine did not start";
        I.engine.shutdown();
        return fail(why);
    }
    mRunning = true;
    return true;
}

void EmbeddedHost::shutdown() {
    auto& I = *mImpl;
    if (!mRunning.exchange(false)) return;
    {
        // the front stops being consulted before it goes; then the engine
        std::lock_guard<std::mutex> lk(mSendMutex);
        I.fronted.detachFront();
    }
    I.engine.shutdown();
    I.front.reset();
}

bool EmbeddedHost::send(const uint8_t* data, uint32_t size) {
    auto& I = *mImpl;
    std::lock_guard<std::mutex> lk(mSendMutex);
    if (!mRunning.load() || !data || !size) return false;
    if (I.front && I.front->ingress(data, size, kClientToken)) return true;
    return I.engine.ingest(data, size, kClientToken) == CLOCKWORK_OK;
}

bool EmbeddedHost::loadDefinitions(const std::string& dir, std::string* why) {
    auto& I = *mImpl;
    if (!mRunning.load() || !I.front) {
        if (why) *why = "not running";
        return false;
    }
    return I.front->loadDefinitions(dir, why);
}

detail_shm_segment::shm_native_handle EmbeddedHost::dupSegmentHandle() const {
    if (!mRunning.load()) return detail_shm_segment::shm_invalid_handle;
    const auto h = mImpl->engine.shmNativeHandle();
    if (!detail_shm_segment::shm_handle_valid(h)) return detail_shm_segment::shm_invalid_handle;
    return detail_shm_segment::shm_dup_handle(h);
}

ClockworkEngine& EmbeddedHost::engine() { return mImpl->engine; }
Commands& EmbeddedHost::front() { return *mImpl->front; }
std::string EmbeddedHost::summary() const { return mImpl->summary; }

} // namespace supersonic
