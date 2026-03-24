/*
 * DebugReader.h — Reads debug text from the DEBUG ring buffer
 */
#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "src/shared_memory.h"

class DebugReader : public juce::Thread {
public:
    std::function<void(const std::string&)> onDebug;

    DebugReader();
    ~DebugReader() override;

    void initialise(uint8_t*               debugBufferStart,
                    uint32_t               debugBufferSize,
                    std::atomic<int32_t>*  debugHead,
                    std::atomic<int32_t>*  debugTail,
                    PerformanceMetrics*    metrics,
                    std::atomic<uint32_t>* audioProcessCount);

private:
    void run() override;
    void drainBuffer();

    uint8_t*               mDebugBufferStart  = nullptr;
    uint32_t               mDebugBufferSize   = 0;
    std::atomic<int32_t>*  mDebugHead         = nullptr;
    std::atomic<int32_t>*  mDebugTail         = nullptr;
    PerformanceMetrics*    mMetrics           = nullptr;
    std::atomic<uint32_t>* mAudioProcessCount = nullptr;

    uint32_t               mLastProcessCount  = 0;
    std::vector<uint8_t>   mMsgBuf;
};
