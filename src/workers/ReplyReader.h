/*
 * ReplyReader.h — Reads OSC replies from the OUT ring buffer
 */
#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>
#include "src/shared_memory.h"

class ReplyReader : public juce::Thread {
public:
    std::function<void(const uint8_t*, uint32_t)> onReply;

    ReplyReader();
    ~ReplyReader() override;

    void initialise(uint8_t*               outBufferStart,
                    uint32_t               outBufferSize,
                    std::atomic<int32_t>*  outHead,
                    std::atomic<int32_t>*  outTail,
                    PerformanceMetrics*    metrics,
                    std::atomic<uint32_t>* audioProcessCount);

private:
    void run() override;
    void drainBuffer();

    uint8_t*               mOutBufferStart     = nullptr;
    uint32_t               mOutBufferSize       = 0;
    std::atomic<int32_t>*  mOutHead             = nullptr;
    std::atomic<int32_t>*  mOutTail             = nullptr;
    PerformanceMetrics*    mMetrics             = nullptr;
    std::atomic<uint32_t>* mAudioProcessCount   = nullptr;

    uint32_t               mLastProcessCount    = 0;
    int32_t                mLastSeq             = -1;
    std::vector<uint8_t>   mMsgBuf;
};
