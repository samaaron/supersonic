/*
 * SuperSonic
 * Copyright (c) 2025 Sam Aaron
 *
 * Dual-licensed MIT OR GPL-3.0-or-later (see repo LICENSE).
 *
 * UdpOscTransport.h — the UDP OSC transport, JUCE-free. Inbound datagrams are
 * received by the Rust std::net subsystem (ss_osc) and handed to the ingest
 * callback carrying an interned origin token; outbound replies/broadcasts go back
 * out through ss_osc_send. The token address-book (OriginTable) and the notify
 * subscriber audiences are the transport's portable, dual-licensed address book;
 * only the actual sockets live in the Rust leaf.
 *
 * The NRT gateway is the sole caller of the IOscTransport methods (one thread);
 * inbound runs on the Rust recv threads (the OriginTable's mutex covers the
 * cross-thread intern/resolve).
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "IOscTransport.h"
#include "OriginTable.h"
#include "ss_osc.h"

class UdpOscTransport : public IOscTransport {
public:
    // Called per inbound datagram: (osc, len, originToken). Set before start().
    using IngestFn = std::function<void(const uint8_t*, uint32_t, uint32_t)>;

    UdpOscTransport();
    ~UdpOscTransport() override;

    void setIngest(IngestFn fn) { mIngest = std::move(fn); }
    void initialise(int port, const std::string& bindAddress = "") {
        mPort = port;
        mBindAddress = bindAddress;
    }
    void start();   // begin receiving (ss_osc ingress on mPort/mBindAddress)
    void stop();    // stop receiving

    // ── IOscTransport ──────────────────────────────────────────────────────────
    bool send(uint32_t token, const uint8_t* data, uint32_t size, bool networkOnly) override;
    void broadcastNotify(const uint8_t* data, uint32_t size) override;
    void broadcastLink(const uint8_t* data, uint32_t size) override;
    bool hasNotifySubscribers() const override { return !mNotifyTargets.empty(); }
    bool subscribeNotify(uint32_t token) override;
    void subscribeNotifyPort(int port) override;
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
    struct Target { std::string ip; int port; };

    static void onDatagram(void* ctx, const uint8_t* ip, uint32_t ip_len,
                           int32_t port, const uint8_t* osc, uint32_t len);

    void sendTo(const std::string& ip, int port, const uint8_t* data, uint32_t size);
    void broadcast(const std::vector<Target>& list, const uint8_t* data, uint32_t size);
    bool subscribeCallerTo(std::vector<Target>& list, uint32_t token);
    void unsubscribeCallerFrom(std::vector<Target>& list, uint32_t token);
    static bool addTarget(std::vector<Target>& list, const std::string& ip, int port);
    static void removeTarget(std::vector<Target>& list, const std::string& ip, int port);

    int           mPort = 57110;
    std::string   mBindAddress;
    IngestFn      mIngest;
    SsOsc*        mOsc     = nullptr;   // outbound send handle
    SsOscIngress* mIngress = nullptr;   // inbound recv (owns the Rust recv threads)
    OriginTable   mOrigins;

    std::vector<Target> mNotifyTargets;
    std::vector<Target> mLinkNotifyTargets;
    std::vector<Target> mMidiNotifyTargets;
    std::vector<Target> mGamepadNotifyTargets;
    std::vector<Target> mOscNotifyTargets;
};
