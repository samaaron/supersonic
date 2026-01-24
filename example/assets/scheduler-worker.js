// Scheduler Worker - runs timing loops isolated from main thread jank
// Sends OSC directly to AudioWorklet via OscChannel, bypassing main thread

import { OscChannel } from "../dist/supersonic.js";

const NTP_EPOCH_OFFSET = 2208988800;
const LOOKAHEAD = 0.5;

// Direct channel to AudioWorklet (works in both SAB and PM modes)
let oscChannel = null;

// OSC bundle header: ASCII "#bundle\0"
const OSC_BUNDLE_HEADER = new Uint8Array([
  0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00,
]);

// Config received from main thread (groups, buses, loopConfig)
let config = null;

// Kick patterns (only used in this worker)
const KICK_PATTERNS = {
  off:        [0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0],
  four:       [1,0,0,0, 1,0,0,0, 1,0,0,0, 1,0,0,0],
  half:       [1,0,0,0, 0,0,0,0, 1,0,0,0, 0,0,0,0],
  offbeat:    [0,0,1,0, 0,0,1,0, 0,0,1,0, 0,0,1,0],
  syncopated: [1,0,0,0, 0,0,1,0, 0,0,0,0, 1,0,0,0],
  breakbeat:  [1,0,1,0, 0,0,0,0, 1,0,1,0, 0,0,0,0],
};

// ===== STATE (updated via postMessage from main thread) =====
const state = {
  synth: "beep",
  rootNote: 60,
  octaves: 2,
  amplitude: 0.5,
  attack: 0,
  release: 0.1,
  arpMode: "random",
  bpm: 120,
  kickPattern: "four",
  kickSample: "bd_haus",
  loopSample: "loop_amen",
};

// ===== TIMING =====
let playbackStartNTP = null;

const getNTP = () =>
  (performance.timeOrigin + performance.now()) / 1000 + NTP_EPOCH_OFFSET;

const getBpmScale = () => state.bpm / 120;

// ===== OSC ENCODING =====
// Minimal OSC encoder (subset of what we need)
function oscEncode(msg) {
  const address = msg.address;
  const args = msg.args || [];

  // Calculate address size (null-padded to 4-byte boundary)
  const addressBytes = new TextEncoder().encode(address);
  const addressPadded = Math.ceil((addressBytes.length + 1) / 4) * 4;

  // Build type tag string
  let typeTag = ",";
  for (const arg of args) {
    typeTag += arg.type;
  }
  const typeTagBytes = new TextEncoder().encode(typeTag);
  const typeTagPadded = Math.ceil((typeTagBytes.length + 1) / 4) * 4;

  // Calculate args size
  let argsSize = 0;
  for (const arg of args) {
    if (arg.type === "i" || arg.type === "f") argsSize += 4;
    else if (arg.type === "s") {
      const strBytes = new TextEncoder().encode(arg.value);
      argsSize += Math.ceil((strBytes.length + 1) / 4) * 4;
    }
  }

  // Build buffer
  const totalSize = addressPadded + typeTagPadded + argsSize;
  const buffer = new ArrayBuffer(totalSize);
  const view = new DataView(buffer);
  const bytes = new Uint8Array(buffer);

  let offset = 0;

  // Write address
  bytes.set(addressBytes, offset);
  offset = addressPadded;

  // Write type tag
  bytes.set(typeTagBytes, offset);
  offset += typeTagPadded;

  // Write args
  for (const arg of args) {
    if (arg.type === "i") {
      view.setInt32(offset, arg.value, false);
      offset += 4;
    } else if (arg.type === "f") {
      view.setFloat32(offset, arg.value, false);
      offset += 4;
    } else if (arg.type === "s") {
      const strBytes = new TextEncoder().encode(arg.value);
      bytes.set(strBytes, offset);
      offset += Math.ceil((strBytes.length + 1) / 4) * 4;
    }
  }

  return new Uint8Array(buffer);
}

function createOSCBundle(ntpTime, messages) {
  const encoded = messages.map((m) => oscEncode(m));
  const size = 16 + encoded.reduce((sum, m) => sum + 4 + m.byteLength, 0);
  const bundle = new Uint8Array(size);
  const view = new DataView(bundle.buffer);

  bundle.set(OSC_BUNDLE_HEADER, 0);
  view.setUint32(8, Math.floor(ntpTime), false);
  view.setUint32(12, Math.floor((ntpTime % 1) * 0x100000000), false);

  let offset = 16;
  for (const msg of encoded) {
    view.setInt32(offset, msg.byteLength, false);
    bundle.set(msg, offset + 4);
    offset += 4 + msg.byteLength;
  }
  return bundle;
}

// ===== HELPERS =====
function oscArgs(...args) {
  return args.map((v) => {
    if (typeof v === "string") return { type: "s", value: v };
    if (Number.isInteger(v)) return { type: "i", value: v };
    return { type: "f", value: v };
  });
}

function getMinorPentatonicScale(root, octaves) {
  const intervals = [0, 3, 5, 7, 10];
  const scale = [];
  for (let o = 0; o < octaves; o++) {
    for (const i of intervals) scale.push(root + i + o * 12);
  }
  return scale;
}

// ===== ARP PATTERN STATE =====
let patternIndex = 0;
let arpDirection = 1;

function getArpNote(scale) {
  const len = scale.length;
  if (len === 0) return 60;

  switch (state.arpMode) {
    case "random":
      return scale[Math.floor(Math.random() * len)];
    case "up":
      return scale[patternIndex++ % len];
    case "down":
      return scale[(len - 1) - (patternIndex++ % len)];
    case "updown":
    case "downup": {
      const idx = patternIndex % len;
      const note = arpDirection === 1 ? scale[idx] : scale[(len - 1) - idx];
      patternIndex++;
      if (patternIndex >= len) {
        patternIndex = 0;
        arpDirection *= -1;
      }
      if (state.arpMode === "downup") {
        return arpDirection === 1 ? scale[(len - 1) - idx] : scale[idx];
      }
      return note;
    }
    default:
      return scale[Math.floor(Math.random() * len)];
  }
}

// ===== SCHEDULER CLASS =====
class Scheduler {
  constructor(name, { getInterval, getBatchSize = () => 4, createMessage, getGroup }) {
    this.name = name;
    this.getInterval = getInterval;
    this.getBatchSize = getBatchSize;
    this.createMessage = createMessage;
    this.getGroup = getGroup;
    this.running = false;
    this.counter = 0;
    this.currentTime = null;
    this.timeoutId = null;
  }

  start() {
    if (this.running) return;
    this.running = true;

    const interval = this.getInterval();
    const now = getNTP();

    if (playbackStartNTP === null) {
      // Nothing playing - start immediately
      playbackStartNTP = now;
      this.counter = 0;
      this.currentTime = playbackStartNTP;
    } else {
      // Sync to existing playback - align to next bar
      const barDuration = 2 / getBpmScale();
      const elapsed = now - playbackStartNTP;
      const nextBar = Math.ceil(elapsed / barDuration) * barDuration;
      this.counter = Math.round(nextBar / interval);
      this.currentTime = playbackStartNTP + nextBar;
    }

    const delay = Math.max(0, (this.currentTime - now) * 1000);
    this.timeoutId = setTimeout(() => this.running && this.scheduleBatch(), delay);
  }

  scheduleBatch() {
    if (!this.running) return;

    const interval = this.getInterval();
    const batchSize = this.getBatchSize();

    for (let i = 0; i < batchSize; i++) {
      const targetNTP = this.currentTime + LOOKAHEAD;
      this.currentTime += interval;

      const message = this.createMessage(this.counter + i);
      if (message) {
        const bundle = createOSCBundle(targetNTP, [message]);
        // Send directly to AudioWorklet (bypasses main thread)
        sendOSC(bundle);
      }
    }

    this.counter += batchSize;

    const now = getNTP();
    const nextDelay = Math.max(50, (this.currentTime - now) * 1000 - 500);
    this.timeoutId = setTimeout(() => this.running && this.scheduleBatch(), nextDelay);
  }

  stop(freeGroup = false) {
    this.running = false;
    this.currentTime = null;
    if (this.timeoutId) {
      clearTimeout(this.timeoutId);
      this.timeoutId = null;
    }
    if (freeGroup) {
      // Send g_freeAll command directly to worklet
      const msg = { address: "/g_freeAll", args: oscArgs(this.getGroup()) };
      const encoded = oscEncode(msg);
      sendOSC(encoded);
    }
    // Reset playbackStartNTP if all schedulers stopped
    if (!arpScheduler.running && !kickScheduler.running && !amenScheduler.running) {
      playbackStartNTP = null;
    }
  }
}


// ===== SCHEDULERS =====
// Use getters for config values so they read from received config
const arpScheduler = new Scheduler("Arp", {
  getInterval: () => 0.125 / getBpmScale(),
  createMessage: () => {
    if (!config) return null;
    const scale = getMinorPentatonicScale(state.rootNote, state.octaves);
    const note = getArpNote(scale);

    return {
      address: "/s_new",
      args: oscArgs(
        `sonic-pi-${state.synth}`,
        -1,
        0,
        config.groups.GROUP_ARP,
        "note",
        note,
        "out_bus",
        config.buses.FX_BUS_ARP,
        "amp",
        state.amplitude * 0.5,
        "attack",
        state.attack,
        "release",
        state.release,
      ),
    };
  },
  getGroup: () => config?.groups.GROUP_ARP ?? 100,
});

const kickScheduler = new Scheduler("Kick", {
  getInterval: () => 0.125 / getBpmScale(),
  createMessage: (counter) => {
    if (!config) return null;
    const pattern = KICK_PATTERNS[state.kickPattern] || KICK_PATTERNS.four;
    const step = counter % 16;
    const shouldPlay = pattern[step];

    if (!shouldPlay) return null;

    const kickCfg = config.kickConfig?.[state.kickSample] || { buffer: 0 };
    return {
      address: "/s_new",
      args: oscArgs(
        "sonic-pi-basic_stereo_player",
        -1,
        0,
        config.groups.GROUP_KICK,
        "buf",
        kickCfg.buffer,
        "out_bus",
        config.buses.FX_BUS_BEAT,
        "amp",
        0.3,
      ),
    };
  },
  getGroup: () => config?.groups.GROUP_KICK ?? 103,
});

const amenScheduler = new Scheduler("Amen", {
  getInterval: () => {
    if (!config) return 2 / getBpmScale();
    const loopConfig = config.loopConfig;
    return (loopConfig[state.loopSample]?.duration ?? 2) / getBpmScale();
  },
  getBatchSize: () => 1,
  createMessage: () => {
    if (!config) return null;
    const loopConfig = config.loopConfig;
    const cfg = loopConfig[state.loopSample] || { rate: 1, buffer: 1 };
    return {
      address: "/s_new",
      args: oscArgs(
        "sonic-pi-basic_stereo_player",
        -1,
        0,
        config.groups.GROUP_LOOPS,
        "buf",
        cfg.buffer,
        "amp",
        0.5,
        "rate",
        cfg.rate * getBpmScale(),
        "out_bus",
        config.buses.FX_BUS_BEAT,
      ),
    };
  },
  getGroup: () => config?.groups.GROUP_LOOPS ?? 102,
});

// ===== SEND OSC =====
// Send directly to AudioWorklet via OscChannel, bypassing main thread
// Works transparently in both SAB mode (ring buffer) and PM mode (MessagePort)
function sendOSC(oscData) {
  if (oscChannel) {
    oscChannel.send(oscData);
  } else {
    // Fallback: send to main thread (adds latency)
    self.postMessage({ type: "osc", bundle: oscData.buffer }, [oscData.buffer]);
  }
}

// ===== MESSAGE HANDLER =====
self.onmessage = (e) => {
  const { type, ...data } = e.data;

  switch (type) {
    case "initChannel":
      // Receive OscChannel from main thread for direct worklet communication
      // OscChannel abstracts SAB vs postMessage - works transparently in both modes
      if (data.channel) {
        oscChannel = OscChannel.fromTransferable(data.channel);
      }
      // Receive config (groups, buses, patterns) from main thread
      if (data.config) {
        config = data.config;
      }
      self.postMessage({ type: "channelReady" });
      console.log(`[Scheduler Worker] Direct worklet channel established (${oscChannel?.mode ?? 'unknown'} mode)`);
      break;

    case "state":
      // Update state values
      Object.assign(state, data);
      // Reset arp direction on mode change
      if (data.arpMode !== undefined) {
        arpDirection = 1;
      }
      break;

    case "start":
      if (data.scheduler === "arp") arpScheduler.start();
      else if (data.scheduler === "kick") kickScheduler.start();
      else if (data.scheduler === "amen") amenScheduler.start();
      else if (data.scheduler === "all") {
        arpScheduler.start();
        kickScheduler.start();
        amenScheduler.start();
      }
      // Notify main thread of playback timing for visual sync (only for arp)
      // Offset by LOOKAHEAD so beat pulse syncs with when audio actually plays
      if (data.scheduler === "arp" || data.scheduler === "all") {
        self.postMessage({ type: "started", scheduler: "arp", playbackStartNTP: playbackStartNTP + LOOKAHEAD });
      }
      break;

    case "stop":
      if (data.scheduler === "arp") arpScheduler.stop();
      else if (data.scheduler === "kick") kickScheduler.stop();
      else if (data.scheduler === "amen") amenScheduler.stop(true);
      else if (data.scheduler === "all") {
        arpScheduler.stop();
        kickScheduler.stop();
        amenScheduler.stop(true);
      }
      break;

    case "reset":
      // Stop all and reset state
      arpScheduler.stop();
      kickScheduler.stop();
      amenScheduler.stop();
      playbackStartNTP = null;
      patternIndex = 0;
      arpDirection = 1;
      break;
  }
};

// Signal ready
self.postMessage({ type: "ready" });
