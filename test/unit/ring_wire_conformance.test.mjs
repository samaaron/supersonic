// ring_wire_conformance.test.mjs — replay the golden ring-wire corpus
// (test/fixtures/ring_wire.txt) through the JS ring implementation
// (js/lib/ring_buffer_core.js) and require byte-identical results.
//
// The same corpus is replayed by test/native/test_ring_wire_conformance.cpp
// through the C++ implementation (RingBufferWriter.h + ring_drain.h). A
// change to either implementation that alters the wire — framing, padding,
// alignment, cursor advance — fails one of the two suites, so the
// implementations cannot drift apart silently.
//
// Run: npm run test:unit   (node --test test/unit/)
// Regenerate the corpus after an INTENDED format change:
//   node scripts/gen-ring-fixtures.mjs   (then make C++ green again)

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import {
    writeMessageToBuffer,
    canWriteMessage,
    readMessagesFromBuffer,
} from '../../js/lib/ring_buffer_core.js';

const MESSAGE_MAGIC = 0xDEADBEEF;
const PADDING_MAGIC = 0xBADDCAFE;
const HEADER_SIZE   = 16;

const root = dirname(dirname(dirname(fileURLToPath(import.meta.url))));
const fixture = readFileSync(join(root, 'test', 'fixtures', 'ring_wire.txt'), 'utf8');

const hex = (bytes) => Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
const unhex = (s) => Uint8Array.from(s.match(/../g) ?? [], (h) => parseInt(h, 16));

// Parse the corpus into [{ name, size, ops, msgs, head, tail, image }].
function parseCases(text) {
    const cases = [];
    let cur = null;
    for (const raw of text.split('\n')) {
        const line = raw.trim();
        if (!line || line.startsWith('#')) continue;
        const [kw, ...rest] = line.split(' ');
        if (kw === 'case')       cur = { name: rest[0], ops: [], msgs: [] };
        else if (kw === 'size')  cur.size = Number(rest[0]);
        else if (kw === 'write') cur.ops.push({ op: 'write', sourceId: Number(rest[0]), payload: unhex(rest[1]), expect: rest[2] });
        else if (kw === 'drain') cur.ops.push({ op: 'drain', max: Number(rest[0]) });
        else if (kw === 'msg')   cur.msgs.push({ seq: Number(rest[0]), sourceId: Number(rest[1]), payload: rest[2] ?? '' });
        else if (kw === 'head')  cur.head = Number(rest[0]);
        else if (kw === 'tail')  cur.tail = Number(rest[0]);
        else if (kw === 'image') cur.image = rest[0];
        else if (kw === 'end')   { cases.push(cur); cur = null; }
    }
    return cases;
}

const cases = parseCases(fixture);
assert.ok(cases.length >= 5, 'corpus parsed');

for (const c of cases) {
    test(`ring wire conformance (JS): ${c.name}`, () => {
        const buf = new ArrayBuffer(c.size);
        const uint8View = new Uint8Array(buf);
        const dataView = new DataView(buf);
        let head = 0, tail = 0, seq = 0;
        const got = [];

        for (const op of c.ops) {
            if (op.op === 'write') {
                const aligned = (HEADER_SIZE + op.payload.length + 3) & ~3;
                if (canWriteMessage(head, tail, c.size, aligned)) {
                    head = writeMessageToBuffer({
                        uint8View, dataView,
                        bufferStart: 0, bufferSize: c.size,
                        head, payload: op.payload, sequence: seq++,
                        messageMagic: MESSAGE_MAGIC,
                        paddingMagic: PADDING_MAGIC,
                        headerSize: HEADER_SIZE,
                        sourceId: op.sourceId,
                    });
                    assert.equal(op.expect, 'ok', `${c.name}: write accepted but corpus says full`);
                } else {
                    assert.equal(op.expect, 'full', `${c.name}: write rejected but corpus says ok`);
                }
            } else {
                const { newTail } = readMessagesFromBuffer({
                    uint8View, dataView,
                    bufferStart: 0, bufferSize: c.size,
                    head, tail,
                    messageMagic: MESSAGE_MAGIC,
                    paddingMagic: PADDING_MAGIC,
                    headerSize: HEADER_SIZE,
                    maxMessages: op.max === 0 ? Infinity : op.max,
                    onMessage: (payloadOffset, payloadLength, sequence, sourceId) => {
                        got.push({
                            seq: sequence, sourceId,
                            payload: hex(uint8View.subarray(payloadOffset, payloadOffset + payloadLength)),
                        });
                    },
                });
                tail = newTail;
            }
        }

        assert.deepEqual(got, c.msgs, `${c.name}: delivered messages`);
        assert.equal(head, c.head, `${c.name}: final head`);
        assert.equal(tail, c.tail, `${c.name}: final tail`);
        assert.equal(hex(uint8View), c.image, `${c.name}: final ring image`);
    });
}
