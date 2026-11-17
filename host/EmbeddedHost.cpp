// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
#include "EmbeddedHost.h"

#include "IOscTransport.h"
#include "OscFront.h"
#include "clockwork_product.h"
#include "supersonic_commands.h"
#if CLOCKWORK_HAS_PLUGIN_TRACKS
#include "TrackControl.h"
#endif

#ifndef SUPERSONIC_VERSION_STRING
#define SUPERSONIC_VERSION_STRING "dev"
#endif

namespace supersonic {

namespace {

// The engine→application boundary: everything the engine says to its one
// client lands on onMessage, on the engine's control thread. An embedded
// application sends without tokens, so none is handed on.
class CallbackClientTransport final : public IOscTransport {
public:
    explicit CallbackClientTransport(EmbeddedHost::Callbacks* cb) : mCb(cb) {}
    bool deliver(const uint8_t* data, uint32_t size, uint32_t /*token*/) override {
        if (!mCb || !mCb->onMessage) return false;
        mCb->onMessage(data, size);
        return true;
    }
private:
    EmbeddedHost::Callbacks* mCb;
};

} // namespace

struct EmbeddedHost::Impl {
    Callbacks callbacks;
    // Declared in the order SuperSonicMain declares them: the transport
    // outlives the engine's teardown; the front is torn down first.
    CallbackClientTransport transport { &callbacks };
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
    // and sees those replies first. It is made after init, over the handle
    // the booted engine hands out.
    I.fronted.attach(&I.transport, nullptr);
    I.engine.setTransport(&I.fronted);

    // The tracks' lanes, laid out when memory is: reserved by the engine as
    // it boots.
    ClockworkEngine::Config booted = cfg;
#if CLOCKWORK_HAS_PLUGIN_TRACKS
    booted.reservedLanes = TrackControl::kLanes;
#endif

    auto fail = [&](const std::string& why) {
        if (err) *err = why;
        I.fronted.detachFront();
        I.front.reset();
        return false;
    };
    try {
        I.engine.init(booted);
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
    I.front = std::make_unique<Commands>(I.engine.newClient(), &I.transport);
    I.summary = std::string(CLOCKWORK_PRODUCT_NAME) + " " + SUPERSONIC_VERSION_STRING + " (embedded)";
    I.front->setSummary(I.summary);
    I.fronted.attach(&I.transport, I.front.get());
    mRunning = true;
    return true;
}

void EmbeddedHost::shutdown() {
    auto& I = *mImpl;
    if (!mRunning.exchange(false)) return;
    {
        // the front stops being consulted before it goes
        std::lock_guard<std::mutex> lk(mSendMutex);
        I.fronted.detachFront();
    }
    // The front before its engine: a client goes before its core. A
    // recording in progress drains its last frames and closes its file while
    // the core is still there; closing the core frees its memory.
    I.front.reset();
    I.engine.shutdown();
}

bool EmbeddedHost::send(const uint8_t* data, uint32_t size) {
    auto& I = *mImpl;
    std::lock_guard<std::mutex> lk(mSendMutex);
    if (!mRunning.load() || !data || !size) return false;
    if (I.front && I.front->ingress(data, size, /*token*/ 0)) return true;
    return I.engine.ingest(data, size, /*token*/ 0) == CLOCKWORK_OK;
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
    const auto h = mImpl->engine.shmReaderHandle();
    if (!detail_shm_segment::shm_handle_valid(h)) return detail_shm_segment::shm_invalid_handle;
    return detail_shm_segment::shm_dup_handle(h);
}

ClockworkEngine& EmbeddedHost::engine() { return mImpl->engine; }
Commands& EmbeddedHost::front() { return *mImpl->front; }
std::string EmbeddedHost::summary() const { return mImpl->summary; }

} // namespace supersonic
