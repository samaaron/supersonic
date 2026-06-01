/*
 * prescheduler_sim.mjs — the reference simulator for the prescheduler vectors,
 * shared by the standalone node runner (test/prescheduler_vectors.mjs) and the
 * CI spec (test/prescheduler_core.spec.mjs) so there is exactly one sim.
 *
 * Event-driven mock clock: it interleaves timer-driven releases (at
 * nextDueTime()) with the vector's ops (at their t), so an event due at D is
 * released at exactly D - lookahead. A past-due event fires at the current
 * clock (delay clamped to 0 in the worker), never running time backward.
 */
import { PreschedulerCore } from '../../js/lib/prescheduler_core.js';

const round6 = (x) => Math.round(x * 1e6) / 1e6;

export function runVector(v, defaults) {
    const core = new PreschedulerCore({
        lookaheadS: v.lookahead_s ?? defaults.lookahead_s,
        maxPending: v.max_pending ?? defaults.max_pending,
        maxFutureS: v.max_future_s ?? defaults.max_future_s,
        poolBytes: v.pool_bytes ?? defaults.pool_bytes,
    });
    const dispatched = [], releasedAt = {}, rejected = [];
    let cancelled = 0, now = 0;
    const sink = (e) => { dispatched.push(e.payload); releasedAt[e.payload] = round6(now); };

    const drainDueUpTo = (limit) => {
        let due;
        while ((due = core.nextDueTime()) !== null && due <= limit) { now = Math.max(now, due); core.dispatchDue(now, sink); }
    };

    for (const op of v.ops) {
        drainDueUpTo(op.t);
        now = Math.max(now, op.t);
        if (op.schedule) {
            const s = op.schedule;
            const r = core.schedule({ ntpTime: s.due ?? null, bytes: s.bytes, sessionId: s.session, runTag: s.tag, payload: s.id }, now);
            if (!r.ok) rejected.push({ id: s.id, reason: r.reason });
        } else if (op.send) {
            const r = core.schedule({ ntpTime: null, payload: op.send.id }, now);
            if (r.ok && r.immediate) sink({ payload: op.send.id });
        } else if ('cancelTag' in op) cancelled += core.cancelTag(op.cancelTag);
        else if ('cancelSession' in op) cancelled += core.cancelSession(op.cancelSession);
        else if (op.cancelSessionTag) cancelled += core.cancelSessionTag(op.cancelSessionTag.session, op.cancelSessionTag.tag);
        else if (op.cancelAll) cancelled += core.cancelAll();
    }
    let due;
    while ((due = core.nextDueTime()) !== null) { now = Math.max(now, due); core.dispatchDue(now, sink); }

    return { dispatched, releasedAt, rejected, cancelled };
}
