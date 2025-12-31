(() => {
  // js/lib/metrics_offsets.js
  var OSC_IN_MESSAGES_RECEIVED = 17;
  var OSC_IN_DROPPED_MESSAGES = 18;
  var OSC_IN_BYTES_RECEIVED = 19;

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

  // js/workers/osc_in_worker.js
  var sharedBuffer = null;
  var ringBufferBase = null;
  var atomicView = null;
  var dataView = null;
  var uint8View = null;
  var bufferConstants = null;
  var CONTROL_INDICES = {};
  var metricsView = null;
  var running = false;
  var oscInLog = (...args) => {
    if (true) {
      console.log(...args);
    }
  };
  var lastSequenceReceived = -1;
  var initRingBuffer = (buffer, base, constants) => {
    sharedBuffer = buffer;
    ringBufferBase = base;
    bufferConstants = constants;
    atomicView = new Int32Array(sharedBuffer);
    dataView = new DataView(sharedBuffer);
    uint8View = new Uint8Array(sharedBuffer);
    CONTROL_INDICES = {
      OUT_HEAD: (ringBufferBase + bufferConstants.CONTROL_START + 8) / 4,
      OUT_TAIL: (ringBufferBase + bufferConstants.CONTROL_START + 12) / 4
    };
    const metricsBase = ringBufferBase + bufferConstants.METRICS_START;
    metricsView = new Uint32Array(sharedBuffer, metricsBase, bufferConstants.METRICS_SIZE / 4);
  };
  var readMessages = () => {
    const head = Atomics.load(atomicView, CONTROL_INDICES.OUT_HEAD);
    const tail = Atomics.load(atomicView, CONTROL_INDICES.OUT_TAIL);
    if (head === tail) {
      return [];
    }
    const messages = [];
    const { newTail, messagesRead } = readMessagesFromBuffer({
      uint8View,
      dataView,
      bufferStart: ringBufferBase + bufferConstants.OUT_BUFFER_START,
      bufferSize: bufferConstants.OUT_BUFFER_SIZE,
      head,
      tail,
      messageMagic: bufferConstants.MESSAGE_MAGIC,
      paddingMagic: bufferConstants.PADDING_MAGIC,
      headerSize: bufferConstants.MESSAGE_HEADER_SIZE,
      maxMessages: 100,
      onMessage: (payload, sequence, length) => {
        if (lastSequenceReceived >= 0) {
          const expectedSeq = lastSequenceReceived + 1 & 4294967295;
          if (sequence !== expectedSeq) {
            const dropped = sequence - expectedSeq + 4294967296 & 4294967295;
            if (dropped < 1e3) {
              console.warn("[OSCInWorker] Detected", dropped, "dropped messages (expected seq", expectedSeq, "got", sequence, ")");
              if (metricsView) Atomics.add(metricsView, OSC_IN_DROPPED_MESSAGES, dropped);
            }
          }
        }
        lastSequenceReceived = sequence;
        messages.push({
          oscData: payload,
          sequence
        });
        if (metricsView) {
          Atomics.add(metricsView, OSC_IN_MESSAGES_RECEIVED, 1);
          Atomics.add(metricsView, OSC_IN_BYTES_RECEIVED, payload.length);
        }
      },
      onCorruption: (position) => {
        console.error("[OSCInWorker] Corrupted message at position", position);
        if (metricsView) Atomics.add(metricsView, OSC_IN_DROPPED_MESSAGES, 1);
      }
    });
    if (messagesRead > 0) {
      Atomics.store(atomicView, CONTROL_INDICES.OUT_TAIL, newTail);
    }
    return messages;
  };
  var waitLoop = () => {
    while (running) {
      try {
        const currentHead = Atomics.load(atomicView, CONTROL_INDICES.OUT_HEAD);
        const currentTail = Atomics.load(atomicView, CONTROL_INDICES.OUT_TAIL);
        if (currentHead === currentTail) {
          const result = Atomics.wait(atomicView, CONTROL_INDICES.OUT_HEAD, currentHead, 100);
          if (result === "ok" || result === "not-equal") {
          } else if (result === "timed-out") {
            continue;
          }
        }
        const messages = readMessages();
        if (messages.length > 0) {
          self.postMessage({
            type: "messages",
            messages
          });
        }
      } catch (error) {
        console.error("[OSCInWorker] Error in wait loop:", error);
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
      console.error("[OSCInWorker] Cannot start - not initialized");
      return;
    }
    if (running) {
      console.warn("[OSCInWorker] Already running");
      return;
    }
    running = true;
    waitLoop();
  };
  var stop = () => {
    running = false;
  };
  self.addEventListener("message", (event) => {
    const { data } = event;
    try {
      switch (data.type) {
        case "init":
          initRingBuffer(data.sharedBuffer, data.ringBufferBase, data.bufferConstants);
          self.postMessage({ type: "initialized" });
          break;
        case "start":
          start();
          break;
        case "stop":
          stop();
          break;
        default:
          console.warn("[OSCInWorker] Unknown message type:", data.type);
      }
    } catch (error) {
      console.error("[OSCInWorker] Error:", error);
      self.postMessage({
        type: "error",
        error: error.message
      });
    }
  });
  oscInLog("[OSCInWorker] Script loaded");
})();
