#!/usr/bin/env node
// gen-ring-fixtures.mjs — regenerate test/fixtures/ring_wire.txt, the golden
// corpus that holds the JS ring implementation (js/lib/ring_buffer_core.js)
// and the C++ implementation (src/workers/RingBufferWriter.h +
// src/lanes/ring_drain.h) byte-identical.
//
// The fixture is produced BY the JS implementation; both conformance tests
// (test/unit/ring_wire_conformance.test.mjs and
// test/native/test_ring_wire_conformance.cpp) replay the same op sequences
// and must reproduce the recorded messages, cursors and full ring image
// exactly. Any drift between the two implementations — or any accidental
// change to the wire format — fails one side.
//
// Run: node scripts/gen-ring-fixtures.mjs

import { writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import {
    writeMessageToBuffer,
    canWriteMessage,
    readMessagesFromBuffer,
} from '../js/lib/ring_buffer_core.js';

const MESSAGE_MAGIC = 0xDEADBEEF;
const PADDING_MAGIC = 0xBADDCAFE;
const HEADER_SIZE   = 16;

const hex = (bytes) => Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');

// Each case is { name, size, ops: [['write', srcId, payloadBytes] | ['drain']] }.
// Payloads are 4-byte multiples (the OSC domain) except where noted.
const CASES = [
    {
        name: 'two_frames',
        size: 128,
        ops: [
            ['write', 7, Uint8Array.from({ length: 8 }, (_, i) => i + 1)],
            ['write', 2, Uint8Array.from({ length: 12 }, (_, i) => i + 1)],
            ['drain'],
        ],
    },
    {
        // Second frame ends exactly at the boundary: head returns to 0 with
        // no padding marker.
        name: 'exact_fit_to_end',
        size: 64,
        ops: [
            ['write', 1, Uint8Array.from({ length: 16 }, (_, i) => 0x10 + i)],
            ['drain'],
            ['write', 2, Uint8Array.from({ length: 16 }, (_, i) => 0x20 + i)],
            ['drain'],
        ],
    },
    {
        // Third frame doesn't fit before the end: full padding marker at 80
        // (16+ bytes of room), frame restarts at offset 0.
        name: 'pad_full_header',
        size: 128,
        ops: [
            ['write', 1, Uint8Array.from({ length: 32 }, (_, i) => 0xA0 + (i & 0x0F))],
            ['write', 2, Uint8Array.from({ length: 16 }, (_, i) => 0xB0 + (i & 0x0F))],
            ['drain'],
            ['write', 3, Uint8Array.from({ length: 56 }, (_, i) => i)],
            ['drain'],
        ],
    },
    {
        // Only 4 bytes remain before the boundary: the padding marker is the
        // bare magic word.
        name: 'pad_magic_only',
        size: 64,
        ops: [
            ['write', 1, Uint8Array.from({ length: 24 }, (_, i) => 0xC0 + (i & 0x0F))],
            ['write', 2, Uint8Array.from({ length: 4 }, (_, i) => 0xD0 + i)],
            ['drain'],
            ['write', 3, Uint8Array.from({ length: 24 }, (_, i) => 0xE0 + (i & 0x0F))],
            ['drain'],
        ],
    },
    {
        // Ring too full: the write must be rejected and leave no trace.
        name: 'full_reject',
        size: 64,
        ops: [
            ['write', 1, Uint8Array.from({ length: 24 }, (_, i) => i)],
            ['write', 2, Uint8Array.from({ length: 24 }, (_, i) => i)],
            ['drain'],
        ],
    },
    {
        // MIDI-style non-4-multiple payload: exact length in the header,
        // zeroed pad bytes, cursor advances by the aligned footprint.
        name: 'non_aligned_payload',
        size: 64,
        ops: [
            ['write', 1, Uint8Array.from([0x90, 60, 100])],
            ['write', 2, Uint8Array.from([0xB0, 7, 1, 2, 3])],
            ['drain'],
        ],
    },
    {
        // Free space exists in total but not contiguously: rejected.
        name: 'no_contiguous_room',
        size: 256,
        ops: [
            ['write', 1, Uint8Array.from({ length: 16 }, (_, i) => i)],
            ['write', 2, Uint8Array.from({ length: 128 }, (_, i) => i)],
            ['drain', 1],
            ['write', 3, Uint8Array.from({ length: 80 }, (_, i) => i)],
            ['drain'],
        ],
    },
];

function runCase({ name, size, ops }) {
    const buf = new ArrayBuffer(size);
    const uint8View = new Uint8Array(buf);
    const dataView = new DataView(buf);
    let head = 0, tail = 0, seq = 0;

    const lines = [`case ${name}`, `size ${size}`];
    const msgs = [];

    for (const op of ops) {
        if (op[0] === 'write') {
            const [, sourceId, payload] = op;
            const aligned = (HEADER_SIZE + payload.length + 3) & ~3;
            if (canWriteMessage(head, tail, size, aligned)) {
                head = writeMessageToBuffer({
                    uint8View, dataView,
                    bufferStart: 0, bufferSize: size,
                    head, payload, sequence: seq++,
                    messageMagic: MESSAGE_MAGIC,
                    paddingMagic: PADDING_MAGIC,
                    headerSize: HEADER_SIZE,
                    sourceId,
                });
                lines.push(`write ${sourceId} ${hex(payload)} ok`);
            } else {
                lines.push(`write ${sourceId} ${hex(payload)} full`);
            }
        } else {
            const max = op[1] ?? 0;  // 0 = drain everything
            const { newTail } = readMessagesFromBuffer({
                uint8View, dataView,
                bufferStart: 0, bufferSize: size,
                head, tail,
                messageMagic: MESSAGE_MAGIC,
                paddingMagic: PADDING_MAGIC,
                headerSize: HEADER_SIZE,
                maxMessages: max === 0 ? Infinity : max,
                onMessage: (payloadOffset, payloadLength, sequence, sourceId) => {
                    msgs.push(`msg ${sequence} ${sourceId} ${hex(uint8View.subarray(payloadOffset, payloadOffset + payloadLength))}`);
                },
            });
            tail = newTail;
            lines.push(`drain ${max}`);
        }
    }

    lines.push(...msgs);
    lines.push(`head ${head}`);
    lines.push(`tail ${tail}`);
    lines.push(`image ${hex(uint8View)}`);
    lines.push('end', '');
    return lines.join('\n');
}

const header = `# ring_wire.txt — golden ring-wire conformance corpus.
# GENERATED by scripts/gen-ring-fixtures.mjs — DO NOT EDIT BY HAND.
# Replayed byte-for-byte by test/unit/ring_wire_conformance.test.mjs (JS) and
# test/native/test_ring_wire_conformance.cpp (C++). msg lines are the expected
# deliveries in order across all drains; image is the final ring contents.
`;

const out = header + '\n' + CASES.map(runCase).join('\n');
const root = dirname(dirname(fileURLToPath(import.meta.url)));
const dest = join(root, 'test', 'fixtures', 'ring_wire.txt');
writeFileSync(dest, out);
console.log(`wrote ${dest}`);
