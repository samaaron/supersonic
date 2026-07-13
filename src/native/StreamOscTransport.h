/*
 * SuperSonic
 * Copyright (c) 2026 Sam Aaron
 *
 * Dual-licensed MIT OR GPL-3.0-or-later (see repo LICENSE).
 *
 * StreamOscTransport.h — the connection-oriented OSC transport, one class for
 * TCP, UDS stream, and Windows named pipes (they share the SsOscStream C ABI
 * and the 4-byte big-endian length-prefixed wire format).
 *
 * A connection id IS the origin token: minted from 1 by the Rust leaf, never
 * reused, dead the moment the connection closes. That collapses the datagram
 * transports' address bookkeeping — no OriginTable, no LRU eviction (an
 * established client can never be churned out by strangers), and subscription
 * lifetime == connection lifetime: on_closed prunes the client from every
 * notify audience. Admission control is the Rust leaf's max_conns cap.
 *
 * The NRT gateway is the sole caller of the IOscTransport methods, but
 * on_closed fires on a reader thread — one mutex covers the audience lists
 * for that cross-thread prune.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "IOscTransport.h"
#include "ss_osc.h"

class StreamOscTransport : public IOscTransport {
public:
    // Called per inbound packet: (osc, len, originToken). Set before start().
    using IngestFn = std::function<void(const uint8_t*, uint32_t, uint32_t)>;

    ~StreamOscTransport() override;

    void setIngest(IngestFn fn) { mIngest = std::move(fn); }
    void setMaxConnections(uint32_t n) { mMaxConns = n; }

    // Pick exactly one endpoint before start().
    void initialiseTcp(int port, const std::string& bindAddress);
    void initialiseUds(const std::string& path);   // unix only
    void initialisePipe(const std::string& name);  // Windows only

    bool start();   // listen + accept; false on failure / unsupported platform
    void stop();

    // The actual bound TCP port (differs from the requested one for port-0
    // starts); 0 for path/name-addressed servers.
    int boundPort() const;

    // ── IOscTransport ──────────────────────────────────────────────────────────
    bool send(uint32_t token, const uint8_t* data, uint32_t size, bool networkOnly) override;
    void broadcastNotify(const uint8_t* data, uint32_t size) override;
    void broadcastLink(const uint8_t* data, uint32_t size) override;
    bool hasNotifySubscribers() const override;
    bool subscribeNotify(uint32_t token) override;
    void subscribeNotifyPort(int port) override;  // UDP-specific — no-op here
    void unsubscribeNotify(uint32_t token) override;
    void clearNotify() override;
    bool subscribeLink(uint32_t token) override;
    void unsubscribeLink(uint32_t token) override;
    void broadcastMidi(const uint8_t* data, uint32_t size) override;
    bool subscribeMidi(uint32_t token) override;
    void unsubscribeMidi(uint32_t token) override;
    void broadcastGamepad(const uint8_t* data, uint32_t size) override;
    bool subscribeGamepad(uint32_t token) override;
    void unsubscribeGamepad(uint32_t token) override;
    void broadcastOsc(const uint8_t* data, uint32_t size) override;
    bool subscribeOsc(uint32_t token) override;
    void unsubscribeOsc(uint32_t token) override;

private:
    enum class Kind { None, Tcp, Uds, Pipe };

    static void onPacket(void* ctx, uint32_t conn, const uint8_t* osc, uint32_t len);
    static void onClosed(void* ctx, uint32_t conn);

    void broadcast(const std::vector<uint32_t>& list, const uint8_t* data, uint32_t size);
    bool addConn(std::vector<uint32_t>& list, uint32_t token);
    void removeConn(std::vector<uint32_t>& list, uint32_t token);

    Kind         mKind = Kind::None;
    int          mPort = 0;             // Tcp
    std::string  mBindAddress;          // Tcp
    std::string  mEndpoint;             // Uds path / pipe name
    uint32_t     mMaxConns = 4;
    SsOscStream* mServer = nullptr;
    IngestFn     mIngest;

    // Audience lists hold conn ids. Guarded by mMutex: the gateway thread
    // subscribes/broadcasts while reader threads prune via onClosed.
    mutable std::mutex    mMutex;
    std::vector<uint32_t> mNotifyTargets;
    std::vector<uint32_t> mLinkNotifyTargets;
    std::vector<uint32_t> mMidiNotifyTargets;
    std::vector<uint32_t> mGamepadNotifyTargets;
    std::vector<uint32_t> mOscNotifyTargets;
};
