// Test AudioWorklet processor for verifying reply reception via pollReplies().
// Implements minimal SAB reply buffer polling inline rather than importing
// OscChannel, to test the low-level ring buffer protocol directly.

class ReplyTestProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.replies = [];
    this.collecting = false;

    // SAB reply buffer config (set by initReplyBuffer message)
    this.sabConfig = null;
    this.atomicView = null;
    this.uint8View = null;
    this.headIdx = 0;
    this.tailIdx = 0;
    this.activeIdx = 0;
    this.bufferStart = 0;
    this.bufferSize = 0;
    this.messageMagic = 0;
    this.headerSize = 16;

    // PM reply port (set by initReplyPort message)
    this.replyPort = null;

    this.port.onmessage = (e) => {
      const { data } = e;

      if (data.type === 'initReplyBuffer') {
        // SAB mode: receive buffer config for direct polling
        this.sabConfig = data;
        this.atomicView = new Int32Array(data.sharedBuffer);
        this.uint8View = new Uint8Array(data.sharedBuffer);
        this.dataView = new DataView(data.sharedBuffer);
        this.headIdx = data.headIdx;
        this.tailIdx = data.tailIdx;
        this.activeIdx = data.activeIdx;
        this.bufferStart = data.bufferStart;
        this.bufferSize = data.bufferSize;
        this.messageMagic = data.messageMagic;
        // Activate the slot
        Atomics.store(this.atomicView, this.activeIdx, 1);
        // Reset head/tail
        Atomics.store(this.atomicView, this.headIdx, 0);
        Atomics.store(this.atomicView, this.tailIdx, 0);
        this.port.postMessage({ type: 'ready' });
      }

      if (data.type === 'initReplyPort') {
        // PM mode: receive a MessagePort for reply messages
        this.replyPort = e.ports[0];
        this.replyPort.onmessage = (re) => {
          if (re.data.type === 'oscReplies' && re.data.count > 0) {
            const bufferView = new Uint8Array(re.data.buffer);
            for (let i = 0; i < re.data.count; i++) {
              const entry = re.data.messages[i];
              if (!entry) continue;
              const oscData = bufferView.slice(entry.offset, entry.offset + entry.length);
              let end = 0;
              while (end < oscData.length && oscData[end] !== 0) end++;
              let addr = '';
              for (let j = 0; j < end; j++) addr += String.fromCharCode(oscData[j]);
              this.replies.push(addr);
            }
          }
        };
        this.port.postMessage({ type: 'ready' });
      }

      if (data.type === 'startCollecting') {
        this.replies = [];
        this.collecting = true;
        this.port.postMessage({ type: 'collectingStarted' });
      }

      if (data.type === 'getResults') {
        this.collecting = false;
        if (this.sabConfig) {
          Atomics.store(this.atomicView, this.activeIdx, 0);
        }
        this.port.postMessage({ type: 'results', replies: this.replies });
      }
    };
  }

  // Minimal ring buffer reader for SAB reply polling.
  pollSabReplies() {
    const head = Atomics.load(this.atomicView, this.headIdx);
    const tail = Atomics.load(this.atomicView, this.tailIdx);
    if (head === tail) return;

    let currentTail = tail;
    const uint8 = this.uint8View;
    const dv = this.dataView;
    const bs = this.bufferStart;
    const sz = this.bufferSize;
    const magic = this.messageMagic;
    const hs = this.headerSize;

    // Read up to 16 messages per process() call
    for (let n = 0; n < 16 && currentTail !== head; n++) {
      const pos = bs + currentTail;
      const m = dv.getUint32(pos, true);  // little-endian
      if (m !== magic) break;

      const len = dv.getUint32(pos + 4, true);
      if (len < hs || len > sz) break;

      // Extract OSC address from payload
      const payloadStart = pos + hs;
      const payloadLen = len - hs;
      let end = 0;
      while (end < payloadLen && uint8[payloadStart + end] !== 0) end++;
      // TextDecoder not available in AudioWorklet — manual ASCII decode
      let addr = '';
      for (let j = 0; j < end; j++) addr += String.fromCharCode(uint8[payloadStart + j]);
      this.replies.push(addr);

      currentTail = (currentTail + len) % sz;
    }

    if (currentTail !== tail) {
      Atomics.store(this.atomicView, this.tailIdx, currentTail);
    }
  }

  process() {
    if (this.collecting && this.sabConfig) {
      this.pollSabReplies();
    }
    // PM mode: replies arrive via port.onmessage between process() calls,
    // already pushed to this.replies by the handler above.
    return true;
  }
}

registerProcessor('reply-test-processor', ReplyTestProcessor);
