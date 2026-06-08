/*
 * OscUdpServer.h — the UDP OSC transport (port 57110).
 *
 * Ingress: binds the socket, interns each sender's (ip,port) into an opaque
 * origin token, and hands every packet to the engine (engine->ingest(data, size,
 * token)). Egress: the IOscTransport implementation — resolves a token (or a
 * subscriber audience) back to (ip,port) and writes the socket. ALL address
 * knowledge — the token table and the notify/link subscriber registries — lives
 * here; the engine deals only in tokens. The NRT gateway is the sole egress
 * caller, so the registries need no lock (only the token table, written by the
 * recv thread, takes one).
 */
#pragma once

#include "IOscTransport.h"

#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class SupersonicEngine;

class OscUdpServer : public juce::Thread, public IOscTransport {
public:
    OscUdpServer();
    ~OscUdpServer() override;

    void initialise(int port, const std::string& bindAddress = "");

    // The engine owns the ingress; the transport just delivers bytes to it.
    void setEngine(SupersonicEngine* engine) { mEngine = engine; }

    // ── IOscTransport (egress) ────────────────────────────────────────────────
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

private:
    void run() override;

    // Intern a sender's (ip,port) → a rolling, non-zero token stamped into the
    // IN-ring Message.sourceId. Each slot stores its token so resolveOrigin can
    // detect a slot reused before it was read (origin then lost → port 0).
    uint32_t internOrigin(const juce::String& ip, int port);
    void resolveOrigin(uint32_t token, juce::String& ip, int& port) const;

    // Raw socket write to one peer. Serialises writes: JUCE's DatagramSocket::write
    // is not concurrent-write safe (internal getaddrinfo/freeaddrinfo races).
    bool sendTo(const juce::String& ip, int port, const uint8_t* data, uint32_t size);

    // Subscriber registries (gateway-only → no lock). Dedup + evict-oldest.
    struct Target { juce::String ip; int port; };
    static bool addTarget(std::vector<Target>& list, const juce::String& ip, int port);
    static void removeTarget(std::vector<Target>& list, const juce::String& ip, int port);
    void        broadcast(const std::vector<Target>& list, const uint8_t* data, uint32_t size);

    int               mPort = 57110;
    std::string       mBindAddress;
    SupersonicEngine* mEngine = nullptr;

    std::unique_ptr<juce::DatagramSocket> mSocket;
    std::vector<uint8_t>                  mRecvBuf;
    juce::CriticalSection                 mSocketWriteLock;

    std::vector<Target> mNotifyTargets;
    std::vector<Target> mLinkNotifyTargets;
    std::vector<Target> mMidiNotifyTargets;

    // (ip,port) ↔ token table — the transport's address book.
    struct OriginEntry { uint32_t token = 0; char ip[64] = {}; int port = 0; };
    static constexpr uint32_t     kOriginTableSize = 1024;
    OriginEntry                   mOriginTable[kOriginTableSize];
    std::atomic<uint32_t>         mOriginCounter{0};
    mutable juce::CriticalSection mOriginLock;
};
