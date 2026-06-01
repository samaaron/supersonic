/*
 * prescheduler_core.spec.mjs — CI coverage for the pure PreschedulerCore
 * (js/lib/prescheduler_core.js), the algorithm the OSC-out prescheduler worker
 * delegates to. Two layers:
 *
 *   1. Vectors — every case in test/vectors/prescheduler.json, the same
 *      language-neutral contract the C++ port satisfies
 *      (test/native/test_prescheduler_core.cpp). This makes the shared contract
 *      enforced in CI, not just by the manual `node test/prescheduler_vectors.mjs`.
 *   2. Property — a fast-check differential test against an independent
 *      reference model: heap order at depth, FIFO ties, and cancellation counts.
 *
 * Pure JS, no browser — the core has no DOM/SAB/worker dependencies.
 */
import { test, expect } from '@playwright/test';
import fc from 'fast-check';
import { readFileSync } from 'node:fs';
import { PreschedulerCore } from '../js/lib/prescheduler_core.js';
import { runVector } from './vectors/prescheduler_sim.mjs';

const data = JSON.parse(readFileSync(new URL('./vectors/prescheduler.json', import.meta.url)));
const D = data.defaults;

test.describe('PreschedulerCore', () => {
    test('matches every shared vector', () => {
        for (const v of data.vectors) {
            const got = runVector(v, D);
            const exp = v.expect;
            if (exp.dispatched) expect(got.dispatched, v.name).toEqual(exp.dispatched);
            if (exp.cancelled != null) expect(got.cancelled, v.name).toBe(exp.cancelled);
            if (exp.rejected) {
                const norm = (a) => [...a].sort((x, y) => String(x.id).localeCompare(String(y.id)));
                expect(norm(got.rejected), v.name).toEqual(norm(exp.rejected));
            }
            if (exp.released_at) {
                for (const [id, t] of Object.entries(exp.released_at)) {
                    expect(got.releasedAt[id], `${v.name} released_at[${id}]`).toBeCloseTo(t, 6);
                }
            }
        }
    });

    test('matches a reference model under randomized ops', () => {
        const TAGS = ['t0', 't1', 't2', 't3'];

        const scheduleArb = fc.record({
            kind: fc.constant('sched'),
            due: fc.integer({ min: 0, max: 49 }),  // many duplicates → exercise seq tie-break
            session: fc.integer({ min: 0, max: 3 }),
            tag: fc.integer({ min: 0, max: 3 }),
        });
        const cancelArb = fc.record({
            kind: fc.constantFrom('cancelTag', 'cancelSession', 'cancelSessionTag', 'cancelAll'),
            session: fc.integer({ min: 0, max: 3 }),
            tag: fc.integer({ min: 0, max: 3 }),
        });
        const opArb = fc.oneof({ weight: 3, arbitrary: scheduleArb }, { weight: 1, arbitrary: cancelArb });

        fc.assert(fc.property(fc.array(opArb, { maxLength: 500 }), (ops) => {
            const core = new PreschedulerCore({ lookaheadS: 0, maxPending: 1e9, maxFutureS: 1e12, poolBytes: 1 << 20 });
            const ref = [];   // { ntpTime, seq, session, tag, id }
            let seq = 0, id = 0;

            for (const op of ops) {
                if (op.kind === 'sched') {
                    const tag = TAGS[op.tag];
                    const r = core.schedule({ ntpTime: op.due, bytes: 16, sessionId: op.session, runTag: tag, payload: id }, 0);
                    if (!r.ok || !r.scheduled) return false;  // headroom is huge → must queue
                    ref.push({ ntpTime: op.due, seq: seq++, session: op.session, tag, id });
                    id++;
                } else {
                    const tag = TAGS[op.tag];
                    let removed, refRemoved;
                    if (op.kind === 'cancelTag') { removed = core.cancelTag(tag); refRemoved = ref.filter((e) => e.tag === tag); }
                    else if (op.kind === 'cancelSession') { removed = core.cancelSession(op.session); refRemoved = ref.filter((e) => e.session === op.session); }
                    else if (op.kind === 'cancelSessionTag') { removed = core.cancelSessionTag(op.session, tag); refRemoved = ref.filter((e) => e.session === op.session && e.tag === tag); }
                    else { removed = core.cancelAll(); refRemoved = ref.slice(); }

                    if (removed !== refRemoved.length) return false;
                    const ids = new Set(refRemoved.map((e) => e.id));
                    for (let i = ref.length - 1; i >= 0; i--) if (ids.has(ref[i].id)) ref.splice(i, 1);
                    if (core.size() !== ref.length) return false;
                }
            }

            const got = [];
            core.dispatchDue(1e12, (e) => got.push(e.payload));
            if (core.size() !== 0) return false;

            ref.sort((a, b) => (a.ntpTime !== b.ntpTime ? a.ntpTime - b.ntpTime : a.seq - b.seq));
            const want = ref.map((e) => e.id);
            return JSON.stringify(got) === JSON.stringify(want);
        }), { numRuns: 300 });
    });
});
