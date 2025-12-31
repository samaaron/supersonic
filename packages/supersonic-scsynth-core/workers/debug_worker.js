(() => {
  // js/lib/metrics_offsets.js
  var DEBUG_MESSAGES_RECEIVED = 20;
  var DEBUG_BYTES_RECEIVED = 21;

  // js/lib/ring_buffer_core.js
  function readMessagesFromBuffer({
    uint8View: uint8View2,
    dataView: dataView2,
    bufferStart,
    bufferSize,
    head,
    tail,
    messageMagic,
    paddingMagic,
    headerSize,
    maxMessages = Infinity,
    onMessage,
    onCorruption
  }) {
    let currentTail = tail;
    let messagesRead = 0;
    while (currentTail !== head && messagesRead < maxMessages) {
      const bytesToEnd = bufferSize - currentTail;
      if (bytesToEnd < headerSize) {
        currentTail = 0;
        continue;
      }
      const readPos = bufferStart + currentTail;
      const magic = dataView2.getUint32(readPos, true);
      if (magic === paddingMagic) {
        currentTail = 0;
        continue;
      }
      if (magic !== messageMagic) {
        if (onCorruption) {
          onCorruption(currentTail);
        }
        currentTail = (currentTail + 1) % bufferSize;
        continue;
      }
      const length = dataView2.getUint32(readPos + 4, true);
      const sequence = dataView2.getUint32(readPos + 8, true);
      if (length < headerSize || length > bufferSize) {
        if (onCorruption) {
          onCorruption(currentTail);
        }
        currentTail = (currentTail + 1) % bufferSize;
        continue;
      }
      const payloadLength = length - headerSize;
      const payloadStart = readPos + headerSize;
      const payload = new Uint8Array(payloadLength);
      for (let i = 0; i < payloadLength; i++) {
        payload[i] = uint8View2[payloadStart + i];
      }
      onMessage(payload, sequence, length);
      currentTail = (currentTail + length) % bufferSize;
      messagesRead++;
    }
    return { newTail: currentTail, messagesRead };
  }

  // js/workers/debug_worker.js
  var mode = "sab";
  var sharedBuffer = null;
  var ringBufferBase = null;
  var atomicView = null;
  var dataView = null;
  var uint8View = null;
  var bufferConstants = null;
  var CONTROL_INDICES = {};
  var metricsView = null;
  var running = false;
  var textDecoder = new TextDecoder("utf-8");
  var debugWorkerLog = (...args) => {
    if (true) {
      console.log(...args);
    }
  };
  var initRingBuffer = (buffer, base, constants) => {
    sharedBuffer = buffer;
    ringBufferBase = base;
    bufferConstants = constants;
    atomicView = new Int32Array(sharedBuffer);
    dataView = new DataView(sharedBuffer);
    uint8View = new Uint8Array(sharedBuffer);
    CONTROL_INDICES = {
      DEBUG_HEAD: (ringBufferBase + bufferConstants.CONTROL_START + 16) / 4,
      DEBUG_TAIL: (ringBufferBase + bufferConstants.CONTROL_START + 20) / 4
    };
    const metricsBase = ringBufferBase + bufferConstants.METRICS_START;
    metricsView = new Uint32Array(sharedBuffer, metricsBase, bufferConstants.METRICS_SIZE / 4);
  };
  var readDebugMessages = () => {
    const head = Atomics.load(atomicView, CONTROL_INDICES.DEBUG_HEAD);
    const tail = Atomics.load(atomicView, CONTROL_INDICES.DEBUG_TAIL);
    if (head === tail) {
      return null;
    }
    const messages = [];
    const { newTail, messagesRead } = readMessagesFromBuffer({
      uint8View,
      dataView,
      bufferStart: ringBufferBase + bufferConstants.DEBUG_BUFFER_START,
      bufferSize: bufferConstants.DEBUG_BUFFER_SIZE,
      head,
      tail,
      messageMagic: bufferConstants.MESSAGE_MAGIC,
      paddingMagic: bufferConstants.PADDING_MAGIC,
      headerSize: bufferConstants.MESSAGE_HEADER_SIZE,
      maxMessages: 1e3,
      onMessage: (payload, sequence, length) => {
        let messageText = textDecoder.decode(payload);
        if (messageText.endsWith("\n")) {
          messageText = messageText.slice(0, -1);
        }
        messages.push({
          text: messageText,
          timestamp: performance.now(),
          sequence
        });
        if (metricsView) {
          Atomics.add(metricsView, DEBUG_MESSAGES_RECEIVED, 1);
          Atomics.add(metricsView, DEBUG_BYTES_RECEIVED, payload.length);
        }
      },
      onCorruption: (position) => {
        console.error("[DebugWorker] Corrupted message at position", position);
      }
    });
    if (messagesRead > 0) {
      Atomics.store(atomicView, CONTROL_INDICES.DEBUG_TAIL, newTail);
    }
    return messages.length > 0 ? messages : null;
  };
  var waitLoop = () => {
    while (running) {
      try {
        const currentHead = Atomics.load(atomicView, CONTROL_INDICES.DEBUG_HEAD);
        const currentTail = Atomics.load(atomicView, CONTROL_INDICES.DEBUG_TAIL);
        if (currentHead === currentTail) {
          const result = Atomics.wait(atomicView, CONTROL_INDICES.DEBUG_HEAD, currentHead, 100);
          if (result === "ok" || result === "not-equal") {
          } else if (result === "timed-out") {
            continue;
          }
        }
        const messages = readDebugMessages();
        if (messages && messages.length > 0) {
          self.postMessage({
            type: "debug",
            messages
          });
        }
      } catch (error) {
        console.error("[DebugWorker] Error in wait loop:", error);
        self.postMessage({
          type: "error",
          error: error.message
        });
        Atomics.wait(atomicView, 0, atomicView[0], 10);
      }
    }
  };
  var start = () => {
    if (!sharedBuffer) {
      console.error("[DebugWorker] Cannot start - not initialized");
      return;
    }
    if (running) {
      console.warn("[DebugWorker] Already running");
      return;
    }
    running = true;
    waitLoop();
  };
  var stop = () => {
    running = false;
  };
  var clear = () => {
    if (!sharedBuffer) return;
    Atomics.store(atomicView, CONTROL_INDICES.DEBUG_HEAD, 0);
    Atomics.store(atomicView, CONTROL_INDICES.DEBUG_TAIL, 0);
  };
  var decodeRawMessages = (rawMessages) => {
    const messages = [];
    for (const raw of rawMessages) {
      try {
        const bytes = new Uint8Array(raw.bytes);
        let text = textDecoder.decode(bytes);
        if (text.endsWith("\n")) {
          text = text.slice(0, -1);
        }
        messages.push({
          text,
          timestamp: performance.now(),
          sequence: raw.sequence
        });
      } catch (err) {
        console.error("[DebugWorker] Failed to decode message:", err);
      }
    }
    if (messages.length > 0) {
      self.postMessage({
        type: "debug",
        messages
      });
    }
  };
  self.addEventListener("message", (event) => {
    const { data } = event;
    try {
      switch (data.type) {
        case "init":
          mode = data.mode || "sab";
          if (mode === "sab") {
            initRingBuffer(data.sharedBuffer, data.ringBufferBase, data.bufferConstants);
          }
          self.postMessage({ type: "initialized" });
          break;
        case "start":
          if (mode === "sab") {
            start();
          }
          break;
        case "stop":
          stop();
          break;
        case "clear":
          if (mode === "sab") {
            clear();
          }
          break;
        case "debugRaw":
          if (data.messages) {
            decodeRawMessages(data.messages);
          }
          break;
        default:
          console.warn("[DebugWorker] Unknown message type:", data.type);
      }
    } catch (error) {
      console.error("[DebugWorker] Error:", error);
      self.postMessage({
        type: "error",
        error: error.message
      });
    }
  });
  debugWorkerLog("[DebugWorker] Script loaded");
})();
