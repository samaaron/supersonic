/*
 * UdsDgramOscTransport.cpp — see UdsDgramOscTransport.h. Inbound via
 * ss_osc_uds_dgram_start (Rust std::os::unix::net), outbound via
 * ss_osc_uds_dgram_send; the token table + subscriber audiences are portable
 * C++. No JUCE.
 */
#include "UdsDgramOscTransport.h"

#include <algorithm>
#include <cstdio>

UdsDgramOscTransport::~UdsDgramOscTransport() {
    stop();
}

bool UdsDgramOscTransport::start() {
    if (mServer) return true;
    mServer = ss_osc_uds_dgram_start(
        this, &UdsDgramOscTransport::onDatagram,
        reinterpret_cast<const uint8_t*>(mPath.data()),
        static_cast<uint32_t>(mPath.size()));
    if (!mServer)
        fprintf(stderr, "[osc] failed to bind UDS dgram socket %s\n", mPath.c_str());
    return mServer != nullptr;
}

void UdsDgramOscTransport::stop() {
    if (mServer) { ss_osc_uds_stop(mServer); mServer = nullptr; }
}

// Rust recv thread → intern the sender's socket path → ingest carrying that
// token. An unbound sender interns the empty path: its commands flow, but the
// empty path is rejected wherever a reply address is needed.
void UdsDgramOscTransport::onDatagram(void* ctx, const uint8_t* peer, uint32_t peer_len,
                                      const uint8_t* osc, uint32_t len) {
    auto* self = static_cast<UdsDgramOscTransport*>(ctx);
    std::string path(reinterpret_cast<const char*>(peer), peer_len);
    uint32_t token = self->mOrigins.intern(path, 0);
    if (self->mIngest) self->mIngest(osc, len, token);
}

bool UdsDgramOscTransport::sendTo(const std::string& path,
                                  const uint8_t* data, uint32_t size) {
    if (!mServer || path.empty()) return false;
    return ss_osc_uds_dgram_send(mServer,
                                 reinterpret_cast<const uint8_t*>(path.data()),
                                 static_cast<uint32_t>(path.size()),
                                 data, size) != 0;
}

// ── IOscTransport: resolve a token / audience → socket path → send ───────────

bool UdsDgramOscTransport::send(uint32_t token, const uint8_t* data, uint32_t size,
                                bool /*networkOnly*/) {
    // networkOnly is moot — there is no in-process observer to skip.
    std::string path;
    int port = 0;
    if (!mOrigins.resolve(token, path, port)) return false;
    return sendTo(path, data, size);
}

void UdsDgramOscTransport::broadcast(const std::vector<std::string>& list,
                                     const uint8_t* data, uint32_t size) {
    for (const auto& p : list) sendTo(p, data, size);
}

void UdsDgramOscTransport::broadcastNotify(const uint8_t* data, uint32_t size) {
    broadcast(mNotifyTargets, data, size);
}
void UdsDgramOscTransport::broadcastLink(const uint8_t* data, uint32_t size) {
    broadcast(mLinkNotifyTargets, data, size);
}

bool UdsDgramOscTransport::subscribeNotify(uint32_t token) {
    return subscribeCallerTo(mNotifyTargets, token);
}
// Registering an explicit localhost reply *port* is a UDP concept (the GUI's
// /supersonic/devices/report path); a UDS peer subscribes caller-relative.
void UdsDgramOscTransport::subscribeNotifyPort(int /*port*/) {}
void UdsDgramOscTransport::unsubscribeNotify(uint32_t token) {
    unsubscribeCallerFrom(mNotifyTargets, token);
}
void UdsDgramOscTransport::clearNotify() { mNotifyTargets.clear(); }

// All audiences share one caller-relative registration: resolve the origin
// token, reject an unaddressable caller (unbound sender — empty path), then
// add/remove the target.
bool UdsDgramOscTransport::subscribeCallerTo(std::vector<std::string>& list, uint32_t token) {
    std::string path;
    int port = 0;
    mOrigins.resolve(token, path, port);
    return addTarget(list, path);
}
void UdsDgramOscTransport::unsubscribeCallerFrom(std::vector<std::string>& list, uint32_t token) {
    std::string path;
    int port = 0;
    mOrigins.resolve(token, path, port);
    removeTarget(list, path);
}

bool UdsDgramOscTransport::subscribeLink(uint32_t token) { return subscribeCallerTo(mLinkNotifyTargets, token); }
void UdsDgramOscTransport::unsubscribeLink(uint32_t token) { unsubscribeCallerFrom(mLinkNotifyTargets, token); }
void UdsDgramOscTransport::broadcastMidi(const uint8_t* data, uint32_t size) { broadcast(mMidiNotifyTargets, data, size); }
bool UdsDgramOscTransport::subscribeMidi(uint32_t token) { return subscribeCallerTo(mMidiNotifyTargets, token); }
void UdsDgramOscTransport::unsubscribeMidi(uint32_t token) { unsubscribeCallerFrom(mMidiNotifyTargets, token); }
void UdsDgramOscTransport::broadcastGamepad(const uint8_t* data, uint32_t size) { broadcast(mGamepadNotifyTargets, data, size); }
bool UdsDgramOscTransport::subscribeGamepad(uint32_t token) { return subscribeCallerTo(mGamepadNotifyTargets, token); }
void UdsDgramOscTransport::unsubscribeGamepad(uint32_t token) { unsubscribeCallerFrom(mGamepadNotifyTargets, token); }
void UdsDgramOscTransport::broadcastOsc(const uint8_t* data, uint32_t size) { broadcast(mOscNotifyTargets, data, size); }
bool UdsDgramOscTransport::subscribeOsc(uint32_t token) { return subscribeCallerTo(mOscNotifyTargets, token); }
void UdsDgramOscTransport::unsubscribeOsc(uint32_t token) { unsubscribeCallerFrom(mOscNotifyTargets, token); }

// Cap subscriber lists so clients that reconnect on fresh socket paths
// (restart loops) can't grow them unbounded; evict oldest first.
bool UdsDgramOscTransport::addTarget(std::vector<std::string>& list, const std::string& path) {
    if (path.empty()) return false;
    if (std::find(list.begin(), list.end(), path) != list.end()) return false;
    constexpr std::size_t kMax = 32;
    if (list.size() >= kMax) list.erase(list.begin());
    list.push_back(path);
    return true;
}

void UdsDgramOscTransport::removeTarget(std::vector<std::string>& list, const std::string& path) {
    list.erase(std::remove(list.begin(), list.end(), path), list.end());
}
