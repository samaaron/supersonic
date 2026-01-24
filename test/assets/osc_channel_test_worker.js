// Test worker that uses OscChannel for direct worklet communication
// Tests that the bypassLookaheadS threshold is correctly passed through

import { OscChannel } from "../../dist/supersonic.js";

const NTP_EPOCH_OFFSET = 2208988800;
const getNTP = () => (performance.timeOrigin + performance.now()) / 1000 + NTP_EPOCH_OFFSET;

// OSC bundle header: "#bundle\0"
const BUNDLE_HEADER = new Uint8Array([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00]);

let oscChannel = null;
let lastTransferData = null;

// Simple OSC encoder for /status message
function encodeStatusMessage() {
  // /status\0 padded to 8 bytes + ,\0\0\0 = 12 bytes total
  return new Uint8Array([
    0x2f, 0x73, 0x74, 0x61, 0x74, 0x75, 0x73, 0x00, // /status\0
    0x2c, 0x00, 0x00, 0x00, // ,\0\0\0
  ]);
}

function createBundle(offsetMs) {
  const targetNTP = getNTP() + offsetMs / 1000;
  const statusMsg = encodeStatusMessage();
  const bundle = new Uint8Array(16 + 4 + statusMsg.length);

  bundle.set(BUNDLE_HEADER, 0);
  const view = new DataView(bundle.buffer);
  view.setUint32(8, Math.floor(targetNTP), false);
  view.setUint32(12, Math.floor((targetNTP % 1) * 0x100000000), false);
  view.setInt32(16, statusMsg.length, false);
  bundle.set(statusMsg, 20);

  return bundle;
}

self.onmessage = (e) => {
  const { type, ...data } = e.data;

  switch (type) {
    case "initChannel":
      // Receive OscChannel from main thread
      if (data.channel) {
        lastTransferData = data.channel;  // Save for SAB debugging
        oscChannel = OscChannel.fromTransferable(data.channel);
        self.postMessage({
          type: "channelReady",
          mode: oscChannel.mode,
          hasSharedBuffer: !!data.channel.sharedBuffer,
          sharedBufferSize: data.channel.sharedBuffer?.byteLength ?? 0,
        });
      }
      break;

    case "classify":
      // Classify a bundle with the given offset and return the category
      if (!oscChannel) {
        self.postMessage({ type: "error", error: "No channel" });
        return;
      }
      const bundle = createBundle(data.offsetMs);
      const category = oscChannel.classify(bundle);
      self.postMessage({
        type: "classified",
        offsetMs: data.offsetMs,
        category,
      });
      break;

    case "sendBundle":
      // Send a bundle and report metrics
      if (!oscChannel) {
        self.postMessage({ type: "error", error: "No channel" });
        return;
      }
      const sendBundle = createBundle(data.offsetMs);
      const success = oscChannel.send(sendBundle);
      const metrics = oscChannel.getMetrics();
      self.postMessage({
        type: "sent",
        offsetMs: data.offsetMs,
        success,
        metrics,
      });
      break;

    case "sendMultiple":
      // Send multiple messages and report count
      if (!oscChannel) {
        self.postMessage({ type: "error", error: "No channel" });
        return;
      }
      let sentCount = 0;
      let totalBytes = 0;

      // For SAB mode verification: read OSC_OUT_MESSAGES_SENT (index 23) before sends
      let sabBefore = null;
      if (lastTransferData?.sharedBuffer) {
        const view = new Int32Array(lastTransferData.sharedBuffer);
        sabBefore = Atomics.load(view, 23);  // OSC_OUT_MESSAGES_SENT
      }

      for (let i = 0; i < data.count; i++) {
        const msg = createBundle(data.offsetMs ?? 0);
        if (oscChannel.send(msg)) {
          sentCount++;
          totalBytes += msg.byteLength;
        }
      }

      // For SAB mode verification: read after sends
      let sabAfter = null;
      if (lastTransferData?.sharedBuffer) {
        const view = new Int32Array(lastTransferData.sharedBuffer);
        sabAfter = Atomics.load(view, 23);  // OSC_OUT_MESSAGES_SENT
      }

      self.postMessage({
        type: "sentMultiple",
        requested: data.count,
        sent: sentCount,
        bytes: totalBytes,
        metrics: oscChannel.getMetrics(),
        sabDebug: { before: sabBefore, after: sabAfter },
      });
      break;

    case "getMetrics":
      if (!oscChannel) {
        self.postMessage({ type: "error", error: "No channel" });
        return;
      }
      self.postMessage({
        type: "metrics",
        metrics: oscChannel.getMetrics(),
      });
      break;

    case "resetMetrics":
      if (!oscChannel) {
        self.postMessage({ type: "error", error: "No channel" });
        return;
      }
      self.postMessage({
        type: "metricsReset",
        metrics: oscChannel.getAndResetMetrics(),
      });
      break;
  }
};

self.postMessage({ type: "ready" });
