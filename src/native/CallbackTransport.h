/*
 * CallbackTransport.h — in-process IOscTransport for embedders and tests.
 *
 * Replies and broadcasts surface through the engine's onReply callback (debug
 * through onDebug); the single observer is the host that set them. This is the
 * engine's default transport, so a bare `SupersonicEngine` works standalone
 * (it's what the test fixture uses).
 *
 * The subscriber sets exist only to gate broadcasts (hasNotifySubscribers): a
 * notify broadcast reaches onReply once something has subscribed. Only the NRT
 * gateway calls these, so no locking is needed.
 */
#pragma once

#include "IOscTransport.h"

#include <cstdint>
#include <functional>
#include <set>

class CallbackTransport : public IOscTransport {
public:
    // Bound to the engine's onReply member by pointer so a callback swapped at
    // runtime (tests do this) is always seen.
    explicit CallbackTransport(const std::function<void(const uint8_t*, uint32_t)>* onReply)
        : mOnReply(onReply) {}

    bool send(uint32_t, const uint8_t* data, uint32_t size, bool networkOnly) override {
        if (networkOnly) return false;            // no in-process observer for snapshots
        if (mOnReply && *mOnReply) { (*mOnReply)(data, size); return true; }
        return false;
    }

    void broadcastNotify(const uint8_t* data, uint32_t size) override {
        if (mOnReply && *mOnReply) (*mOnReply)(data, size);
    }

    void broadcastLink(const uint8_t*, uint32_t) override {}  // no in-process Link audience

    bool hasNotifySubscribers() const override {
        return !mNotify.empty() || !mNotifyPorts.empty();
    }
    bool subscribeNotify(uint32_t token) override { return mNotify.insert(token).second; }
    void subscribeNotifyPort(int port) override { mNotifyPorts.insert(port); }
    void unsubscribeNotify(uint32_t token) override { mNotify.erase(token); }
    void clearNotify() override { mNotify.clear(); mNotifyPorts.clear(); }

    // An in-process caller has no port, so there is no Link-notify target.
    bool subscribeLink(uint32_t) override { return false; }
    void unsubscribeLink(uint32_t) override {}

    // MIDI notify: deliver to the single in-process observer so an
    // embedder/test can see /midi/in/* events through onReply.
    void broadcastMidi(const uint8_t* data, uint32_t size) override {
        if (mOnReply && *mOnReply) (*mOnReply)(data, size);
    }
    bool subscribeMidi(uint32_t token) override { return mMidi.insert(token).second; }
    void unsubscribeMidi(uint32_t token) override { mMidi.erase(token); }

    // Gamepad notify: deliver to the single in-process observer so an
    // embedder/test can see /gamepad/in/* events through onReply.
    void broadcastGamepad(const uint8_t* data, uint32_t size) override {
        if (mOnReply && *mOnReply) (*mOnReply)(data, size);
    }
    bool subscribeGamepad(uint32_t token) override { return mGamepad.insert(token).second; }
    void unsubscribeGamepad(uint32_t token) override { mGamepad.erase(token); }

    // OSC-cue notify: deliver to the single in-process observer so an
    // embedder/test can see /external-osc-cue events through onReply.
    void broadcastOsc(const uint8_t* data, uint32_t size) override {
        if (mOnReply && *mOnReply) (*mOnReply)(data, size);
    }
    bool subscribeOsc(uint32_t token) override { return mOsc.insert(token).second; }
    void unsubscribeOsc(uint32_t token) override { mOsc.erase(token); }

private:
    const std::function<void(const uint8_t*, uint32_t)>* mOnReply;
    std::set<uint32_t> mNotify;
    std::set<int>      mNotifyPorts;
    std::set<uint32_t> mMidi;
    std::set<uint32_t> mGamepad;
    std::set<uint32_t> mOsc;
};
