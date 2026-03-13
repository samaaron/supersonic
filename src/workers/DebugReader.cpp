/*
 * DebugReader.cpp
 */
#include "DebugReader.h"
#include <cstring>

DebugReader::DebugReader() : juce::Thread("SuperSonic-DebugReader") {
    mMsgBuf.reserve(4096);
}

DebugReader::~DebugReader() {
    signalThreadShouldExit();
    if (mAudioProcessCount) mAudioProcessCount->notify_all();
    stopThread(2000);
}

void DebugReader::initialise(uint8_t*               debugBufferStart,
                              uint32_t               debugBufferSize,
                              std::atomic<int32_t>*  debugHead,
                              std::atomic<int32_t>*  debugTail,
                              PerformanceMetrics*    metrics,
                              std::atomic<uint32_t>* audioProcessCount)
{
    mDebugBufferStart  = debugBufferStart;
    mDebugBufferSize   = debugBufferSize;
    mDebugHead         = debugHead;
    mDebugTail         = debugTail;
    mMetrics           = metrics;
    mAudioProcessCount = audioProcessCount;
}

void DebugReader::drainBuffer() {
    if (!mDebugBufferStart || !mDebugHead || !mDebugTail) return;

    while (true) {
        int32_t head = mDebugHead->load(std::memory_order_acquire);
        int32_t tail = mDebugTail->load(std::memory_order_relaxed);

        if (head == tail) break;

        uint32_t ut = static_cast<uint32_t>(tail);

        if (mDebugBufferStart[ut] == DEBUG_PADDING_MARKER) {
            mDebugTail->store(0, std::memory_order_release);
            break;
        }

        uint32_t uh    = static_cast<uint32_t>(head);
        uint32_t avail = (uh - ut + mDebugBufferSize) % mDebugBufferSize;
        if (avail < sizeof(Message)) break;

        Message hdr;
        {
            uint32_t sz    = sizeof(Message);
            uint32_t first = mDebugBufferSize - ut;
            if (sz <= first) {
                std::memcpy(&hdr, mDebugBufferStart + ut, sz);
            } else {
                std::memcpy(&hdr, mDebugBufferStart + ut, first);
                std::memcpy(reinterpret_cast<uint8_t*>(&hdr) + first, mDebugBufferStart, sz - first);
            }
        }

        if (hdr.magic != MESSAGE_MAGIC) {
            mDebugTail->store(head, std::memory_order_release);
            break;
        }

        uint32_t totalLen = hdr.length;
        if (totalLen < sizeof(Message) || totalLen > mDebugBufferSize) {
            mDebugTail->store(head, std::memory_order_release);
            break;
        }

        if (avail < totalLen) break;

        uint32_t payloadSize  = totalLen - sizeof(Message);
        uint32_t payloadStart = (ut + sizeof(Message)) % mDebugBufferSize;
        mMsgBuf.resize(payloadSize + 1);

        {
            uint32_t first = mDebugBufferSize - payloadStart;
            if (payloadSize <= first) {
                std::memcpy(mMsgBuf.data(), mDebugBufferStart + payloadStart, payloadSize);
            } else {
                std::memcpy(mMsgBuf.data(), mDebugBufferStart + payloadStart, first);
                std::memcpy(mMsgBuf.data() + first, mDebugBufferStart, payloadSize - first);
            }
        }
        mMsgBuf[payloadSize] = '\0';

        mDebugTail->store(static_cast<int32_t>((ut + totalLen) % mDebugBufferSize),
                          std::memory_order_release);

        if (mMetrics) {
            mMetrics->debug_messages_received.fetch_add(1, std::memory_order_relaxed);
            mMetrics->debug_bytes_received.fetch_add(payloadSize, std::memory_order_relaxed);
        }

        if (onDebug && payloadSize > 0)
            onDebug(std::string(reinterpret_cast<char*>(mMsgBuf.data()), payloadSize));
    }
}

void DebugReader::run() {
    if (mAudioProcessCount)
        mLastProcessCount = mAudioProcessCount->load(std::memory_order_relaxed);

    while (!threadShouldExit()) {
        if (mAudioProcessCount) {
            // Block until the audio thread increments processCount — C++20 Atomics.wait()
            mAudioProcessCount->wait(mLastProcessCount);
            mLastProcessCount = mAudioProcessCount->load(std::memory_order_acquire);
        } else {
            juce::Thread::sleep(5);
        }

        if (threadShouldExit()) break;

        // Drain debug buffer every N audio events (~21ms at 48kHz/128)
        if (++mEventCount >= mEventsPerDrain) {
            mEventCount = 0;
            drainBuffer();
        }
    }
}
