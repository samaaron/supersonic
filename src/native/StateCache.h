/*
 * StateCache.h — Caches synthdef binaries and buffer metadata for cold swap
 */
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

class StateCache {
public:
    // --- Synthdef cache ---
    void cacheSynthDef(const std::string& name, std::vector<uint8_t> data);
    void uncacheSynthDef(const std::string& name);
    void clearSynthDefs();
    std::map<std::string, std::vector<uint8_t>> synthDefs() const;

    // --- Buffer cache ---
    struct BufferMeta {
        int         bufnum;
        std::string path;
        int         startFrame;
        int         numFrames;
        int         numChannels;
        int         sampleRate;
    };
    void cacheBuffer(BufferMeta meta);
    void uncacheBuffer(int bufnum);
    void clearBuffers();
    std::vector<BufferMeta> buffers() const;

    // --- Extensible modules (tau hooks in here later) ---
    struct Module {
        std::string name;
        std::function<void()> capture;
        std::function<void()> restore;
    };
    void registerModule(Module module);
    void captureAll();   // calls capture() on each registered module
    void restoreAll();   // calls restore() on each registered module

    // --- SynthDef name extraction from SCgf binary ---
    static std::string extractSynthDefName(const uint8_t* data, size_t size);

private:
    mutable std::mutex                          mMutex;
    std::map<std::string, std::vector<uint8_t>> mSynthDefs;
    std::vector<BufferMeta>                     mBuffers;
    std::vector<Module>                         mModules;
};
