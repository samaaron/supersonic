(() => {
  // js/lib/metrics_offsets.js
  var PROCESS_COUNT = 0;
  var MESSAGES_PROCESSED = 1;
  var MESSAGES_DROPPED = 2;
  var SCHEDULER_QUEUE_DEPTH = 3;
  var SCHEDULER_QUEUE_MAX = 4;
  var SCHEDULER_QUEUE_DROPPED = 5;

  // js/lib/ring_buffer_core.js
  function calculateAvailableSpace(head, tail, bufferSize) {
    return (bufferSize - 1 - head + tail) % bufferSize;
  }
  function writeMessageToBuffer({
    uint8View,
    dataView,
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
        uint8View.set(headerBytes, writePos1);
        const payloadBytesInFirstPart = spaceToEnd - headerSize;
        if (payloadBytesInFirstPart > 0) {
          uint8View.set(payload.subarray(0, payloadBytesInFirstPart), writePos1 + headerSize);
        }
        uint8View.set(payload.subarray(payloadBytesInFirstPart), writePos2);
      } else {
        uint8View.set(headerBytes.subarray(0, spaceToEnd), writePos1);
        uint8View.set(headerBytes.subarray(spaceToEnd), writePos2);
        const payloadOffset = headerSize - spaceToEnd;
        uint8View.set(payload, writePos2 + payloadOffset);
      }
    } else {
      const writePos = bufferStart + head;
      dataView.setUint32(writePos, messageMagic, true);
      dataView.setUint32(writePos + 4, alignedSize, true);
      dataView.setUint32(writePos + 8, sequence, true);
      dataView.setUint32(writePos + 12, 0, true);
      uint8View.set(payload, writePos + headerSize);
    }
    return (head + alignedSize) % bufferSize;
  }
  function readMessagesFromBuffer({
    uint8View,
    dataView,
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
      const magic = dataView.getUint32(readPos, true);
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
      const length = dataView.getUint32(readPos + 4, true);
      const sequence = dataView.getUint32(readPos + 8, true);
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
        payload[i] = uint8View[payloadStart + i];
      }
      onMessage(payload, sequence, length);
      currentTail = (currentTail + length) % bufferSize;
      messagesRead++;
    }
    return { newTail: currentTail, messagesRead };
  }

  // js/workers/scsynth_audio_worklet.js
  var ScsynthProcessor = class extends AudioWorkletProcessor {
    constructor() {
      super();
      this.mode = "sab";
      this.sharedBuffer = null;
      this.wasmModule = null;
      this.wasmInstance = null;
      this.isInitialized = false;
      this.processCallCount = 0;
      this.lastStatusCheck = 0;
      this.ringBufferBase = null;
      this.audioView = null;
      this.lastAudioBufferPtr = 0;
      this.lastWasmBufferSize = 0;
      this.lastTreeVersion = -1;
      this.treeSnapshotsSent = 0;
      this.lastTreeSendTime = -1;
      this.treeSnapshotMinInterval = 0.025;
      this.atomicView = null;
      this.uint8View = null;
      this.dataView = null;
      this.localClockOffsetView = null;
      this.bufferConstants = null;
      this.CONTROL_INDICES = null;
      this.metricsView = null;
      this.STATUS_FLAGS = {
        OK: 0,
        BUFFER_FULL: 1 << 0,
        OVERRUN: 1 << 1,
        WASM_ERROR: 1 << 2,
        FRAGMENTED_MSG: 1 << 3
      };
      this.oscQueue = [];
      this.port.onmessage = this.handleMessage.bind(this);
    }
    // Load buffer constants from WASM module
    // Reads the BufferLayout struct exported by C++
    loadBufferConstants() {
      if (!this.wasmInstance || !this.wasmInstance.exports.get_buffer_layout) {
        throw new Error("WASM instance does not export get_buffer_layout");
      }
      const layoutPtr = this.wasmInstance.exports.get_buffer_layout();
      const memory = this.wasmMemory;
      if (!memory) {
        throw new Error("WASM memory not available");
      }
      const uint32View = new Uint32Array(memory.buffer, layoutPtr, 34);
      const uint8View = new Uint8Array(memory.buffer, layoutPtr, 140);
      this.bufferConstants = {
        IN_BUFFER_START: uint32View[0],
        IN_BUFFER_SIZE: uint32View[1],
        OUT_BUFFER_START: uint32View[2],
        OUT_BUFFER_SIZE: uint32View[3],
        DEBUG_BUFFER_START: uint32View[4],
        DEBUG_BUFFER_SIZE: uint32View[5],
        CONTROL_START: uint32View[6],
        CONTROL_SIZE: uint32View[7],
        METRICS_START: uint32View[8],
        METRICS_SIZE: uint32View[9],
        // NODE_TREE now immediately follows METRICS
        NODE_TREE_START: uint32View[10],
        NODE_TREE_SIZE: uint32View[11],
        NODE_TREE_HEADER_SIZE: uint32View[12],
        NODE_TREE_ENTRY_SIZE: uint32View[13],
        NODE_TREE_DEF_NAME_SIZE: uint32View[14],
        NODE_TREE_MAX_NODES: uint32View[15],
        NTP_START_TIME_START: uint32View[16],
        NTP_START_TIME_SIZE: uint32View[17],
        DRIFT_OFFSET_START: uint32View[18],
        DRIFT_OFFSET_SIZE: uint32View[19],
        GLOBAL_OFFSET_START: uint32View[20],
        GLOBAL_OFFSET_SIZE: uint32View[21],
        AUDIO_CAPTURE_START: uint32View[22],
        AUDIO_CAPTURE_SIZE: uint32View[23],
        AUDIO_CAPTURE_HEADER_SIZE: uint32View[24],
        AUDIO_CAPTURE_FRAMES: uint32View[25],
        AUDIO_CAPTURE_CHANNELS: uint32View[26],
        AUDIO_CAPTURE_SAMPLE_RATE: uint32View[27],
        TOTAL_BUFFER_SIZE: uint32View[28],
        MAX_MESSAGE_SIZE: uint32View[29],
        MESSAGE_MAGIC: uint32View[30],
        PADDING_MAGIC: uint32View[31],
        scheduler_slot_size: uint32View[32],
        scheduler_slot_count: uint32View[33],
        DEBUG_PADDING_MARKER: uint8View[136],
        // After 34 uint32s = 136 bytes
        MESSAGE_HEADER_SIZE: 16
        // sizeof(Message) - 4 x uint32_t (magic, length, sequence, padding)
      };
      if (this.bufferConstants.MESSAGE_MAGIC !== 3735928559) {
        throw new Error("Invalid buffer constants from WASM");
      }
    }
    // Calculate buffer indices based on dynamic ring buffer base address
    // Uses constants loaded from WASM via loadBufferConstants()
    calculateBufferIndices(ringBufferBase) {
      if (!this.bufferConstants) {
        throw new Error("Buffer constants not loaded. Call loadBufferConstants() first.");
      }
      const CONTROL_START = this.bufferConstants.CONTROL_START;
      const METRICS_START = this.bufferConstants.METRICS_START;
      this.CONTROL_INDICES = {
        IN_HEAD: (ringBufferBase + CONTROL_START + 0) / 4,
        IN_TAIL: (ringBufferBase + CONTROL_START + 4) / 4,
        OUT_HEAD: (ringBufferBase + CONTROL_START + 8) / 4,
        OUT_TAIL: (ringBufferBase + CONTROL_START + 12) / 4,
        DEBUG_HEAD: (ringBufferBase + CONTROL_START + 16) / 4,
        DEBUG_TAIL: (ringBufferBase + CONTROL_START + 20) / 4,
        IN_SEQUENCE: (ringBufferBase + CONTROL_START + 24) / 4,
        OUT_SEQUENCE: (ringBufferBase + CONTROL_START + 28) / 4,
        DEBUG_SEQUENCE: (ringBufferBase + CONTROL_START + 32) / 4,
        STATUS_FLAGS: (ringBufferBase + CONTROL_START + 36) / 4,
        IN_WRITE_LOCK: (ringBufferBase + CONTROL_START + 40) / 4
      };
      if (this.mode === "sab") {
        const metricsBase = ringBufferBase + METRICS_START;
        this.metricsView = new Uint32Array(this.sharedBuffer, metricsBase, this.bufferConstants.METRICS_SIZE / 4);
      } else {
        this.atomicView = new Int32Array(this.wasmMemory.buffer);
        this.uint8View = new Uint8Array(this.wasmMemory.buffer);
        this.dataView = new DataView(this.wasmMemory.buffer);
        const metricsBase = ringBufferBase + METRICS_START;
        this.metricsView = new Uint32Array(this.wasmMemory.buffer, metricsBase, this.bufferConstants.METRICS_SIZE / 4);
      }
    }
    // Write worldOptions to SharedArrayBuffer for C++ to read
    // WorldOptions are written after ring buffer storage (65536 bytes)
    writeWorldOptionsToMemory() {
      if (!this.worldOptions || !this.wasmMemory) {
        return;
      }
      const WORLD_OPTIONS_OFFSET = this.ringBufferBase + 65536;
      const uint32View = new Uint32Array(this.wasmMemory.buffer, WORLD_OPTIONS_OFFSET, 32);
      const float32View = new Float32Array(this.wasmMemory.buffer, WORLD_OPTIONS_OFFSET, 32);
      uint32View[0] = this.worldOptions.numBuffers || 1024;
      uint32View[1] = this.worldOptions.maxNodes || 1024;
      uint32View[2] = this.worldOptions.maxGraphDefs || 1024;
      uint32View[3] = this.worldOptions.maxWireBufs || 64;
      uint32View[4] = this.worldOptions.numAudioBusChannels || 128;
      uint32View[5] = this.worldOptions.numInputBusChannels || 0;
      uint32View[6] = this.worldOptions.numOutputBusChannels || 0;
      uint32View[7] = this.worldOptions.numControlBusChannels || 4096;
      uint32View[8] = this.worldOptions.bufLength || 128;
      uint32View[9] = this.worldOptions.realTimeMemorySize || 16384;
      uint32View[10] = this.worldOptions.numRGens || 64;
      uint32View[11] = this.worldOptions.realTime ? 1 : 0;
      uint32View[12] = this.worldOptions.memoryLocking ? 1 : 0;
      uint32View[13] = this.worldOptions.loadGraphDefs || 0;
      uint32View[14] = this.worldOptions.preferredSampleRate || 0;
      uint32View[15] = this.worldOptions.verbosity || 0;
    }
    // Write debug message to DEBUG ring buffer
    js_debug(message) {
      if (!this.uint8View || !this.atomicView || !this.CONTROL_INDICES || !this.ringBufferBase) {
        return;
      }
      try {
        const DEBUG_BUFFER_START = this.bufferConstants.DEBUG_BUFFER_START;
        const DEBUG_BUFFER_SIZE = this.bufferConstants.DEBUG_BUFFER_SIZE;
        const DEBUG_PADDING_MARKER = this.bufferConstants.DEBUG_PADDING_MARKER;
        const prefixedMessage = "[JS] " + message + "\n";
        const encoder = new TextEncoder();
        const bytes = encoder.encode(prefixedMessage);
        if (bytes.length > DEBUG_BUFFER_SIZE) {
          return;
        }
        const debugHeadIndex = this.CONTROL_INDICES.DEBUG_HEAD;
        const currentHead = this.atomicLoad(debugHeadIndex);
        const spaceToEnd = DEBUG_BUFFER_SIZE - currentHead;
        let writePos = currentHead;
        if (bytes.length > spaceToEnd) {
          this.uint8View[this.ringBufferBase + DEBUG_BUFFER_START + currentHead] = DEBUG_PADDING_MARKER;
          writePos = 0;
        }
        const debugBufferStart = this.ringBufferBase + DEBUG_BUFFER_START;
        for (let i = 0; i < bytes.length; i++) {
          this.uint8View[debugBufferStart + writePos + i] = bytes[i];
        }
        const newHead = writePos + bytes.length;
        this.atomicStore(debugHeadIndex, newHead);
      } catch (err) {
      }
    }
    // Atomic-safe load - uses Atomics in SAB mode, regular access in postMessage mode
    atomicLoad(index) {
      if (this.mode === "sab") {
        return Atomics.load(this.atomicView, index);
      } else {
        return this.atomicView[index];
      }
    }
    // Atomic-safe store - uses Atomics in SAB mode, regular access in postMessage mode
    atomicStore(index, value) {
      if (this.mode === "sab") {
        Atomics.store(this.atomicView, index, value);
      } else {
        this.atomicView[index] = value;
      }
    }
    // Write queued OSC messages to the IN ring buffer (postMessage mode)
    // Uses shared ring_buffer_core for wrap-around handling
    drainOscQueue() {
      if (this.oscQueue.length === 0) return;
      const IN_BUFFER_SIZE = this.bufferConstants.IN_BUFFER_SIZE;
      const MESSAGE_HEADER_SIZE = this.bufferConstants.MESSAGE_HEADER_SIZE;
      const bufferStart = this.ringBufferBase + this.bufferConstants.IN_BUFFER_START;
      while (this.oscQueue.length > 0) {
        const oscData = this.oscQueue[0];
        const messageLength = oscData.byteLength;
        const totalLength = MESSAGE_HEADER_SIZE + messageLength;
        const alignedLength = totalLength + 3 & ~3;
        const head = this.atomicLoad(this.CONTROL_INDICES.IN_HEAD);
        const tail = this.atomicLoad(this.CONTROL_INDICES.IN_TAIL);
        const available = calculateAvailableSpace(head, tail, IN_BUFFER_SIZE);
        if (alignedLength > available) {
          break;
        }
        this.oscQueue.shift();
        const sequence = this.atomicLoad(this.CONTROL_INDICES.IN_SEQUENCE);
        this.atomicStore(this.CONTROL_INDICES.IN_SEQUENCE, sequence + 1);
        const oscBytes = new Uint8Array(oscData);
        const newHead = writeMessageToBuffer({
          uint8View: this.uint8View,
          dataView: this.dataView,
          bufferStart,
          bufferSize: IN_BUFFER_SIZE,
          head,
          payload: oscBytes,
          sequence,
          messageMagic: this.bufferConstants.MESSAGE_MAGIC,
          headerSize: MESSAGE_HEADER_SIZE
        });
        this.atomicStore(this.CONTROL_INDICES.IN_HEAD, newHead);
      }
    }
    // Read OSC replies from OUT ring buffer and send via postMessage
    // Uses shared ring_buffer_core for read logic
    readOscReplies() {
      const head = this.atomicLoad(this.CONTROL_INDICES.OUT_HEAD);
      const tail = this.atomicLoad(this.CONTROL_INDICES.OUT_TAIL);
      if (head === tail) return;
      const messages = [];
      const { newTail, messagesRead } = readMessagesFromBuffer({
        uint8View: this.uint8View,
        dataView: this.dataView,
        bufferStart: this.ringBufferBase + this.bufferConstants.OUT_BUFFER_START,
        bufferSize: this.bufferConstants.OUT_BUFFER_SIZE,
        head,
        tail,
        messageMagic: this.bufferConstants.MESSAGE_MAGIC,
        paddingMagic: this.bufferConstants.PADDING_MAGIC,
        headerSize: this.bufferConstants.MESSAGE_HEADER_SIZE,
        onMessage: (payload, sequence) => {
          messages.push({ oscData: payload.buffer, sequence });
        }
      });
      if (messagesRead > 0) {
        this.atomicStore(this.CONTROL_INDICES.OUT_TAIL, newTail);
      }
      if (messages.length > 0) {
        this.port.postMessage({
          type: "oscReplies",
          messages
        });
      }
    }
    // Read node tree from WASM memory and return the tree data
    // Returns null if not ready, otherwise { nodeCount, version, nodes }
    readNodeTree() {
      if (!this.bufferConstants || !this.wasmMemory || this.ringBufferBase === null) {
        return null;
      }
      const bc = this.bufferConstants;
      const treeBase = this.ringBufferBase + bc.NODE_TREE_START;
      const headerView = new Uint32Array(this.wasmMemory.buffer, treeBase, 2);
      const nodeCount = headerView[0];
      const version = headerView[1];
      const entriesBase = treeBase + bc.NODE_TREE_HEADER_SIZE;
      const maxNodes = bc.NODE_TREE_MAX_NODES;
      const entrySize = bc.NODE_TREE_ENTRY_SIZE;
      const defNameSize = bc.NODE_TREE_DEF_NAME_SIZE;
      const dataView = new DataView(this.wasmMemory.buffer, entriesBase, maxNodes * entrySize);
      const nodes = [];
      let foundCount = 0;
      for (let i = 0; i < maxNodes && foundCount < nodeCount; i++) {
        const byteOffset = i * entrySize;
        const id = dataView.getInt32(byteOffset, true);
        if (id === -1) continue;
        foundCount++;
        const defNameStart = entriesBase + byteOffset + 24;
        const defNameBytes = new Uint8Array(this.wasmMemory.buffer, defNameStart, defNameSize);
        let defName = "";
        for (let j = 0; j < defNameSize && defNameBytes[j] !== 0; j++) {
          defName += String.fromCharCode(defNameBytes[j]);
        }
        nodes.push({
          id,
          parentId: dataView.getInt32(byteOffset + 4, true),
          isGroup: dataView.getInt32(byteOffset + 8, true) === 1,
          prevId: dataView.getInt32(byteOffset + 12, true),
          nextId: dataView.getInt32(byteOffset + 16, true),
          headId: dataView.getInt32(byteOffset + 20, true),
          defName
        });
      }
      return { nodeCount, version, nodes };
    }
    // Read metrics from WASM memory as raw Uint32Array
    // Returns null if not ready, otherwise a copy of the metrics buffer
    // Same layout as SAB - can be used directly with MetricsOffsets
    readMetrics() {
      if (!this.metricsView) {
        return null;
      }
      return new Uint32Array(this.metricsView);
    }
    // Read metrics + node tree from WASM memory and send via postMessage
    // Sends immediately on tree version change, OR on interval (for metrics updates)
    // METRICS and NODE_TREE are contiguous in memory - copy as one atomic unit
    checkAndSendSnapshot(audioTime) {
      const bc = this.bufferConstants;
      if (!bc || !this.wasmMemory || this.ringBufferBase === null) return;
      const treeBase = this.ringBufferBase + bc.NODE_TREE_START;
      const headerView = new Uint32Array(this.wasmMemory.buffer, treeBase, 2);
      const currentVersion = headerView[1];
      const versionChanged = currentVersion !== this.lastTreeVersion;
      if (versionChanged) {
        this.lastTreeVersion = currentVersion;
        this.lastTreeSendTime = audioTime;
      } else {
        if (this.lastTreeSendTime >= 0 && audioTime - this.lastTreeSendTime < this.treeSnapshotMinInterval) {
          return;
        }
        this.lastTreeSendTime = audioTime;
      }
      const buffer = this.readMetricsAndTreeBuffer();
      if (!buffer) return;
      this.treeSnapshotsSent++;
      this.port.postMessage({
        type: "snapshot",
        buffer,
        snapshotsSent: this.treeSnapshotsSent
      }, [buffer]);
    }
    // Read metrics + node tree as one contiguous memory copy
    // Returns raw ArrayBuffer (transferable) or null if not ready
    // Main thread parses the buffer - keeps worklet fast
    readMetricsAndTreeBuffer() {
      if (!this.bufferConstants || !this.wasmMemory || this.ringBufferBase === null) {
        return null;
      }
      const bc = this.bufferConstants;
      const metricsBase = this.ringBufferBase + bc.METRICS_START;
      const totalSize = bc.METRICS_SIZE + bc.NODE_TREE_SIZE;
      const view = new Uint8Array(this.wasmMemory.buffer, metricsBase, totalSize);
      const buffer = new ArrayBuffer(totalSize);
      new Uint8Array(buffer).set(view);
      return buffer;
    }
    // Read debug messages from DEBUG ring buffer and send via postMessage
    // Uses shared ring_buffer_core for read logic
    readDebugMessages() {
      const head = this.atomicLoad(this.CONTROL_INDICES.DEBUG_HEAD);
      const tail = this.atomicLoad(this.CONTROL_INDICES.DEBUG_TAIL);
      if (head === tail) return;
      const messages = [];
      const { newTail, messagesRead } = readMessagesFromBuffer({
        uint8View: this.uint8View,
        dataView: this.dataView,
        bufferStart: this.ringBufferBase + this.bufferConstants.DEBUG_BUFFER_START,
        bufferSize: this.bufferConstants.DEBUG_BUFFER_SIZE,
        head,
        tail,
        messageMagic: this.bufferConstants.MESSAGE_MAGIC,
        paddingMagic: this.bufferConstants.PADDING_MAGIC,
        headerSize: this.bufferConstants.MESSAGE_HEADER_SIZE,
        onMessage: (payload, sequence) => {
          messages.push({
            bytes: payload.buffer,
            sequence
          });
        }
      });
      if (messagesRead > 0) {
        this.atomicStore(this.CONTROL_INDICES.DEBUG_TAIL, newTail);
      }
      if (messages.length > 0) {
        this.port.postMessage({
          type: "debugRawBatch",
          messages
        }, messages.map((m) => m.bytes));
      }
    }
    async handleMessage(event) {
      const { data } = event;
      try {
        if (data.type === "osc") {
          if (this.mode === "postMessage") {
            if (data.oscData) {
              this.oscQueue.push(data.oscData);
            }
          }
          return;
        }
        if (data.type === "init") {
          this.mode = data.mode || "sab";
          if (data.snapshotIntervalMs) {
            this.treeSnapshotMinInterval = data.snapshotIntervalMs / 1e3;
          }
          if (this.mode === "sab" && data.sharedBuffer) {
            this.sharedBuffer = data.sharedBuffer;
            this.atomicView = new Int32Array(this.sharedBuffer);
            this.uint8View = new Uint8Array(this.sharedBuffer);
            this.dataView = new DataView(this.sharedBuffer);
          }
        }
        if (data.type === "loadWasm") {
          if (data.wasmBytes) {
            let memory;
            if (this.mode === "sab") {
              memory = data.wasmMemory;
              if (!memory) {
                this.port.postMessage({
                  type: "error",
                  error: "No WASM memory provided!"
                });
                return;
              }
            } else {
              const memoryPages = data.memoryPages || 1280;
              memory = new WebAssembly.Memory({
                initial: memoryPages,
                maximum: memoryPages,
                shared: true
              });
            }
            this.wasmMemory = memory;
            this.worldOptions = data.worldOptions || {};
            this.sampleRate = data.sampleRate || 48e3;
            const imports = {
              env: {
                memory,
                // Time
                emscripten_asm_const_double: () => Date.now() * 1e3,
                // Filesystem syscalls
                __syscall_getdents64: () => 0,
                __syscall_unlinkat: () => 0,
                // pthread stubs (no-ops - AudioWorklet doesn't support threading)
                _emscripten_init_main_thread_js: () => {
                },
                _emscripten_thread_mailbox_await: () => {
                },
                _emscripten_thread_set_strongref: () => {
                },
                emscripten_exit_with_live_runtime: () => {
                },
                _emscripten_receive_on_main_thread_js: () => {
                },
                emscripten_check_blocking_allowed: () => {
                },
                _emscripten_thread_cleanup: () => {
                },
                emscripten_num_logical_cores: () => 1,
                // Report 1 core
                _emscripten_notify_mailbox_postmessage: () => {
                }
              },
              wasi_snapshot_preview1: {
                clock_time_get: (clockid, precision, timestamp_ptr) => {
                  const view = new DataView(memory.buffer);
                  const nanos = BigInt(Math.floor(Date.now() * 1e6));
                  view.setBigUint64(timestamp_ptr, nanos, true);
                  return 0;
                },
                environ_sizes_get: () => 0,
                environ_get: () => 0,
                fd_close: () => 0,
                fd_write: () => 0,
                fd_seek: () => 0,
                fd_read: () => 0,
                proc_exit: (code) => {
                  console.error("[AudioWorklet] WASM tried to exit with code:", code);
                }
              }
            };
            const module = await WebAssembly.compile(data.wasmBytes);
            this.wasmInstance = await WebAssembly.instantiate(module, imports);
            if (this.wasmInstance.exports.get_ring_buffer_base) {
              this.ringBufferBase = this.wasmInstance.exports.get_ring_buffer_base();
              this.loadBufferConstants();
              this.calculateBufferIndices(this.ringBufferBase);
              this.writeWorldOptionsToMemory();
              if (this.wasmInstance.exports.init_memory) {
                this.wasmInstance.exports.init_memory(this.sampleRate);
                this.isInitialized = true;
                const initialSnapshot = this.mode === "postMessage" ? this.readMetricsAndTreeBuffer() : void 0;
                const msg = {
                  type: "initialized",
                  success: true,
                  ringBufferBase: this.ringBufferBase,
                  bufferConstants: this.bufferConstants,
                  exports: Object.keys(this.wasmInstance.exports),
                  initialSnapshot
                };
                this.port.postMessage(msg, initialSnapshot ? [initialSnapshot] : []);
              }
            }
          } else if (data.wasmInstance) {
            this.wasmInstance = data.wasmInstance;
            if (this.wasmInstance.exports.get_ring_buffer_base) {
              this.ringBufferBase = this.wasmInstance.exports.get_ring_buffer_base();
              this.loadBufferConstants();
              this.calculateBufferIndices(this.ringBufferBase);
              this.writeWorldOptionsToMemory();
              if (this.wasmInstance.exports.init_memory) {
                this.wasmInstance.exports.init_memory(this.sampleRate);
                this.isInitialized = true;
                const initialSnapshot = this.mode === "postMessage" ? this.readMetricsAndTreeBuffer() : void 0;
                const msg = {
                  type: "initialized",
                  success: true,
                  ringBufferBase: this.ringBufferBase,
                  bufferConstants: this.bufferConstants,
                  exports: Object.keys(this.wasmInstance.exports),
                  initialSnapshot
                };
                this.port.postMessage(msg, initialSnapshot ? [initialSnapshot] : []);
              }
            }
          }
        }
        if (data.type === "getVersion") {
          if (this.wasmInstance && this.wasmInstance.exports.get_supersonic_version_string) {
            const versionPtr = this.wasmInstance.exports.get_supersonic_version_string();
            const memory = new Uint8Array(this.wasmMemory.buffer);
            let version = "";
            for (let i = versionPtr; memory[i] !== 0; i++) {
              version += String.fromCharCode(memory[i]);
            }
            this.port.postMessage({
              type: "version",
              version
            });
          } else {
            this.port.postMessage({
              type: "version",
              version: "unknown"
            });
          }
        }
        if (data.type === "getTimeOffset") {
          if (this.wasmInstance && this.wasmInstance.exports.get_time_offset) {
            const offset = this.wasmInstance.exports.get_time_offset();
            this.port.postMessage({
              type: "timeOffset",
              offset
            });
          } else {
            console.error("[AudioWorklet] get_time_offset not available! wasmInstance:", !!this.wasmInstance);
            this.port.postMessage({
              type: "error",
              error: "get_time_offset function not available in WASM exports"
            });
          }
        }
        if (data.type === "setNTPStartTime") {
          if (this.wasmMemory && this.ringBufferBase !== null && this.bufferConstants) {
            const offset = this.ringBufferBase + this.bufferConstants.NTP_START_TIME_START;
            const view = new Float64Array(this.wasmMemory.buffer, offset, 1);
            view[0] = data.ntpStartTime;
          }
        }
        if (data.type === "setDriftOffset") {
          if (this.wasmMemory && this.ringBufferBase !== null && this.bufferConstants) {
            const offset = this.ringBufferBase + this.bufferConstants.DRIFT_OFFSET_START;
            const view = new Int32Array(this.wasmMemory.buffer, offset, 1);
            view[0] = data.driftOffsetMs;
          }
        }
        if (data.type === "setGlobalOffset") {
          if (this.wasmMemory && this.ringBufferBase !== null && this.bufferConstants) {
            const offset = this.ringBufferBase + this.bufferConstants.GLOBAL_OFFSET_START;
            const view = new Int32Array(this.wasmMemory.buffer, offset, 1);
            view[0] = data.globalOffsetMs;
          }
        }
        if (data.type === "getMetrics") {
          const metrics = this.metricsView ? new Uint32Array(this.metricsView) : null;
          this.port.postMessage({
            type: "metricsSnapshot",
            requestId: data.requestId,
            metrics
          });
        }
        if (data.type === "copyBufferData") {
          try {
            const { copyId, ptr, data: bufferData } = data;
            if (!this.wasmMemory || !this.wasmMemory.buffer) {
              throw new Error("WASM memory not initialized");
            }
            const floatData = new Float32Array(bufferData);
            const wasmFloatView = new Float32Array(this.wasmMemory.buffer, ptr, floatData.length);
            wasmFloatView.set(floatData);
            if (true) {
              console.log(`[AudioWorklet] Copied ${floatData.length} samples to WASM memory at offset ${ptr}`);
            }
            this.port.postMessage({
              type: "bufferCopied",
              copyId,
              success: true
            });
          } catch (copyError) {
            console.error("[AudioWorklet] Buffer copy failed:", copyError);
            this.port.postMessage({
              type: "bufferCopied",
              copyId: data.copyId,
              success: false,
              error: copyError.message
            });
          }
        }
      } catch (error) {
        console.error("[AudioWorklet] Error handling message:", error);
        this.port.postMessage({
          type: "error",
          error: error.message,
          stack: error.stack
        });
      }
    }
    process(inputs, outputs, parameters) {
      this.processCallCount++;
      if (!this.isInitialized) {
        return true;
      }
      try {
        if (this.wasmInstance && this.wasmInstance.exports.process_audio) {
          if (this.mode === "postMessage") {
            this.drainOscQueue();
          }
          const audioContextTime = currentTime;
          const inputChannels = inputs[0]?.length || 0;
          const outputChannels = outputs[0]?.length || 0;
          if (inputChannels > 0 && this.wasmInstance?.exports?.get_audio_input_bus) {
            try {
              const inputBusPtr = this.wasmInstance.exports.get_audio_input_bus();
              const numSamples = this.wasmInstance.exports.get_audio_buffer_samples();
              if (inputBusPtr && inputBusPtr > 0) {
                const memBuffer = this.sharedBuffer || this.wasmMemory?.buffer;
                if (memBuffer) {
                  const configuredChannels = this.worldOptions?.numInputBusChannels || 2;
                  const effectiveChannels = Math.min(inputChannels, configuredChannels);
                  if (!this.inputView || this.lastInputBusPtr !== inputBusPtr || this.lastInputChannels !== configuredChannels) {
                    this.inputView = new Float32Array(memBuffer, inputBusPtr, numSamples * configuredChannels);
                    this.lastInputBusPtr = inputBusPtr;
                    this.lastInputChannels = configuredChannels;
                  }
                  for (let ch = 0; ch < effectiveChannels; ch++) {
                    if (inputs[0]?.[ch]) {
                      this.inputView.set(inputs[0][ch], ch * numSamples);
                    }
                  }
                }
              }
            } catch (err) {
            }
          }
          const keepAlive = this.wasmInstance.exports.process_audio(
            audioContextTime,
            outputChannels,
            inputChannels
          );
          if (this.wasmInstance.exports.get_audio_output_bus && outputs[0] && outputs[0].length >= 2) {
            try {
              const audioBufferPtr = this.wasmInstance.exports.get_audio_output_bus();
              const numSamples = this.wasmInstance.exports.get_audio_buffer_samples();
              if (audioBufferPtr && audioBufferPtr > 0) {
                const wasmMemory = this.wasmInstance.exports.memory || this.wasmMemory;
                if (!wasmMemory || !wasmMemory.buffer) {
                  return true;
                }
                const currentBuffer = wasmMemory.buffer;
                const bufferSize = currentBuffer.byteLength;
                const requiredBytes = audioBufferPtr + numSamples * 2 * 4;
                if (audioBufferPtr < 0 || audioBufferPtr > bufferSize || requiredBytes > bufferSize) {
                  return true;
                }
                if (!this.audioView || this.lastAudioBufferPtr !== audioBufferPtr || this.lastWasmBufferSize !== bufferSize || currentBuffer !== this.audioView.buffer) {
                  this.audioView = new Float32Array(currentBuffer, audioBufferPtr, numSamples * 2);
                  this.lastAudioBufferPtr = audioBufferPtr;
                  this.lastWasmBufferSize = bufferSize;
                }
                outputs[0][0].set(this.audioView.subarray(0, numSamples));
                outputs[0][1].set(this.audioView.subarray(numSamples, numSamples * 2));
              }
            } catch (err) {
            }
          }
          if (this.mode === "postMessage") {
            this.readOscReplies();
            this.readDebugMessages();
            this.checkAndSendSnapshot(audioContextTime);
          } else {
            if (this.atomicView) {
              const head = this.atomicLoad(this.CONTROL_INDICES.OUT_HEAD);
              const tail = this.atomicLoad(this.CONTROL_INDICES.OUT_TAIL);
              if (head !== tail) {
                Atomics.notify(this.atomicView, this.CONTROL_INDICES.OUT_HEAD, 1);
              }
            }
          }
          if (this.processCallCount % 3750 === 0) {
            this.checkStatus();
          }
          return keepAlive !== 0;
        }
      } catch (error) {
        console.error("[AudioWorklet] process() error:", error);
        console.error("[AudioWorklet] Stack:", error.stack);
        if (this.atomicView && this.mode === "sab") {
          Atomics.or(this.atomicView, this.CONTROL_INDICES.STATUS_FLAGS, this.STATUS_FLAGS.WASM_ERROR);
        }
      }
      return true;
    }
    checkStatus() {
      if (!this.atomicView) return;
      const statusFlags = this.atomicLoad(this.CONTROL_INDICES.STATUS_FLAGS);
      if (statusFlags !== this.STATUS_FLAGS.OK) {
        const status = {
          bufferFull: !!(statusFlags & this.STATUS_FLAGS.BUFFER_FULL),
          overrun: !!(statusFlags & this.STATUS_FLAGS.OVERRUN),
          wasmError: !!(statusFlags & this.STATUS_FLAGS.WASM_ERROR),
          fragmented: !!(statusFlags & this.STATUS_FLAGS.FRAGMENTED_MSG)
        };
        const metrics = {
          processCount: this.metricsView[PROCESS_COUNT],
          messagesProcessed: this.metricsView[MESSAGES_PROCESSED],
          messagesDropped: this.metricsView[MESSAGES_DROPPED],
          schedulerQueueDepth: this.metricsView[SCHEDULER_QUEUE_DEPTH],
          schedulerQueueMax: this.metricsView[SCHEDULER_QUEUE_MAX],
          schedulerQueueDropped: this.metricsView[SCHEDULER_QUEUE_DROPPED]
        };
        this.port.postMessage({
          type: "status",
          flags: statusFlags,
          status,
          metrics
        });
        const persistentFlags = statusFlags & this.STATUS_FLAGS.BUFFER_FULL;
        this.atomicStore(this.CONTROL_INDICES.STATUS_FLAGS, persistentFlags);
      }
    }
  };
  registerProcessor("scsynth-processor", ScsynthProcessor);
})();
