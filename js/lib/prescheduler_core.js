/*
 * prescheduler_core.js — pure OSC-out prescheduler: an NTP min-heap with
 * cancellation. No timers, no SharedArrayBuffer, no postMessage — the algorithm
 * only. Hosts wrap it: the web worker drives it with setTimeout + ring-buffer
 * writes; the native/embedded port wraps it in a thread / FreeRTOS task.
 *
 * Behaviour is pinned by test/vectors/prescheduler.json — the contract every
 * host (the web worker, the C++ port, the embedded wrapper) must satisfy.
 *
 * Events are ordered by NTP time, ties broken by insertion order (seq) → FIFO.
 * An event due at D is released when now >= D - lookahead.
 */
export class PreschedulerCore {
    constructor({ lookaheadS = 0.5, maxPending = 1000, maxFutureS = 3600, poolBytes = 524288 } = {}) {
        this.lookaheadS = lookaheadS;
        this.maxPending = maxPending;
        this.maxFutureS = maxFutureS;
        this.poolBytes = poolBytes;
        this._heap = [];
        this._seq = 0;
        this.retryCount = 0; // host-owned output-retry pressure; counts toward backpressure
    }

    // Number of events currently queued (for metrics / pending checks).
    size() { return this._heap.length; }

    // Schedule an event at `now`. ntpTime == null means "immediate" (a non-bundle):
    // it is not queued — it's returned so the host dispatches it right away.
    // bytes is the encoded OSC size (checked against the pool slot).
    // Returns { ok:true, scheduled } | { ok:true, immediate } | { ok:false, reason }.
    schedule(ev, now) {
        const totalPending = this._heap.length + this.retryCount;
        if (totalPending >= this.maxPending) return { ok: false, reason: 'queue_full' };
        if (ev.ntpTime == null) return { ok: true, immediate: ev };
        if ((ev.bytes || 0) > this.poolBytes) return { ok: false, reason: 'too_large' };
        if (ev.ntpTime - now > this.maxFutureS) return { ok: false, reason: 'too_far_future' };
        const event = {
            ntpTime: ev.ntpTime,
            seq: this._seq++,
            sessionId: ev.sessionId || 0,
            runTag: ev.runTag || '',
            payload: ev.payload,
        };
        this._push(event);
        return { ok: true, scheduled: event };
    }

    // When the next event should be released (its ntpTime - lookahead), or null.
    nextDueTime() {
        return this._heap.length ? this._heap[0].ntpTime - this.lookaheadS : null;
    }

    // Release every event due by `now` (ntpTime <= now + lookahead), in order,
    // handing each to sink(event). Returns the count released.
    dispatchDue(now, sink) {
        const lookaheadTime = now + this.lookaheadS;
        let n = 0;
        while (this._heap.length && this._heap[0].ntpTime <= lookaheadTime) {
            sink(this._pop());
            n++;
        }
        return n;
    }

    // --- cancellation (filter + re-heapify) ------------------------------------
    cancelBy(pred) {
        if (this._heap.length === 0) return 0;
        const before = this._heap.length;
        this._heap = this._heap.filter((e) => !pred(e));
        const removed = before - this._heap.length;
        if (removed > 0) this._heapify();
        return removed;
    }
    cancelTag(runTag) { return this.cancelBy((e) => e.runTag === runTag); }
    cancelSession(sessionId) { return this.cancelBy((e) => e.sessionId === sessionId); }
    cancelSessionTag(sessionId, runTag) {
        return this.cancelBy((e) => e.sessionId === sessionId && e.runTag === runTag);
    }
    cancelAll() { const n = this._heap.length; this._heap = []; return n; }

    // --- min-heap internals ----------------------------------------------------
    _cmp(a, b) { return a.ntpTime === b.ntpTime ? a.seq - b.seq : a.ntpTime - b.ntpTime; }
    _swap(i, j) { const t = this._heap[i]; this._heap[i] = this._heap[j]; this._heap[j] = t; }
    _push(e) { this._heap.push(e); this._siftUp(this._heap.length - 1); }
    _pop() {
        const top = this._heap[0];
        const last = this._heap.pop();
        if (this._heap.length) { this._heap[0] = last; this._siftDown(0); }
        return top;
    }
    _siftUp(i) {
        while (i > 0) {
            const p = (i - 1) >> 1;
            if (this._cmp(this._heap[i], this._heap[p]) >= 0) break;
            this._swap(i, p);
            i = p;
        }
    }
    _siftDown(i) {
        const n = this._heap.length;
        for (;;) {
            const l = 2 * i + 1, r = 2 * i + 2;
            let s = i;
            if (l < n && this._cmp(this._heap[l], this._heap[s]) < 0) s = l;
            if (r < n && this._cmp(this._heap[r], this._heap[s]) < 0) s = r;
            if (s === i) break;
            this._swap(i, s);
            i = s;
        }
    }
    _heapify() { for (let i = (this._heap.length >> 1) - 1; i >= 0; i--) this._siftDown(i); }
}
