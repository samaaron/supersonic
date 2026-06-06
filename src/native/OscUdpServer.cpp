/*
 * OscUdpServer.cpp — the UDP transport. Inbound bytes go to engine->ingest;
 * outbound bytes are delivered through the IOscTransport methods (token/audience
 * → (ip,port) → socket). The subscriber registries and the token table are the
 * transport's address book — see OscUdpServer.h.
 */
#include "OscUdpServer.h"
#include "SupersonicEngine.h"

#include <algorithm>

OscUdpServer::OscUdpServer()
    : juce::Thread("SuperSonic-OscUdpServer")
{
    mRecvBuf.resize(65536);
}

OscUdpServer::~OscUdpServer() {
    signalThreadShouldExit();
    if (mSocket) mSocket->shutdown();
    stopThread(2000);
}

void OscUdpServer::initialise(int port, const std::string& bindAddress) {
    mPort        = port;
    mBindAddress = bindAddress;
}

bool OscUdpServer::sendTo(const juce::String& ip, int port,
                          const uint8_t* data, uint32_t size) {
    if (!mSocket || ip.isEmpty() || port <= 0) return false;
    juce::ScopedLock lk(mSocketWriteLock);
    mSocket->write(ip, port, data, static_cast<int>(size));
    return true;
}

// ── IOscTransport: resolve a token / audience → (ip,port) → socket ───────────

bool OscUdpServer::send(uint32_t token, const uint8_t* data, uint32_t size,
                        bool /*networkOnly*/) {
    // networkOnly is moot for UDP — there is no in-process observer to skip.
    juce::String ip;
    int port = 0;
    resolveOrigin(token, ip, port);
    return sendTo(ip, port, data, size);
}

void OscUdpServer::broadcastNotify(const uint8_t* data, uint32_t size) {
    broadcast(mNotifyTargets, data, size);
}

void OscUdpServer::broadcastLink(const uint8_t* data, uint32_t size) {
    broadcast(mLinkNotifyTargets, data, size);
}

void OscUdpServer::broadcast(const std::vector<Target>& list,
                             const uint8_t* data, uint32_t size) {
    for (const auto& t : list) sendTo(t.ip, t.port, data, size);
}

bool OscUdpServer::subscribeNotify(uint32_t token) {
    juce::String ip;
    int port = 0;
    resolveOrigin(token, ip, port);
    return addTarget(mNotifyTargets, ip, port);
}

void OscUdpServer::subscribeNotifyPort(int port) {
    addTarget(mNotifyTargets, "127.0.0.1", port);
}

void OscUdpServer::unsubscribeNotify(uint32_t token) {
    juce::String ip;
    int port = 0;
    resolveOrigin(token, ip, port);
    removeTarget(mNotifyTargets, ip, port);
}

void OscUdpServer::clearNotify() {
    mNotifyTargets.clear();
}

bool OscUdpServer::subscribeLink(uint32_t token) {
    juce::String ip;
    int port = 0;
    resolveOrigin(token, ip, port);
    if (port <= 0) return false;       // an unaddressable caller can't be a Link target
    addTarget(mLinkNotifyTargets, ip, port);
    return true;
}

void OscUdpServer::unsubscribeLink(uint32_t token) {
    juce::String ip;
    int port = 0;
    resolveOrigin(token, ip, port);
    if (port <= 0) return;
    removeTarget(mLinkNotifyTargets, ip, port);
}

// Cap subscriber lists so clients that reconnect on fresh ephemeral ports
// (restart loops) can't grow them unbounded; evict oldest first.
bool OscUdpServer::addTarget(std::vector<Target>& list, const juce::String& ip, int port) {
    for (auto& t : list)
        if (t.ip == ip && t.port == port) return false;
    constexpr std::size_t kMax = 32;
    if (list.size() >= kMax) list.erase(list.begin());
    list.push_back({ip, port});
    return true;
}

void OscUdpServer::removeTarget(std::vector<Target>& list, const juce::String& ip, int port) {
    list.erase(std::remove_if(list.begin(), list.end(),
                   [&](const Target& t) { return t.ip == ip && t.port == port; }),
               list.end());
}

// --- Origin token table (the transport's address book) -----------------------
// internOrigin runs on the recv thread (this thread); resolveOrigin on the NRT
// reader thread — different threads, so the table is guarded by mOriginLock.

uint32_t OscUdpServer::internOrigin(const juce::String& ip, int port) {
    uint32_t token = mOriginCounter.fetch_add(1, std::memory_order_relaxed) + 1;  // >= 1
    juce::ScopedLock sl(mOriginLock);
    OriginEntry& e = mOriginTable[token % kOriginTableSize];
    e.token = token;
    e.port  = port;
    ip.copyToUTF8(e.ip, sizeof(e.ip));
    return token;
}

void OscUdpServer::resolveOrigin(uint32_t token, juce::String& ip, int& port) const {
    if (token == 0) { ip = juce::String(); port = 0; return; }  // in-process caller
    juce::ScopedLock sl(mOriginLock);
    const OriginEntry& e = mOriginTable[token % kOriginTableSize];
    if (e.token != token) { ip = juce::String(); port = 0; return; }  // slot reused, origin lost
    ip   = juce::String::fromUTF8(e.ip);
    port = e.port;
}

void OscUdpServer::run() {
    mSocket = std::make_unique<juce::DatagramSocket>();

    if (!mSocket->bindToPort(mPort, mBindAddress.empty() ? juce::String() : juce::String(mBindAddress))) {
        fprintf(stderr, "[osc] failed to bind to port %d\n", mPort);
        return;
    }

    while (!threadShouldExit()) {
        // Wait up to 100ms for a packet, then loop to re-check
        // threadShouldExit(). A bare non-blocking read here pegged a CPU
        // core; a fully blocking read won't wake on shutdown because
        // ::shutdown() on macOS doesn't reliably unblock a recvfrom on
        // a UDP socket (see JUCE comment in juce_Socket.cpp::closeSocket
        // about shutdown not unblocking select on macOS — the same
        // applies to recvfrom in practice). The timed wait + non-blocking
        // read pattern gives ~0% idle CPU and ≤100ms shutdown latency.
        const int ready = mSocket->waitUntilReady(true, 100);
        if (ready < 0) break;       // socket error / shutdown
        if (ready == 0) continue;   // timeout — re-check threadShouldExit

        juce::String senderIP;
        int senderPort = 0;

        int bytesRead = mSocket->read(mRecvBuf.data(),
                                       static_cast<int>(mRecvBuf.size()),
                                       false, senderIP, senderPort);

        if (bytesRead > 0) {
            // Intern the sender → an opaque token, and hand the packet to the
            // engine's ingress carrying only that token. The engine resolves it
            // back to (ip,port) via resolveOrigin when it needs to reply.
            try {
                if (mEngine)
                    mEngine->ingest(mRecvBuf.data(),
                                    static_cast<uint32_t>(bytesRead),
                                    internOrigin(senderIP, senderPort));
            } catch (const std::exception& e) {
                fprintf(stderr, "[osc] exception in ingest: %s\n", e.what());
            } catch (...) {
                fprintf(stderr, "[osc] unknown exception in ingest\n");
            }
        } else if (bytesRead < 0) {
            if (threadShouldExit()) break;
            juce::Thread::sleep(1);
        }
    }

    mSocket.reset();
}
