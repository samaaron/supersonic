/*
 * StateCache.cpp — State cache + synthdef name parser implementation
 */
#include "StateCache.h"
#include <cstring>

// --- Synthdef cache ---

void StateCache::cacheSynthDef(const std::string& name, std::vector<uint8_t> data) {
    std::lock_guard<std::mutex> lk(mMutex);
    mSynthDefs[name] = std::move(data);
}

void StateCache::uncacheSynthDef(const std::string& name) {
    std::lock_guard<std::mutex> lk(mMutex);
    mSynthDefs.erase(name);
}

void StateCache::clearSynthDefs() {
    std::lock_guard<std::mutex> lk(mMutex);
    mSynthDefs.clear();
}

std::map<std::string, std::vector<uint8_t>> StateCache::synthDefs() const {
    std::lock_guard<std::mutex> lk(mMutex);
    return mSynthDefs;
}

// --- Buffer cache ---

void StateCache::cacheBuffer(BufferMeta meta) {
    std::lock_guard<std::mutex> lk(mMutex);
    // Replace existing entry for same bufnum
    for (auto& b : mBuffers) {
        if (b.bufnum == meta.bufnum) {
            b = std::move(meta);
            return;
        }
    }
    mBuffers.push_back(std::move(meta));
}

void StateCache::uncacheBuffer(int bufnum) {
    std::lock_guard<std::mutex> lk(mMutex);
    for (auto it = mBuffers.begin(); it != mBuffers.end(); ++it) {
        if (it->bufnum == bufnum) {
            mBuffers.erase(it);
            return;
        }
    }
}

void StateCache::clearBuffers() {
    std::lock_guard<std::mutex> lk(mMutex);
    mBuffers.clear();
}

std::vector<StateCache::BufferMeta> StateCache::buffers() const {
    std::lock_guard<std::mutex> lk(mMutex);
    return mBuffers;
}

// --- Extensible modules ---

void StateCache::registerModule(Module module) {
    std::lock_guard<std::mutex> lk(mMutex);
    mModules.push_back(std::move(module));
}

void StateCache::captureAll() {
    std::lock_guard<std::mutex> lk(mMutex);
    for (auto& m : mModules) {
        if (m.capture) m.capture();
    }
}

void StateCache::restoreAll() {
    std::lock_guard<std::mutex> lk(mMutex);
    for (auto& m : mModules) {
        if (m.restore) m.restore();
    }
}

// --- SynthDef name extraction from SCgf binary ---
//
// SCgf binary format:
//   4 bytes: magic "SCgf"
//   4 bytes: version (big-endian int32)
//   2 bytes: numDefs (big-endian int16)
//   For v3: 4 bytes defSize (big-endian int32) before name
//   1 byte:  name length
//   N bytes: name (ASCII)

std::string StateCache::extractSynthDefName(const uint8_t* data, size_t size) {
    // Minimum: 4 (magic) + 4 (version) + 2 (numDefs) + 1 (nameLen) = 11
    if (!data || size < 11) return {};

    // Check magic "SCgf"
    if (std::memcmp(data, "SCgf", 4) != 0) return {};

    // Read version (big-endian int32)
    int32_t version = (static_cast<int32_t>(data[4]) << 24) |
                      (static_cast<int32_t>(data[5]) << 16) |
                      (static_cast<int32_t>(data[6]) << 8)  |
                      static_cast<int32_t>(data[7]);

    size_t offset = 10; // past magic(4) + version(4) + numDefs(2)

    // v3 has a 4-byte defSize field before the name
    if (version == 3) {
        offset += 4; // skip defSize
    }

    if (offset >= size) return {};

    uint8_t nameLen = data[offset];
    offset += 1;

    if (nameLen == 0 || offset + nameLen > size) return {};

    return std::string(reinterpret_cast<const char*>(data + offset), nameLen);
}
