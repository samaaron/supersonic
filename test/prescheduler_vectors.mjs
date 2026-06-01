/*
 * prescheduler_vectors.mjs — standalone runner for test/vectors/prescheduler.json
 * against the pure PreschedulerCore. The same vectors drive the C++ port
 * (test/native/test_prescheduler_core.cpp) and the CI spec
 * (test/prescheduler_core.spec.mjs); a green run here is the quick dev check.
 *
 *   node test/prescheduler_vectors.mjs
 *
 * The simulator lives in test/vectors/prescheduler_sim.mjs (one shared sim).
 */
import { readFileSync } from 'node:fs';
import { runVector } from './vectors/prescheduler_sim.mjs';

const data = JSON.parse(readFileSync(new URL('./vectors/prescheduler.json', import.meta.url)));
const D = data.defaults;

const eq = (a, b) => JSON.stringify(a) === JSON.stringify(b);
let pass = 0, fail = 0;
for (const v of data.vectors) {
    const got = runVector(v, D), exp = v.expect, errs = [];
    if (exp.dispatched && !eq(got.dispatched, exp.dispatched)) errs.push(`dispatched ${JSON.stringify(got.dispatched)} != ${JSON.stringify(exp.dispatched)}`);
    if (exp.cancelled != null && got.cancelled !== exp.cancelled) errs.push(`cancelled ${got.cancelled} != ${exp.cancelled}`);
    if (exp.rejected) {
        const norm = (a) => [...a].sort((x, y) => String(x.id).localeCompare(String(y.id)));
        if (!eq(norm(got.rejected), norm(exp.rejected))) errs.push(`rejected ${JSON.stringify(got.rejected)} != ${JSON.stringify(exp.rejected)}`);
    }
    if (exp.released_at) for (const [id, t] of Object.entries(exp.released_at))
        if (got.releasedAt[id] !== t) errs.push(`released_at[${id}] ${got.releasedAt[id]} != ${t}`);
    if (errs.length) { fail++; console.log(`  FAIL  ${v.name}`); errs.forEach((e) => console.log(`          ${e}`)); }
    else { pass++; console.log(`  ok    ${v.name}`); }
}
console.log(`\n${pass}/${pass + fail} vectors pass`);
process.exit(fail ? 1 : 0);
