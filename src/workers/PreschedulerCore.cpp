/*
 * PreschedulerCore.cpp — see PreschedulerCore.h. Behaviour is pinned by
 * test/vectors/prescheduler.json.
 */
#include "PreschedulerCore.h"

namespace supersonic {

uint64_t PreschedulerTagHash(const char* tag) {
    uint64_t h = 1469598103934665603ull; // FNV-1a 64 offset basis
    if (tag) {
        for (; *tag; ++tag) {
            h ^= static_cast<unsigned char>(*tag);
            h *= 1099511628211ull;        // FNV-1a 64 prime
        }
    }
    return h;
}

PreschedulerCore::PreschedulerCore(PreschedulerEvent* storage, uint32_t capacity,
                                   const PreschedulerConfig& cfg)
    : mStorage(storage), mCapacity(capacity) {
    setConfig(cfg);
}

void PreschedulerCore::setConfig(const PreschedulerConfig& cfg) {
    mCfg = cfg;
    // The heap can never exceed its backing array; the cap is the policy.
    if (mCfg.maxPending > mCapacity) mCfg.maxPending = mCapacity;
}

PreschedulerStatus PreschedulerCore::schedule(const PreschedulerRequest& req, double now,
                                              PreschedulerEvent* outScheduled) {
    // Precedence: backpressure, then immediate, then the size/horizon
    // rejections, then enqueue.
    if (mCount + retryCount >= mCfg.maxPending) return PreschedulerStatus::RejectQueueFull;
    if (!req.hasTime)                            return PreschedulerStatus::Immediate;
    if (req.bytes > mCfg.poolBytes)              return PreschedulerStatus::RejectTooLarge;
    if (req.ntpTime - now > mCfg.maxFutureS)     return PreschedulerStatus::RejectTooFarFuture;

    PreschedulerEvent e;
    e.ntpTime   = req.ntpTime;
    e.seq       = mSeq++;
    e.sessionId = req.sessionId;
    e.tagId     = req.tagId;
    e.bytes     = req.bytes;
    e.payload   = req.payload;
    push(e);
    if (outScheduled) *outScheduled = e;
    return PreschedulerStatus::Scheduled;
}

bool PreschedulerCore::nextDueTime(double* out) const {
    if (mCount == 0) return false;
    if (out) *out = mStorage[0].ntpTime - mCfg.lookaheadS;
    return true;
}

uint32_t PreschedulerCore::dispatchDue(double now, PreschedulerSink sink, void* ctx) {
    const double lookaheadTime = now + mCfg.lookaheadS;
    uint32_t n = 0;
    while (mCount > 0 && mStorage[0].ntpTime <= lookaheadTime) {
        PreschedulerEvent e = pop();
        sink(e, ctx);
        ++n;
    }
    return n;
}

uint32_t PreschedulerCore::cancelTag(uint64_t tagId, PreschedulerOnRemoved onRemoved, void* ctx) {
    return cancelMatching(Match::Tag, 0, tagId, onRemoved, ctx);
}

uint32_t PreschedulerCore::cancelSession(uint32_t sessionId, PreschedulerOnRemoved onRemoved, void* ctx) {
    return cancelMatching(Match::Session, sessionId, 0, onRemoved, ctx);
}

uint32_t PreschedulerCore::cancelSessionTag(uint32_t sessionId, uint64_t tagId,
                                            PreschedulerOnRemoved onRemoved, void* ctx) {
    return cancelMatching(Match::SessionTag, sessionId, tagId, onRemoved, ctx);
}

uint32_t PreschedulerCore::cancelAll(PreschedulerOnRemoved onRemoved, void* ctx) {
    const uint32_t removed = mCount;
    if (onRemoved) {
        for (uint32_t i = 0; i < mCount; ++i) onRemoved(mStorage[i], ctx);
    }
    mCount = 0;
    return removed;
}

uint32_t PreschedulerCore::cancelMatching(Match match, uint32_t sessionId, uint64_t tagId,
                                          PreschedulerOnRemoved onRemoved, void* ctx) {
    if (mCount == 0) return 0;

    // Compact survivors in place (stable), reporting each removal, then
    // re-heapify the survivors.
    uint32_t kept = 0;
    for (uint32_t i = 0; i < mCount; ++i) {
        const PreschedulerEvent& e = mStorage[i];
        bool matched = false;
        switch (match) {
            case Match::Tag:        matched = (e.tagId == tagId); break;
            case Match::Session:    matched = (e.sessionId == sessionId); break;
            case Match::SessionTag: matched = (e.sessionId == sessionId && e.tagId == tagId); break;
        }
        if (matched) {
            if (onRemoved) onRemoved(e, ctx);
        } else {
            mStorage[kept++] = e;
        }
    }

    const uint32_t removed = mCount - kept;
    if (removed > 0) {
        mCount = kept;
        heapify();
    }
    return removed;
}

// --- min-heap internals ----------------------------------------------------

bool PreschedulerCore::cmpLess(const PreschedulerEvent& a, const PreschedulerEvent& b) const {
    if (a.ntpTime == b.ntpTime) return a.seq < b.seq;
    return a.ntpTime < b.ntpTime;
}

void PreschedulerCore::swap(uint32_t i, uint32_t j) {
    PreschedulerEvent t = mStorage[i];
    mStorage[i] = mStorage[j];
    mStorage[j] = t;
}

void PreschedulerCore::push(const PreschedulerEvent& e) {
    mStorage[mCount] = e;          // capacity guaranteed by the maxPending cap
    siftUp(mCount);
    ++mCount;
}

PreschedulerEvent PreschedulerCore::pop() {
    PreschedulerEvent top = mStorage[0];
    --mCount;
    if (mCount > 0) {
        mStorage[0] = mStorage[mCount];
        siftDown(0);
    }
    return top;
}

void PreschedulerCore::siftUp(uint32_t i) {
    while (i > 0) {
        uint32_t p = (i - 1) >> 1;
        if (!cmpLess(mStorage[i], mStorage[p])) break;
        swap(i, p);
        i = p;
    }
}

void PreschedulerCore::siftDown(uint32_t i) {
    for (;;) {
        uint32_t l = 2 * i + 1, r = 2 * i + 2, s = i;
        if (l < mCount && cmpLess(mStorage[l], mStorage[s])) s = l;
        if (r < mCount && cmpLess(mStorage[r], mStorage[s])) s = r;
        if (s == i) break;
        swap(i, s);
        i = s;
    }
}

void PreschedulerCore::heapify() {
    if (mCount < 2) return;
    for (uint32_t i = mCount / 2; i-- > 0;) siftDown(i);
}

} // namespace supersonic
