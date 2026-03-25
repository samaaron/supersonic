// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * Shared infrastructure for SAB ring-buffer reader workers.
 *
 * Each worker calls runSabWorker() with configuration specifying which
 * ring buffer to read, how to process messages, and what to post back.
 */
export function runSabWorker(config) {
    const {
        name,
        calculateControlIndices,
        headIndex,          // (CONTROL_INDICES) => int32 array index for head
        tailIndex,          // (CONTROL_INDICES) => int32 array index for tail
        readMessages,       // (ctx) => array of results (empty if none)
        postResults,        // (results) => void — sends results to main thread
        initMetrics = true,
        onInit,             // optional (ctx) => void — runs after ring buffer setup
        extraHandlers,      // optional { [type]: (data, ctx) => void }
    } = config;

    // Mutable shared state — updated by initRingBuffer, read by callbacks via ctx
    const ctx = {
        sharedBuffer: null,
        ringBufferBase: null,
        bufferConstants: null,
        atomicView: null,
        dataView: null,
        uint8View: null,
        metricsView: null,
        CONTROL_INDICES: {},
    };

    let running = false;

    function initRingBuffer(buffer, base, constants) {
        ctx.sharedBuffer = buffer;
        ctx.ringBufferBase = base;
        ctx.bufferConstants = constants;
        ctx.atomicView = new Int32Array(buffer);
        ctx.dataView = new DataView(buffer);
        ctx.uint8View = new Uint8Array(buffer);
        ctx.CONTROL_INDICES = calculateControlIndices(base, constants.CONTROL_START);
        if (initMetrics) {
            const metricsBase = base + constants.METRICS_START;
            ctx.metricsView = new Uint32Array(buffer, metricsBase, constants.METRICS_SIZE / 4);
        }
        onInit?.(ctx);
    }

    function waitLoop() {
        const hIdx = headIndex(ctx.CONTROL_INDICES);
        const tIdx = tailIndex(ctx.CONTROL_INDICES);

        while (running) {
            try {
                const currentHead = Atomics.load(ctx.atomicView, hIdx);
                const currentTail = Atomics.load(ctx.atomicView, tIdx);

                if (currentHead === currentTail) {
                    Atomics.wait(ctx.atomicView, hIdx, currentHead);
                }

                const results = readMessages(ctx);
                if (results && results.length > 0) {
                    postResults(results);
                }
            } catch (error) {
                console.error(`[${name}] Error in wait loop:`, error);
                self.postMessage({ type: 'error', error: error.message });
                Atomics.wait(ctx.atomicView, 0, ctx.atomicView[0], 10);
            }
        }
    }

    function start() {
        if (!ctx.sharedBuffer) {
            console.error(`[${name}] Cannot start - not initialized`);
            return;
        }
        if (running) {
            if (__DEV__) console.warn(`[${name}] Already running`);
            return;
        }
        running = true;
        waitLoop();
    }

    function stop() {
        running = false;
    }

    self.addEventListener('message', (event) => {
        const { data } = event;
        try {
            if (extraHandlers?.[data.type]) {
                extraHandlers[data.type](data, ctx);
                return;
            }
            switch (data.type) {
                case 'init':
                    if (data.sharedBuffer) {
                        initRingBuffer(data.sharedBuffer, data.ringBufferBase, data.bufferConstants);
                    }
                    self.postMessage({ type: 'initialized' });
                    break;
                case 'start':
                    if (ctx.sharedBuffer) start();
                    break;
                case 'stop':
                    stop();
                    break;
                default:
                    if (__DEV__) console.warn(`[${name}] Unknown message type:`, data.type);
            }
        } catch (error) {
            console.error(`[${name}] Error:`, error);
            self.postMessage({ type: 'error', error: error.message });
        }
    });

    if (__DEV__) console.log(`[${name}] Script loaded`);
}
