/*
 * SuperSonic
 * Copyright (c) 2026 Sam Aaron
 *
 * Dual-licensed MIT OR GPL-3.0-or-later (see repo LICENSE).
 *
 * UdsDgramOscTransport.h — the UDS-datagram OSC transport: the UDP control
 * port's kernel-ACL'd sibling. Same shape as UdpOscTransport — inbound
 * datagrams are received by the Rust leaf (ss_osc_uds_dgram_*) and handed to
 * the ingest callback with an interned origin token; replies resolve the token
 * back to the sender's socket path. The socket file is created 0600; callers
 * wanting the full owner-only guarantee put it in a 0700 directory. A client
 * must bind its own path to be addressable (macOS has no autobind) — an
 * unbound sender's commands are ingested but its replies/subscriptions are
 * rejected.
 *
 * Unix only: on Windows start() fails (the Rust leaf returns null) and the
 * named-pipe transport (StreamOscTransport) is the analogue.
 *
 * The NRT gateway is the sole caller of the IOscTransport methods (one
 * thread); inbound runs on the Rust recv thread (the OriginTable's mutex
 * covers the cross-thread intern/resolve; audiences are gateway-only).
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "IOscTransport.h"
#include "OriginTable.h"
#include "ss_osc.h"

class UdsDgramOscTransport : public IOscTransport {
public:
    // Called per inbound datagram: (osc, len, originToken). Set before start().
    using IngestFn = std::function<void(const uint8_t*, uint32_t, uint32_t)>;

    ~UdsDgramOscTransport() override;

    void setIngest(IngestFn fn) { mIngest = std::move(fn); }
    void initialise(const std::string& path) { mPath = path; }
    bool start();   // bind + begin receiving; false on failure (always, on Windows)
    void stop();

    // ── IOscTransport ──────────────────────────────────────────────────────────
    bool send(uint32_t token, const uint8_t* data, uint32_t size, bool networkOnly) override;
    void broadcastNotify(const uint8_t* data, uint32_t size) override;
    void broadcastLink(const uint8_t* data, uint32_t size) override;
    bool hasNotifySubscribers() const override { return !mNotifyTargets.empty(); }
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
    static void onDatagram(void* ctx, const uint8_t* peer, uint32_t peer_len,
                           const uint8_t* osc, uint32_t len);

    bool sendTo(const std::string& path, const uint8_t* data, uint32_t size);
    void broadcast(const std::vector<std::string>& list, const uint8_t* data, uint32_t size);
    bool subscribeCallerTo(std::vector<std::string>& list, uint32_t token);
    void unsubscribeCallerFrom(std::vector<std::string>& list, uint32_t token);
    static bool addTarget(std::vector<std::string>& list, const std::string& path);
    static void removeTarget(std::vector<std::string>& list, const std::string& path);

    std::string mPath;
    SsOscUds*   mServer = nullptr;
    IngestFn    mIngest;

    // Peer socket paths are interned as (path, port=0); an empty path marks an
    // unaddressable (unbound) sender.
    OriginTable mOrigins;

    std::vector<std::string> mNotifyTargets;
    std::vector<std::string> mLinkNotifyTargets;
    std::vector<std::string> mMidiNotifyTargets;
    std::vector<std::string> mGamepadNotifyTargets;
    std::vector<std::string> mOscNotifyTargets;
};
