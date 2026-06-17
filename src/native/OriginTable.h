/*
 * SuperSonic
 * Copyright (c) 2025 Sam Aaron
 *
 * Dual-licensed MIT OR GPL-3.0-or-later (see repo LICENSE).
 *
 * OriginTable.h — a UDP transport's address book: maps a sender (ip,port) to a
 * stable, non-zero origin token (stamped into the IN-ring Message.sourceId) and
 * back, so a reply can be addressed to the client that sent a command.
 *
 * Client-keyed, not packet-keyed: the same (ip,port) always maps to the same
 * token, so a token never churns under traffic — it survives across a scheduled
 * event's delay. Eviction is per distinct client (LRU by last-seen) only when the
 * table is full. intern() runs on the recv thread, resolve() on the egress
 * thread, so a mutex guards the table. JUCE-free — plain std types.
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

class OriginTable {
public:
    // Map (ip,port) → a stable token (>= 1). Known client → its existing token
    // (refreshing the LRU stamp); new client → a fresh token, evicting the
    // least-recently-seen entry if the table is full.
    uint32_t intern(const std::string& ip, int port) {
        std::lock_guard<std::mutex> lk(mMutex);
        const uint64_t now = ++mClock;
        for (uint32_t i = 0; i < mUsed; ++i) {       // port-check first (cheap)
            Entry& e = mTable[i];
            if (e.port == port && e.ip == ip) { e.lastSeen = now; return e.token; }
        }
        uint32_t slot;
        if (mUsed < kSize) {
            slot = mUsed++;
        } else {
            slot = 0;
            for (uint32_t i = 1; i < kSize; ++i)
                if (mTable[i].lastSeen < mTable[slot].lastSeen) slot = i;
        }
        Entry& e = mTable[slot];
        e.token    = ++mCounter;   // >= 1
        e.lastSeen = now;
        e.port     = port;
        e.ip       = ip;
        return e.token;
    }

    // Resolve a token back to (ip,port). Returns false (ip cleared, port 0) for
    // token 0 (in-process caller) or an unknown/evicted token.
    bool resolve(uint32_t token, std::string& ip, int& port) const {
        ip.clear();
        port = 0;
        if (token == 0) return false;
        std::lock_guard<std::mutex> lk(mMutex);
        for (uint32_t i = 0; i < mUsed; ++i) {
            const Entry& e = mTable[i];
            if (e.token == token) { ip = e.ip; port = e.port; return true; }
        }
        return false;
    }

private:
    struct Entry {
        uint32_t    token    = 0;
        uint64_t    lastSeen = 0;
        std::string ip;
        int         port     = 0;
    };
    static constexpr uint32_t kSize = 1024;   // max distinct clients

    Entry              mTable[kSize];
    uint32_t           mUsed    = 0;   // entries in use (packed prefix)
    uint32_t           mCounter = 0;   // next token
    uint64_t           mClock   = 0;   // LRU stamp source
    mutable std::mutex mMutex;
};
