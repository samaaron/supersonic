/*
 * ReplyReader.cpp
 */
#include "ReplyReader.h"
#include <cstring>

ReplyReader::ReplyReader() : juce::Thread("SuperSonic-ReplyReader") {
    mMsgBuf.reserve(65536);
}

ReplyReader::~ReplyReader() {
    signalThreadShouldExit();
    if (mAudioProcessCount) mAudioProcessCount->notify_all();
    stopThread(2000);
}

void ReplyReader::initialise(uint8_t*               outBufferStart,
                              uint32_t               outBufferSize,
                              std::atomic<int32_t>*  outHead,
                              std::atomic<int32_t>*  outTail,
                              PerformanceMetrics*    metrics,
                              std::atomic<uint32_t>* audioProcessCount)
{
    mOutBufferStart   = outBufferStart;
    mOutBufferSize    = outBufferSize;
    mOutHead          = outHead;
    mOutTail          = outTail;
    mMetrics          = metrics;
    mAudioProcessCount = audioProcessCount;
}

void ReplyReader::drainBuffer() {
    if (!mOutBufferStart || !mOutHead || !mOutTail) return;

    while (true) {
        int32_t head = mOutHead->load(std::memory_order_acquire);
        int32_t tail = mOutTail->load(std::memory_order_relaxed);

        if (head == tail) break;

        uint32_t ut    = static_cast<uint32_t>(tail);
        uint32_t uh    = static_cast<uint32_t>(head);
        uint32_t avail = (uh - ut + mOutBufferSize) % mOutBufferSize;
        if (avail < sizeof(Message)) break;

        // Read header (wrapping)
        Message hdr;
        {
            uint32_t sz    = sizeof(Message);
            uint32_t first = mOutBufferSize - ut;
            if (sz <= first) {
                std::memcpy(&hdr, mOutBufferStart + ut, sz);
            } else {
                std::memcpy(&hdr, mOutBufferStart + ut, first);
                std::memcpy(reinterpret_cast<uint8_t*>(&hdr) + first, mOutBufferStart, sz - first);
            }
        }

        if (hdr.magic == PADDING_MAGIC) {
            mOutTail->store(0, std::memory_order_release);
            break;
        }

        if (hdr.magic != MESSAGE_MAGIC) {
            if (mMetrics) mMetrics->osc_in_corrupted.fetch_add(1, std::memory_order_relaxed);
            mOutTail->store(head, std::memory_order_release);
            break;
        }

        uint32_t totalLen = hdr.length;
        if (totalLen < sizeof(Message) || totalLen > mOutBufferSize) {
            if (mMetrics) mMetrics->osc_in_corrupted.fetch_add(1, std::memory_order_relaxed);
            mOutTail->store(head, std::memory_order_release);
            break;
        }

        if (avail < totalLen) break;

        int32_t seq = static_cast<int32_t>(hdr.sequence);
        if (mLastSeq >= 0 && seq != mLastSeq + 1) {
            if (mMetrics) mMetrics->messages_sequence_gaps.fetch_add(1, std::memory_order_relaxed);
        }
        mLastSeq = seq;

        uint32_t payloadSize  = totalLen - sizeof(Message);
        uint32_t payloadStart = (ut + sizeof(Message)) % mOutBufferSize;
        mMsgBuf.resize(payloadSize);

        {
            uint32_t first = mOutBufferSize - payloadStart;
            if (payloadSize <= first) {
                std::memcpy(mMsgBuf.data(), mOutBufferStart + payloadStart, payloadSize);
            } else {
                std::memcpy(mMsgBuf.data(), mOutBufferStart + payloadStart, first);
                std::memcpy(mMsgBuf.data() + first, mOutBufferStart, payloadSize - first);
            }
        }

        mOutTail->store(static_cast<int32_t>((ut + totalLen) % mOutBufferSize),
                        std::memory_order_release);

        if (mMetrics) {
            mMetrics->osc_in_messages_received.fetch_add(1, std::memory_order_relaxed);
            mMetrics->osc_in_bytes_received.fetch_add(payloadSize, std::memory_order_relaxed);
        }

        if (onReply && payloadSize > 0)
            onReply(mMsgBuf.data(), payloadSize);
    }
}

void ReplyReader::run() {
    if (mAudioProcessCount)
        mLastProcessCount = mAudioProcessCount->load(std::memory_order_relaxed);

    while (!threadShouldExit()) {
        if (mAudioProcessCount) {
            // C++20: block until processCount != mLastProcessCount
            // This is the exact equivalent of JS Atomics.wait()
            mAudioProcessCount->wait(mLastProcessCount);
            mLastProcessCount = mAudioProcessCount->load(std::memory_order_acquire);
        } else {
            juce::Thread::sleep(1);
        }

        if (threadShouldExit()) break;
        drainBuffer();
    }
}
