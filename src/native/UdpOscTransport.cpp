/*
 * UdpOscTransport.cpp — see UdpOscTransport.h. Inbound via ss_osc_ingress (Rust
 * std::net), outbound via ss_osc_send; the token table + subscriber audiences are
 * portable C++. No JUCE.
 */
#include "UdpOscTransport.h"

#include <algorithm>
#include <cstdio>

namespace {
// ss_osc_create needs an inbound emit callback for its cue server, which this
// transport never configures — so a no-op satisfies the ABI.
void cueEmitNoop(void*, int32_t, const uint8_t*, uint32_t) {}
}  // namespace

UdpOscTransport::UdpOscTransport() {
    mOsc = ss_osc_create(nullptr, &cueEmitNoop);  // outbound sockets for ss_osc_send
}

UdpOscTransport::~UdpOscTransport() {
    stop();
    if (mOsc) ss_osc_destroy(mOsc);
}

void UdpOscTransport::start() {
    if (mIngress) return;
    mIngress = ss_osc_ingress_start_with_src(
        this, &UdpOscTransport::onDatagram, mPort,
        reinterpret_cast<const uint8_t*>(mBindAddress.data()),
        static_cast<uint32_t>(mBindAddress.size()));
    if (!mIngress)
        fprintf(stderr, "[osc] failed to bind control port %d\n", mPort);
}

void UdpOscTransport::stop() {
    if (mIngress) { ss_osc_ingress_stop(mIngress); mIngress = nullptr; }
}

// Rust recv thread → intern the sender → ingest the packet carrying that token.
void UdpOscTransport::onDatagram(void* ctx, const uint8_t* ip, uint32_t ip_len,
                                 int32_t port, const uint8_t* osc, uint32_t len) {
    auto* self = static_cast<UdpOscTransport*>(ctx);
    std::string ipStr(reinterpret_cast<const char*>(ip), ip_len);
    uint32_t token = self->mOrigins.intern(ipStr, port);
    if (self->mIngest) self->mIngest(osc, len, token);
}

void UdpOscTransport::sendTo(const std::string& ip, int port,
                             const uint8_t* data, uint32_t size) {
    if (!mOsc || ip.empty() || port <= 0) return;
    ss_osc_send(mOsc, reinterpret_cast<const uint8_t*>(ip.data()),
                static_cast<uint32_t>(ip.size()), port, data, size);
}

// ── IOscTransport: resolve a token / audience → (ip,port) → ss_osc_send ───────

bool UdpOscTransport::send(uint32_t token, const uint8_t* data, uint32_t size,
                           bool /*networkOnly*/) {
    // networkOnly is moot for UDP — there is no in-process observer to skip.
    std::string ip;
    int port = 0;
    if (!mOrigins.resolve(token, ip, port)) return false;
    sendTo(ip, port, data, size);
    return true;
}

void UdpOscTransport::broadcast(const std::vector<Target>& list,
                                const uint8_t* data, uint32_t size) {
    for (const auto& t : list) sendTo(t.ip, t.port, data, size);
}

void UdpOscTransport::broadcastNotify(const uint8_t* data, uint32_t size) {
    broadcast(mNotifyTargets, data, size);
}
void UdpOscTransport::broadcastLink(const uint8_t* data, uint32_t size) {
    broadcast(mLinkNotifyTargets, data, size);
}

bool UdpOscTransport::subscribeNotify(uint32_t token) {
    std::string ip;
    int port = 0;
    mOrigins.resolve(token, ip, port);
    return addTarget(mNotifyTargets, ip, port);
}
void UdpOscTransport::subscribeNotifyPort(int port) {
    addTarget(mNotifyTargets, "127.0.0.1", port);
}
void UdpOscTransport::unsubscribeNotify(uint32_t token) {
    std::string ip;
    int port = 0;
    mOrigins.resolve(token, ip, port);
    removeTarget(mNotifyTargets, ip, port);
}
void UdpOscTransport::clearNotify() { mNotifyTargets.clear(); }

// The per-subsystem notify audiences (Link / MIDI / gamepad / osc) share one
// caller-relative registration: resolve the origin token, reject an unaddressable
// caller (in-process, no port), then add/remove the target.
bool UdpOscTransport::subscribeCallerTo(std::vector<Target>& list, uint32_t token) {
    std::string ip;
    int port = 0;
    mOrigins.resolve(token, ip, port);
    if (port <= 0) return false;
    addTarget(list, ip, port);
    return true;
}
void UdpOscTransport::unsubscribeCallerFrom(std::vector<Target>& list, uint32_t token) {
    std::string ip;
    int port = 0;
    mOrigins.resolve(token, ip, port);
    if (port <= 0) return;
    removeTarget(list, ip, port);
}

bool UdpOscTransport::subscribeLink(uint32_t token) { return subscribeCallerTo(mLinkNotifyTargets, token); }
void UdpOscTransport::unsubscribeLink(uint32_t token) { unsubscribeCallerFrom(mLinkNotifyTargets, token); }
void UdpOscTransport::broadcastMidi(const uint8_t* data, uint32_t size) { broadcast(mMidiNotifyTargets, data, size); }
bool UdpOscTransport::subscribeMidi(uint32_t token) { return subscribeCallerTo(mMidiNotifyTargets, token); }
void UdpOscTransport::unsubscribeMidi(uint32_t token) { unsubscribeCallerFrom(mMidiNotifyTargets, token); }
void UdpOscTransport::broadcastGamepad(const uint8_t* data, uint32_t size) { broadcast(mGamepadNotifyTargets, data, size); }
bool UdpOscTransport::subscribeGamepad(uint32_t token) { return subscribeCallerTo(mGamepadNotifyTargets, token); }
void UdpOscTransport::unsubscribeGamepad(uint32_t token) { unsubscribeCallerFrom(mGamepadNotifyTargets, token); }
void UdpOscTransport::broadcastOsc(const uint8_t* data, uint32_t size) { broadcast(mOscNotifyTargets, data, size); }
bool UdpOscTransport::subscribeOsc(uint32_t token) { return subscribeCallerTo(mOscNotifyTargets, token); }
void UdpOscTransport::unsubscribeOsc(uint32_t token) { unsubscribeCallerFrom(mOscNotifyTargets, token); }

// Cap subscriber lists so clients that reconnect on fresh ephemeral ports
// (restart loops) can't grow them unbounded; evict oldest first.
bool UdpOscTransport::addTarget(std::vector<Target>& list, const std::string& ip, int port) {
    if (ip.empty() || port <= 0) return false;
    for (auto& t : list)
        if (t.ip == ip && t.port == port) return false;
    constexpr std::size_t kMax = 32;
    if (list.size() >= kMax) list.erase(list.begin());
    list.push_back({ip, port});
    return true;
}

void UdpOscTransport::removeTarget(std::vector<Target>& list, const std::string& ip, int port) {
    list.erase(std::remove_if(list.begin(), list.end(),
                   [&](const Target& t) { return t.ip == ip && t.port == port; }),
               list.end());
}
