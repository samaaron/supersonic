(() => {
  // js/lib/metrics_offsets.js
  var PRESCHEDULER_PENDING = 6;
  var PRESCHEDULER_PEAK = 7;
  var PRESCHEDULER_SENT = 8;
  var RETRIES_SUCCEEDED = 9;
  var RETRIES_FAILED = 10;
  var BUNDLES_SCHEDULED = 11;
  var EVENTS_CANCELLED = 12;
  var TOTAL_DISPATCHES = 13;
  var MESSAGES_RETRIED = 14;
  var RETRY_QUEUE_SIZE = 15;
  var RETRY_QUEUE_MAX = 16;

  // js/lib/ring_buffer_core.js
  function calculateAvailableSpace(head, tail, bufferSize) {
    return (bufferSize - 1 - head + tail) % bufferSize;
  }
  function writeMessageToBuffer({
    uint8View: uint8View2,
    dataView: dataView2,
    bufferStart,
    bufferSize,
    head,
    payload,
    sequence,
    messageMagic,
    headerSize
  }) {
    const payloadSize = payload.length;
    const totalSize = headerSize + payloadSize;
    const alignedSize = totalSize + 3 & ~3;
    const spaceToEnd = bufferSize - head;
    if (alignedSize > spaceToEnd) {
      const headerBytes = new Uint8Array(headerSize);
      const headerView = new DataView(headerBytes.buffer);
      headerView.setUint32(0, messageMagic, true);
      headerView.setUint32(4, alignedSize, true);
      headerView.setUint32(8, sequence, true);
      headerView.setUint32(12, 0, true);
      const writePos1 = bufferStart + head;
      const writePos2 = bufferStart;
      if (spaceToEnd >= headerSize) {
        uint8View2.set(headerBytes, writePos1);
        const payloadBytesInFirstPart = spaceToEnd - headerSize;
        if (payloadBytesInFirstPart > 0) {
          uint8View2.set(payload.subarray(0, payloadBytesInFirstPart), writePos1 + headerSize);
        }
        uint8View2.set(payload.subarray(payloadBytesInFirstPart), writePos2);
      } else {
        uint8View2.set(headerBytes.subarray(0, spaceToEnd), writePos1);
        uint8View2.set(headerBytes.subarray(spaceToEnd), writePos2);
        const payloadOffset = headerSize - spaceToEnd;
        uint8View2.set(payload, writePos2 + payloadOffset);
      }
    } else {
      const writePos = bufferStart + head;
      dataView2.setUint32(writePos, messageMagic, true);
      dataView2.setUint32(writePos + 4, alignedSize, true);
      dataView2.setUint32(writePos + 8, sequence, true);
      dataView2.setUint32(writePos + 12, 0, true);
      uint8View2.set(payload, writePos + headerSize);
    }
    return (head + alignedSize) % bufferSize;
  }

  // js/lib/ring_buffer_writer.js
  function tryAcquireLock(atomicView2, lockIndex, maxSpins = 0) {
    for (let i = 0; i <= maxSpins; i++) {
      const oldValue = Atomics.compareExchange(atomicView2, lockIndex, 0, 1);
      if (oldValue === 0) {
        return true;
      }
    }
    return false;
  }
  function releaseLock(atomicView2, lockIndex) {
    Atomics.store(atomicView2, lockIndex, 0);
  }
  function writeToRingBuffer({
    atomicView: atomicView2,
    dataView: dataView2,
    uint8View: uint8View2,
    bufferConstants: bufferConstants2,
    ringBufferBase: ringBufferBase2,
    controlIndices,
    oscMessage,
    maxSpins = 0
  }) {
    const payloadSize = oscMessage.length;
    const totalSize = bufferConstants2.MESSAGE_HEADER_SIZE + payloadSize;
    if (totalSize > bufferConstants2.IN_BUFFER_SIZE - bufferConstants2.MESSAGE_HEADER_SIZE) {
      return false;
    }
    if (!tryAcquireLock(atomicView2, controlIndices.IN_WRITE_LOCK, maxSpins)) {
      return false;
    }
    try {
      const head = Atomics.load(atomicView2, controlIndices.IN_HEAD);
      const tail = Atomics.load(atomicView2, controlIndices.IN_TAIL);
      const alignedSize = totalSize + 3 & ~3;
      const available = calculateAvailableSpace(head, tail, bufferConstants2.IN_BUFFER_SIZE);
      if (available < alignedSize) {
        return false;
      }
      const messageSeq = Atomics.add(atomicView2, controlIndices.IN_SEQUENCE, 1);
      const newHead = writeMessageToBuffer({
        uint8View: uint8View2,
        dataView: dataView2,
        bufferStart: ringBufferBase2 + bufferConstants2.IN_BUFFER_START,
        bufferSize: bufferConstants2.IN_BUFFER_SIZE,
        head,
        payload: oscMessage,
        sequence: messageSeq,
        messageMagic: bufferConstants2.MESSAGE_MAGIC,
        headerSize: bufferConstants2.MESSAGE_HEADER_SIZE
      });
      Atomics.load(atomicView2, controlIndices.IN_HEAD);
      Atomics.store(atomicView2, controlIndices.IN_HEAD, newHead);
      return true;
    } finally {
      releaseLock(atomicView2, controlIndices.IN_WRITE_LOCK);
    }
  }

  // js/workers/osc_out_prescheduler_worker.js
  var mode = "sab";
  var sharedBuffer = null;
  var ringBufferBase = null;
  var bufferConstants = null;
  var atomicView = null;
  var dataView = null;
  var uint8View = null;
  var CONTROL_INDICES = {};
  var metricsView = null;
  var localMetricsBuffer = null;
  var metricsSendTimer = null;
  var metricsSendIntervalMs = 25;
  var metricsStore = (offset, value) => {
    if (!metricsView) return;
    if (mode === "sab") {
      Atomics.store(metricsView, offset, value);
    } else {
      metricsView[offset] = value;
    }
  };
  var metricsLoad = (offset) => {
    if (!metricsView) return 0;
    if (mode === "sab") {
      return Atomics.load(metricsView, offset);
    } else {
      return metricsView[offset];
    }
  };
  var metricsAdd = (offset, value) => {
    if (!metricsView) return;
    if (mode === "sab") {
      Atomics.add(metricsView, offset, value);
    } else {
      metricsView[offset] += value;
    }
  };
  var startMetricsSending = () => {
    if (mode !== "postMessage" || metricsSendTimer !== null) return;
    const sendMetrics = () => {
      if (localMetricsBuffer && metricsView) {
        self.postMessage({
          type: "preschedulerMetrics",
          metrics: new Uint32Array(localMetricsBuffer.slice(0))
        });
      }
      metricsSendTimer = setTimeout(sendMetrics, metricsSendIntervalMs);
    };
    sendMetrics();
    schedulerLog("[PreScheduler] Started metrics sending (every " + metricsSendIntervalMs + "ms)");
  };
  var eventHeap = [];
  var periodicTimer = null;
  var sequenceCounter = 0;
  var isDispatching = false;
  var retryQueue = [];
  var MAX_RETRIES_PER_MESSAGE = 5;
  var maxPendingMessages = 65536;
  var NTP_EPOCH_OFFSET = 2208988800;
  var POLL_INTERVAL_MS = 25;
  var LOOKAHEAD_S = 0.2;
  var schedulerLog = (...args) => {
    if (true) {
      console.log(...args);
    }
  };
  var getCurrentNTP = () => {
    const perfTimeMs = performance.timeOrigin + performance.now();
    return perfTimeMs / 1e3 + NTP_EPOCH_OFFSET;
  };
  var extractNTPFromBundle = (oscData) => {
    if (oscData.length >= 16 && oscData[0] === 35) {
      const view = new DataView(oscData.buffer, oscData.byteOffset);
      const ntpSeconds = view.getUint32(8, false);
      const ntpFraction = view.getUint32(12, false);
      return ntpSeconds + ntpFraction / 4294967296;
    }
    return null;
  };
  var initSharedBuffer = () => {
    if (!sharedBuffer || !bufferConstants) {
      console.error("[PreScheduler] Cannot init - missing buffer or constants");
      return;
    }
    atomicView = new Int32Array(sharedBuffer);
    dataView = new DataView(sharedBuffer);
    uint8View = new Uint8Array(sharedBuffer);
    CONTROL_INDICES = {
      IN_HEAD: (ringBufferBase + bufferConstants.CONTROL_START + 0) / 4,
      IN_TAIL: (ringBufferBase + bufferConstants.CONTROL_START + 4) / 4,
      IN_SEQUENCE: (ringBufferBase + bufferConstants.CONTROL_START + 24) / 4,
      IN_WRITE_LOCK: (ringBufferBase + bufferConstants.CONTROL_START + 40) / 4
    };
    const metricsBase = ringBufferBase + bufferConstants.METRICS_START;
    metricsView = new Uint32Array(sharedBuffer, metricsBase, bufferConstants.METRICS_SIZE / 4);
    schedulerLog("[PreScheduler] SharedArrayBuffer initialized with direct ring buffer writing and metrics");
  };
  var updateMetrics = () => {
    if (!metricsView) return;
    metricsStore(PRESCHEDULER_PENDING, eventHeap.length);
    const currentPending = eventHeap.length;
    const currentMax = metricsLoad(PRESCHEDULER_PEAK);
    if (currentPending > currentMax) {
      metricsStore(PRESCHEDULER_PEAK, currentPending);
    }
  };
  var dispatchOSCMessage = (oscMessage, isRetry, timestamp = null) => {
    if (mode === "postMessage") {
      self.postMessage({
        type: "dispatch",
        oscData: oscMessage,
        timestamp
      });
      metricsAdd(PRESCHEDULER_SENT, 1);
      return true;
    }
    if (!sharedBuffer || !atomicView) {
      console.error("[PreScheduler] Not initialized for ring buffer writing");
      return false;
    }
    const payloadSize = oscMessage.length;
    const totalSize = bufferConstants.MESSAGE_HEADER_SIZE + payloadSize;
    if (totalSize > bufferConstants.IN_BUFFER_SIZE - bufferConstants.MESSAGE_HEADER_SIZE) {
      console.error("[PreScheduler] Message too large:", totalSize);
      return false;
    }
    const success = writeToRingBuffer({
      atomicView,
      dataView,
      uint8View,
      bufferConstants,
      ringBufferBase,
      controlIndices: CONTROL_INDICES,
      oscMessage,
      maxSpins: 10
      // Worker can afford brief spinning
    });
    if (!success) {
      if (!isRetry) {
        console.warn("[PreScheduler] Ring buffer full, message will be queued for retry");
      }
      return false;
    }
    metricsAdd(PRESCHEDULER_SENT, 1);
    return true;
  };
  var queueForRetry = (oscData, context) => {
    const totalPending = eventHeap.length + retryQueue.length;
    if (totalPending >= maxPendingMessages) {
      console.error("[PreScheduler] Backpressure: dropping retry (" + totalPending + " pending)");
      metricsAdd(RETRIES_FAILED, 1);
      return;
    }
    retryQueue.push({
      oscData,
      retryCount: 0,
      context: context || "unknown",
      queuedAt: performance.now()
    });
    metricsStore(RETRY_QUEUE_SIZE, retryQueue.length);
    const currentMax = metricsLoad(RETRY_QUEUE_MAX);
    if (retryQueue.length > currentMax) {
      metricsStore(RETRY_QUEUE_MAX, retryQueue.length);
    }
    schedulerLog("[PreScheduler] Queued message for retry:", context, "queue size:", retryQueue.length);
  };
  var processRetryQueue = () => {
    if (retryQueue.length === 0) {
      return;
    }
    let i = 0;
    while (i < retryQueue.length) {
      const item = retryQueue[i];
      const success = dispatchOSCMessage(item.oscData, true);
      if (success) {
        retryQueue.splice(i, 1);
        metricsAdd(RETRIES_SUCCEEDED, 1);
        metricsAdd(MESSAGES_RETRIED, 1);
        metricsStore(RETRY_QUEUE_SIZE, retryQueue.length);
        schedulerLog(
          "[PreScheduler] Retry succeeded for:",
          item.context,
          "after",
          item.retryCount + 1,
          "attempts"
        );
      } else {
        item.retryCount++;
        metricsAdd(MESSAGES_RETRIED, 1);
        if (item.retryCount >= MAX_RETRIES_PER_MESSAGE) {
          const errorMsg = `Ring buffer full - dropped message after ${MAX_RETRIES_PER_MESSAGE} retries (${item.context})`;
          console.error("[PreScheduler]", errorMsg);
          retryQueue.splice(i, 1);
          metricsAdd(RETRIES_FAILED, 1);
          metricsStore(RETRY_QUEUE_SIZE, retryQueue.length);
          self.postMessage({ type: "error", error: errorMsg });
        } else {
          i++;
        }
      }
    }
  };
  var scheduleEvent = (oscData, sessionId, runTag) => {
    const totalPending = eventHeap.length + retryQueue.length;
    if (totalPending >= maxPendingMessages) {
      console.warn("[PreScheduler] Backpressure: rejecting message (" + totalPending + " pending)");
      return false;
    }
    const ntpTime = extractNTPFromBundle(oscData);
    if (ntpTime === null) {
      schedulerLog("[PreScheduler] Non-bundle message, dispatching immediately");
      const success = dispatchOSCMessage(oscData, false);
      if (!success) {
        queueForRetry(oscData, "immediate message");
      }
      return true;
    }
    const currentNTP = getCurrentNTP();
    const timeUntilExec = ntpTime - currentNTP;
    const event = {
      ntpTime,
      seq: sequenceCounter++,
      sessionId: sessionId || 0,
      runTag: runTag || "",
      oscData
    };
    heapPush(event);
    metricsAdd(BUNDLES_SCHEDULED, 1);
    updateMetrics();
    schedulerLog(
      "[PreScheduler] Scheduled bundle:",
      "NTP=" + ntpTime.toFixed(3),
      "current=" + currentNTP.toFixed(3),
      "wait=" + (timeUntilExec * 1e3).toFixed(1) + "ms",
      "pending=" + eventHeap.length
    );
    return true;
  };
  var heapPush = (event) => {
    eventHeap.push(event);
    siftUp(eventHeap.length - 1);
  };
  var heapPeek = () => eventHeap.length > 0 ? eventHeap[0] : null;
  var heapPop = () => {
    if (eventHeap.length === 0) {
      return null;
    }
    const top = eventHeap[0];
    const last = eventHeap.pop();
    if (eventHeap.length > 0) {
      eventHeap[0] = last;
      siftDown(0);
    }
    return top;
  };
  var siftUp = (index) => {
    while (index > 0) {
      const parent = Math.floor((index - 1) / 2);
      if (compareEvents(eventHeap[index], eventHeap[parent]) >= 0) {
        break;
      }
      swap(index, parent);
      index = parent;
    }
  };
  var siftDown = (index) => {
    const length = eventHeap.length;
    while (true) {
      const left = 2 * index + 1;
      const right = 2 * index + 2;
      let smallest = index;
      if (left < length && compareEvents(eventHeap[left], eventHeap[smallest]) < 0) {
        smallest = left;
      }
      if (right < length && compareEvents(eventHeap[right], eventHeap[smallest]) < 0) {
        smallest = right;
      }
      if (smallest === index) {
        break;
      }
      swap(index, smallest);
      index = smallest;
    }
  };
  var compareEvents = (a, b) => {
    if (a.ntpTime === b.ntpTime) {
      return a.seq - b.seq;
    }
    return a.ntpTime - b.ntpTime;
  };
  var swap = (i, j) => {
    const tmp = eventHeap[i];
    eventHeap[i] = eventHeap[j];
    eventHeap[j] = tmp;
  };
  var startPeriodicPolling = () => {
    if (periodicTimer !== null) {
      console.warn("[PreScheduler] Polling already started");
      return;
    }
    schedulerLog("[PreScheduler] Starting periodic polling (every " + POLL_INTERVAL_MS + "ms)");
    checkAndDispatch();
  };
  var checkAndDispatch = () => {
    isDispatching = true;
    processRetryQueue();
    const currentNTP = getCurrentNTP();
    const lookaheadTime = currentNTP + LOOKAHEAD_S;
    let dispatchCount = 0;
    while (eventHeap.length > 0) {
      const nextEvent = heapPeek();
      if (nextEvent.ntpTime <= lookaheadTime) {
        heapPop();
        updateMetrics();
        const timeUntilExec = nextEvent.ntpTime - currentNTP;
        metricsAdd(TOTAL_DISPATCHES, 1);
        schedulerLog(
          "[PreScheduler] Dispatching bundle:",
          "NTP=" + nextEvent.ntpTime.toFixed(3),
          "current=" + currentNTP.toFixed(3),
          "early=" + (timeUntilExec * 1e3).toFixed(1) + "ms",
          "remaining=" + eventHeap.length
        );
        const success = dispatchOSCMessage(nextEvent.oscData, false);
        if (!success) {
          queueForRetry(nextEvent.oscData, "scheduled bundle NTP=" + nextEvent.ntpTime.toFixed(3));
        }
        dispatchCount++;
      } else {
        break;
      }
    }
    if (dispatchCount > 0 || eventHeap.length > 0 || retryQueue.length > 0) {
      schedulerLog(
        "[PreScheduler] Dispatch cycle complete:",
        "dispatched=" + dispatchCount,
        "pending=" + eventHeap.length,
        "retrying=" + retryQueue.length
      );
    }
    isDispatching = false;
    periodicTimer = setTimeout(checkAndDispatch, POLL_INTERVAL_MS);
  };
  var cancelBy = (predicate) => {
    if (eventHeap.length === 0) {
      return;
    }
    const before = eventHeap.length;
    const remaining = [];
    for (let i = 0; i < eventHeap.length; i++) {
      const event = eventHeap[i];
      if (!predicate(event)) {
        remaining.push(event);
      }
    }
    const removed = before - remaining.length;
    if (removed > 0) {
      eventHeap = remaining;
      heapify();
      metricsAdd(EVENTS_CANCELLED, removed);
      updateMetrics();
      schedulerLog("[PreScheduler] Cancelled " + removed + " events, " + eventHeap.length + " remaining");
    }
  };
  var heapify = () => {
    for (let i = Math.floor(eventHeap.length / 2) - 1; i >= 0; i--) {
      siftDown(i);
    }
  };
  var cancelSessionTag = (sessionId, runTag) => {
    cancelBy((event) => event.sessionId === sessionId && event.runTag === runTag);
  };
  var cancelSession = (sessionId) => {
    cancelBy((event) => event.sessionId === sessionId);
  };
  var cancelTag = (runTag) => {
    cancelBy((event) => event.runTag === runTag);
  };
  var cancelAllTags = () => {
    if (eventHeap.length === 0) {
      return;
    }
    const cancelled = eventHeap.length;
    metricsAdd(EVENTS_CANCELLED, cancelled);
    eventHeap = [];
    updateMetrics();
    schedulerLog("[PreScheduler] Cancelled all " + cancelled + " events");
  };
  var isBundle = (data) => {
    if (!data || data.length < 8) {
      return false;
    }
    return data[0] === 35 && data[1] === 98 && data[2] === 117 && data[3] === 110 && data[4] === 100 && data[5] === 108 && data[6] === 101 && data[7] === 0;
  };
  var extractMessagesFromBundle = (data) => {
    const messages = [];
    const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    let offset = 16;
    while (offset < data.length) {
      const messageSize = view.getInt32(offset, false);
      offset += 4;
      if (messageSize <= 0 || offset + messageSize > data.length) {
        break;
      }
      const messageData = data.slice(offset, offset + messageSize);
      messages.push(messageData);
      offset += messageSize;
      while (offset % 4 !== 0 && offset < data.length) {
        offset++;
      }
    }
    return messages;
  };
  var processImmediate = (oscData) => {
    if (isBundle(oscData)) {
      const messages = extractMessagesFromBundle(oscData);
      for (let i = 0; i < messages.length; i++) {
        const success = dispatchOSCMessage(messages[i], false);
        if (!success) {
          queueForRetry(messages[i], "immediate bundle message " + i);
        }
      }
    } else {
      const success = dispatchOSCMessage(oscData, false);
      if (!success) {
        queueForRetry(oscData, "immediate message");
      }
    }
  };
  self.addEventListener("message", (event) => {
    const { data } = event;
    try {
      switch (data.type) {
        case "init":
          mode = data.mode || "sab";
          if (data.maxPendingMessages) {
            maxPendingMessages = data.maxPendingMessages;
          }
          if (data.snapshotIntervalMs) {
            metricsSendIntervalMs = data.snapshotIntervalMs;
          }
          if (mode === "sab") {
            sharedBuffer = data.sharedBuffer;
            ringBufferBase = data.ringBufferBase;
            bufferConstants = data.bufferConstants;
            initSharedBuffer();
          } else {
            const METRICS_SIZE = 128;
            localMetricsBuffer = new ArrayBuffer(METRICS_SIZE);
            metricsView = new Uint32Array(localMetricsBuffer);
            startMetricsSending();
          }
          startPeriodicPolling();
          schedulerLog("[OSCPreSchedulerWorker] Initialized with NTP-based scheduling, mode=" + mode + ", capacity=" + maxPendingMessages);
          self.postMessage({ type: "initialized" });
          break;
        case "send":
          scheduleEvent(
            data.oscData,
            data.sessionId || 0,
            data.runTag || ""
          );
          break;
        case "sendImmediate":
          processImmediate(data.oscData);
          break;
        case "cancelSessionTag":
          if (data.runTag !== void 0 && data.runTag !== null && data.runTag !== "") {
            cancelSessionTag(data.sessionId || 0, data.runTag);
          }
          break;
        case "cancelSession":
          cancelSession(data.sessionId || 0);
          break;
        case "cancelTag":
          if (data.runTag !== void 0 && data.runTag !== null && data.runTag !== "") {
            cancelTag(data.runTag);
          }
          break;
        case "cancelAll":
          cancelAllTags();
          break;
        default:
          console.warn("[OSCPreSchedulerWorker] Unknown message type:", data.type);
      }
    } catch (error) {
      console.error("[OSCPreSchedulerWorker] Error:", error);
      self.postMessage({
        type: "error",
        error: error.message
      });
    }
  });
  schedulerLog("[OSCPreSchedulerWorker] Script loaded");
})();
