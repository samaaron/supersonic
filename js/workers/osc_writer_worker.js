/*
    SuperSonic - OSC Writer Worker
    This is the ONLY worker that writes to the ring buffer going to WASM.
    It reads from a queue populated by the scheduler worker.

    Benefits:
    - Single writer eliminates concurrent write corruption
    - Can safely block using Atomics.wait() when ring buffer is full
    - Decouples scheduling from writing
*/

// Shared memory references
var sharedBuffer = null;
var ringBufferBase = null;
var atomicView = null;
var dataView = null;
var uint8View = null;

// Ring buffer constants (from WASM)
var bufferConstants = null;

// Ring buffer control indices
var CONTROL_INDICES = {};

// Queue for messages from scheduler (simple growing array for now)
// TODO: Could be replaced with a proper ring buffer if needed
var messageQueue = [];
var isProcessing = false;

var DEBUG_WRITER_LOGS = false;
function writerLog() {
    if (DEBUG_WRITER_LOGS) {
        console.log.apply(console, arguments);
    }
}

// Statistics
var stats = {
    messagesWritten: 0,
    messagesDropped: 0,
    bufferOverruns: 0,
    queueDepth: 0,
    maxQueueDepth: 0
};

// Running flag
var running = false;

// MessagePort for receiving messages from scheduler worker
var schedulerPort = null;

/**
 * Initialize ring buffer access
 */
function initRingBuffer(buffer, base, constants) {
    sharedBuffer = buffer;
    ringBufferBase = base;
    bufferConstants = constants;
    atomicView = new Int32Array(sharedBuffer);
    dataView = new DataView(sharedBuffer);
    uint8View = new Uint8Array(sharedBuffer);

    // Calculate control indices
    CONTROL_INDICES = {
        IN_HEAD: (ringBufferBase + bufferConstants.CONTROL_START + 0) / 4,
        IN_TAIL: (ringBufferBase + bufferConstants.CONTROL_START + 4) / 4
    };

    writerLog('[OSCWriterWorker] Ring buffer initialized');
}

/**
 * Extract bundle timestamp for logging
 */
function getBundleTimestamp(oscMessage) {
    if (oscMessage.length >= 16 && oscMessage[0] === 0x23) {
        var view = new DataView(oscMessage.buffer, oscMessage.byteOffset);
        var ntpSeconds = view.getUint32(8, false);
        var ntpFraction = view.getUint32(12, false);
        return ntpSeconds + ntpFraction / 0x100000000;
    }
    return null;
}

/**
 * Write message to ring buffer - blocks until space is available
 * This is the ONLY function that writes to the ring buffer
 */
function writeToRingBuffer(oscMessage) {
    if (!sharedBuffer) {
        console.error('[OSCWriterWorker] Not initialized');
        return false;
    }

    var bundleTimestamp = getBundleTimestamp(oscMessage);
    var enterWriter = performance.now();

    var payloadSize = oscMessage.length;
    var totalSize = bufferConstants.MESSAGE_HEADER_SIZE + payloadSize;

    // Check if message fits in buffer at all
    if (totalSize > bufferConstants.IN_BUFFER_SIZE - bufferConstants.MESSAGE_HEADER_SIZE) {
        console.error('[OSCWriterWorker] Message too large:', totalSize);
        stats.messagesDropped++;
        return false;
    }

    // Keep trying until we have space (blocking)
    while (running) {
        var head = Atomics.load(atomicView, CONTROL_INDICES.IN_HEAD);
        var tail = Atomics.load(atomicView, CONTROL_INDICES.IN_TAIL);

        // Calculate available space
        var available = (bufferConstants.IN_BUFFER_SIZE - 1 - head + tail) % bufferConstants.IN_BUFFER_SIZE;

        if (available >= totalSize) {
            // Check if message fits contiguously
            var spaceToEnd = bufferConstants.IN_BUFFER_SIZE - head;

            if (totalSize > spaceToEnd) {
                if (spaceToEnd >= bufferConstants.MESSAGE_HEADER_SIZE) {
                    // Write padding marker and wrap
                    var paddingPos = ringBufferBase + bufferConstants.IN_BUFFER_START + head;
                    dataView.setUint32(paddingPos, bufferConstants.PADDING_MAGIC, true);
                    dataView.setUint32(paddingPos + 4, 0, true);
                    dataView.setUint32(paddingPos + 8, 0, true);
                    dataView.setUint32(paddingPos + 12, 0, true);
                } else if (spaceToEnd > 0) {
                    // Not enough room for a padding header - clear remaining bytes
                    var padStart = ringBufferBase + bufferConstants.IN_BUFFER_START + head;
                    for (var i = 0; i < spaceToEnd; i++) {
                        uint8View[padStart + i] = 0;
                    }
                }

                // Wrap to beginning
                head = 0;
                spaceToEnd = bufferConstants.IN_BUFFER_SIZE; // subsequent writes use full buffer
            }

            // Write message
            var writePos = ringBufferBase + bufferConstants.IN_BUFFER_START + head;

            // Write header
            dataView.setUint32(writePos, bufferConstants.MESSAGE_MAGIC, true);
            dataView.setUint32(writePos + 4, totalSize, true);
            dataView.setUint32(writePos + 8, stats.messagesWritten, true); // sequence
            dataView.setUint32(writePos + 12, 0, true); // padding

            // Write payload
            uint8View.set(oscMessage, writePos + bufferConstants.MESSAGE_HEADER_SIZE);

            // Update head pointer (publish message)
            var newHead = (head + totalSize) % bufferConstants.IN_BUFFER_SIZE;
            Atomics.store(atomicView, CONTROL_INDICES.IN_HEAD, newHead);

            var writerDuration = performance.now() - enterWriter;
            if (bundleTimestamp !== null) {
                console.log(`[Writer] Bundle NTP=${bundleTimestamp.toFixed(3)} → WASM in ${writerDuration.toFixed(2)}ms`);
            }

            stats.messagesWritten++;
            return true;
        }

        // Buffer full - wait for space
        stats.bufferOverruns++;

        // Block until tail moves (WASM consumes data)
        // This is safe because this worker ONLY writes, never does scheduling
        var result = Atomics.wait(atomicView, CONTROL_INDICES.IN_TAIL, tail, 100);

        if (result === 'ok' || result === 'not-equal') {
            // Tail moved, retry
            continue;
        }
        // On timeout, check if we should stop, otherwise retry
    }

    return false;
}

/**
 * Process the message queue
 * Pulls messages from the queue and writes to ring buffer
 */
function processQueue() {
    if (isProcessing || messageQueue.length === 0) {
        return;
    }

    isProcessing = true;

    while (messageQueue.length > 0 && running) {
        var message = messageQueue.shift();
        stats.queueDepth = messageQueue.length;

        // Write to ring buffer (may block)
        writeToRingBuffer(message);
    }

    isProcessing = false;
}

/**
 * Add message to queue from scheduler worker
 */
function enqueue(oscData) {
    messageQueue.push(oscData);
    stats.queueDepth = messageQueue.length;

    if (stats.queueDepth > stats.maxQueueDepth) {
        stats.maxQueueDepth = stats.queueDepth;
    }

    // Start processing if not already
    if (!isProcessing) {
        // Process immediately - queue drains all messages in while loop
        processQueue();
    }
}

/**
 * Handle messages from main thread and scheduler worker
 */
self.onmessage = function(event) {
    var data = event.data;

    try {
        switch (data.type) {
            case 'init':
                initRingBuffer(data.sharedBuffer, data.ringBufferBase, data.bufferConstants);
                self.postMessage({ type: 'initialized' });
                break;

            case 'setSchedulerPort':
                // Receive MessagePort from main thread for direct scheduler communication
                schedulerPort = data.port;
                schedulerPort.onmessage = function(event) {
                    // Messages from scheduler worker arrive here
                    var msg = event.data;
                    if (msg.type === 'write') {
                        enqueue(msg.oscData);
                    }
                };
                writerLog('[OSCWriterWorker] Scheduler port connected');
                break;

            case 'start':
                running = true;
                writerLog('[OSCWriterWorker] Started');
                break;

            case 'stop':
                running = false;
                writerLog('[OSCWriterWorker] Stopped');
                break;

            case 'write':
                // Fallback: Message from main thread (shouldn't normally happen)
                enqueue(data.oscData);
                break;

            case 'getStats':
                self.postMessage({
                    type: 'stats',
                    stats: stats
                });
                break;

            default:
                console.warn('[OSCWriterWorker] Unknown message type:', data.type);
        }
    } catch (error) {
        console.error('[OSCWriterWorker] Error:', error);
        self.postMessage({
            type: 'error',
            error: error.message
        });
    }
};

writerLog('[OSCWriterWorker] Script loaded');
