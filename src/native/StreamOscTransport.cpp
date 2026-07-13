/*
 * StreamOscTransport.cpp — see StreamOscTransport.h. Listening, framing, and
 * the connection registry live in the Rust leaf (ss_osc stream API); this
 * side is the audience bookkeeping and the IOscTransport seam. No JUCE.
 */
#include "StreamOscTransport.h"

#include <algorithm>
#include <cstdio>

StreamOscTransport::~StreamOscTransport() {
    stop();
}

void StreamOscTransport::initialiseTcp(int port, const std::string& bindAddress) {
    mKind = Kind::Tcp;
    mPort = port;
    mBindAddress = bindAddress;
}

void StreamOscTransport::initialiseUds(const std::string& path) {
    mKind = Kind::Uds;
    mEndpoint = path;
}

void StreamOscTransport::initialisePipe(const std::string& name) {
    mKind = Kind::Pipe;
    mEndpoint = name;
}

bool StreamOscTransport::start() {
    if (mServer) return true;
    const auto* ep = reinterpret_cast<const uint8_t*>(mEndpoint.data());
    const auto epLen = static_cast<uint32_t>(mEndpoint.size());
    switch (mKind) {
    case Kind::Tcp:
        mServer = ss_osc_tcp_start(this, &onPacket, &onClosed, mPort,
                                   reinterpret_cast<const uint8_t*>(mBindAddress.data()),
                                   static_cast<uint32_t>(mBindAddress.size()), mMaxConns);
        if (!mServer)
            fprintf(stderr, "[osc] failed to bind TCP command port %d\n", mPort);
        break;
    case Kind::Uds:
        mServer = ss_osc_uds_stream_start(this, &onPacket, &onClosed, ep, epLen, mMaxConns);
        if (!mServer)
            fprintf(stderr, "[osc] failed to bind UDS stream socket %s\n", mEndpoint.c_str());
        break;
    case Kind::Pipe:
        mServer = ss_osc_pipe_start(this, &onPacket, &onClosed, ep, epLen, mMaxConns);
        if (!mServer)
            fprintf(stderr, "[osc] failed to create named pipe %s\n", mEndpoint.c_str());
        break;
    case Kind::None:
        fprintf(stderr, "[osc] stream transport started without an endpoint\n");
        break;
    }
    return mServer != nullptr;
}

void StreamOscTransport::stop() {
    if (mServer) { ss_osc_stream_stop(mServer); mServer = nullptr; }
}

int StreamOscTransport::boundPort() const {
    return mServer ? ss_osc_stream_port(mServer) : 0;
}

// Rust reader thread → ingest carrying the connection id as the origin token.
void StreamOscTransport::onPacket(void* ctx, uint32_t conn, const uint8_t* osc, uint32_t len) {
    auto* self = static_cast<StreamOscTransport*>(ctx);
    if (self->mIngest) self->mIngest(osc, len, conn);
}

// Rust reader thread, once per ended connection: subscription lifetime ==
// connection lifetime, so the client leaves every audience.
void StreamOscTransport::onClosed(void* ctx, uint32_t conn) {
    auto* self = static_cast<StreamOscTransport*>(ctx);
    std::lock_guard<std::mutex> lk(self->mMutex);
    self->removeConn(self->mNotifyTargets, conn);
    self->removeConn(self->mLinkNotifyTargets, conn);
    self->removeConn(self->mMidiNotifyTargets, conn);
    self->removeConn(self->mGamepadNotifyTargets, conn);
    self->removeConn(self->mOscNotifyTargets, conn);
}

// ── IOscTransport: a token is a conn id — hand it straight to the leaf ───────

bool StreamOscTransport::send(uint32_t token, const uint8_t* data, uint32_t size,
                              bool /*networkOnly*/) {
    // networkOnly is moot — there is no in-process observer to skip.
    if (!mServer || token == 0) return false;
    return ss_osc_stream_send(mServer, token, data, size) != 0;
}

void StreamOscTransport::broadcast(const std::vector<uint32_t>& list,
                                   const uint8_t* data, uint32_t size) {
    if (!mServer) return;
    // Copy the audience out so a send never runs under the audience lock (a
    // reader thread pruning via onClosed must not wait on a slow write).
    std::vector<uint32_t> targets;
    {
        std::lock_guard<std::mutex> lk(mMutex);
        targets = list;
    }
    for (uint32_t conn : targets) ss_osc_stream_send(mServer, conn, data, size);
}

void StreamOscTransport::broadcastNotify(const uint8_t* data, uint32_t size) {
    broadcast(mNotifyTargets, data, size);
}
void StreamOscTransport::broadcastLink(const uint8_t* data, uint32_t size) {
    broadcast(mLinkNotifyTargets, data, size);
}

bool StreamOscTransport::hasNotifySubscribers() const {
    std::lock_guard<std::mutex> lk(mMutex);
    return !mNotifyTargets.empty();
}

bool StreamOscTransport::subscribeNotify(uint32_t token) {
    std::lock_guard<std::mutex> lk(mMutex);
    return addConn(mNotifyTargets, token);
}
// Registering an explicit localhost reply *port* is a UDP concept (the GUI's
// /supersonic/devices/report path); a stream peer subscribes caller-relative.
void StreamOscTransport::subscribeNotifyPort(int /*port*/) {}
void StreamOscTransport::unsubscribeNotify(uint32_t token) {
    std::lock_guard<std::mutex> lk(mMutex);
    removeConn(mNotifyTargets, token);
}
void StreamOscTransport::clearNotify() {
    std::lock_guard<std::mutex> lk(mMutex);
    mNotifyTargets.clear();
}

bool StreamOscTransport::subscribeLink(uint32_t token) {
    std::lock_guard<std::mutex> lk(mMutex);
    return addConn(mLinkNotifyTargets, token);
}
void StreamOscTransport::unsubscribeLink(uint32_t token) {
    std::lock_guard<std::mutex> lk(mMutex);
    removeConn(mLinkNotifyTargets, token);
}
void StreamOscTransport::broadcastMidi(const uint8_t* data, uint32_t size) {
    broadcast(mMidiNotifyTargets, data, size);
}
bool StreamOscTransport::subscribeMidi(uint32_t token) {
    std::lock_guard<std::mutex> lk(mMutex);
    return addConn(mMidiNotifyTargets, token);
}
void StreamOscTransport::unsubscribeMidi(uint32_t token) {
    std::lock_guard<std::mutex> lk(mMutex);
    removeConn(mMidiNotifyTargets, token);
}
void StreamOscTransport::broadcastGamepad(const uint8_t* data, uint32_t size) {
    broadcast(mGamepadNotifyTargets, data, size);
}
bool StreamOscTransport::subscribeGamepad(uint32_t token) {
    std::lock_guard<std::mutex> lk(mMutex);
    return addConn(mGamepadNotifyTargets, token);
}
void StreamOscTransport::unsubscribeGamepad(uint32_t token) {
    std::lock_guard<std::mutex> lk(mMutex);
    removeConn(mGamepadNotifyTargets, token);
}
void StreamOscTransport::broadcastOsc(const uint8_t* data, uint32_t size) {
    broadcast(mOscNotifyTargets, data, size);
}
bool StreamOscTransport::subscribeOsc(uint32_t token) {
    std::lock_guard<std::mutex> lk(mMutex);
    return addConn(mOscNotifyTargets, token);
}
void StreamOscTransport::unsubscribeOsc(uint32_t token) {
    std::lock_guard<std::mutex> lk(mMutex);
    removeConn(mOscNotifyTargets, token);
}

// Audience size is naturally bounded: ids are live connections (≤ max_conns)
// and onClosed prunes the dead — no LRU eviction needed. Callers hold mMutex.
bool StreamOscTransport::addConn(std::vector<uint32_t>& list, uint32_t token) {
    if (token == 0) return false;  // the in-process caller has no connection
    if (std::find(list.begin(), list.end(), token) != list.end()) return false;
    list.push_back(token);
    return true;
}

void StreamOscTransport::removeConn(std::vector<uint32_t>& list, uint32_t token) {
    list.erase(std::remove(list.begin(), list.end(), token), list.end());
}
