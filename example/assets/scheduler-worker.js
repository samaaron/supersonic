// Scheduler Worker - runs timing loops isolated from main thread jank
// Sends OSC directly to AudioWorklet via OscChannel, bypassing main thread

import { OscChannel, oscFast } from "../dist/supersonic.js";

const LOOKAHEAD = 0.15;

// Direct channel to AudioWorklet (works in both SAB and PM modes)
let oscChannel = null;

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
const getNTP = () =>
  (performance.timeOrigin + performance.now()) / 1000 + oscFast.NTP_EPOCH_OFFSET;

// ===== TIMELINE (Ableton Link style) =====
// Single source of truth for beat → time conversion
// All schedulers reference this for timing, ensuring they stay in phase
// When BPM changes, anchor adjusts to preserve current beat position
const timeline = {
  anchor: null,      // NTP time of beat 0 (adjusted when BPM changes)
  bpm: 120,

  // Get NTP time for a given beat number (beats are 8th notes)
  getTimeAtBeat(beat) {
    if (this.anchor === null) return null;
    const interval = 0.125 / (this.bpm / 120);
    return this.anchor + beat * interval;
  },

  // Get current beat at a given NTP time
  getBeatAtTime(ntp) {
    if (this.anchor === null) return 0;
    const interval = 0.125 / (this.bpm / 120);
    return (ntp - this.anchor) / interval;
  },

  // Start the timeline at a given NTP time
  start(startNTP, bpm) {
    this.anchor = startNTP;
    this.bpm = bpm;
  },

  // Update BPM while preserving current beat position (Ableton Link style)
  setBpm(newBpm) {
    if (this.anchor === null || newBpm === this.bpm) return;

    const now = getNTP();
    const currentBeat = this.getBeatAtTime(now);

    // Update BPM and recalculate anchor so currentBeat stays at 'now'
    // now = newAnchor + currentBeat * newInterval
    // newAnchor = now - currentBeat * newInterval
    const newInterval = 0.125 / (newBpm / 120);
    this.anchor = now - currentBeat * newInterval;
    this.bpm = newBpm;
  },

  reset() {
    this.anchor = null;
    this.bpm = 120;
  }
};

// ===== OSC HELPERS =====
// Create a single-message bundle using osc_fast (zero-allocation)
function createOSCBundle(ntpTime, address, args) {
  return oscFast.encodeSingleBundle(ntpTime, address, args);
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
// Schedulers use the shared timeline for all timing, ensuring phase alignment
class Scheduler {
  constructor(name, { getBeatsPerEvent, getBatchSize = () => 4, createMessage, getGroup }) {
    this.name = name;
    this.getBeatsPerEvent = getBeatsPerEvent;  // How many 8th-note beats per event
    this.getBatchSize = getBatchSize;
    this.createMessage = createMessage;
    this.getGroup = getGroup;
    this.running = false;
    this.nextBeat = 0;  // Next beat to schedule (in 8th notes)
    this.timeoutId = null;
  }

  start() {
    console.log(`[${this.name}] start() called, running=${this.running}`);
    if (this.running) {
      console.log(`[${this.name}] already running, returning early`);
      return;
    }
    this.running = true;

    const now = getNTP();

    if (timeline.anchor === null) {
      // First scheduler to start - initialize timeline
      const STARTUP_DELAY = 0.1;
      timeline.start(now + STARTUP_DELAY, state.bpm);
      this.nextBeat = 0;
    } else {
      // Join existing playback - align to next bar (16 8th notes)
      const currentBeat = timeline.getBeatAtTime(now);
      const beatsPerBar = 16;
      this.nextBeat = Math.ceil(currentBeat / beatsPerBar) * beatsPerBar;
    }

    const nextTime = timeline.getTimeAtBeat(this.nextBeat);
    const delay = Math.max(0, (nextTime - now) * 1000);
    this.timeoutId = setTimeout(() => this.running && this.scheduleBatch(), delay);
  }

  scheduleBatch() {
    if (!this.running) return;

    const beatsPerEvent = this.getBeatsPerEvent();
    const batchSize = this.getBatchSize();
    const now = getNTP();

    // Debug: log first 8 events to verify timestamps
    const shouldLog = Math.floor(this.nextBeat / beatsPerEvent) < 8 && this.name === "Arp";

    for (let i = 0; i < batchSize; i++) {
      const eventBeat = this.nextBeat + i * beatsPerEvent;
      const targetNTP = timeline.getTimeAtBeat(eventBeat) + LOOKAHEAD;

      const eventIndex = Math.floor(eventBeat / beatsPerEvent);
      const message = this.createMessage(eventIndex);
      if (message) {
        if (shouldLog) {
          const playsIn = (targetNTP - now) * 1000;
          console.log(`[${this.name} beat=${eventBeat}] target=${targetNTP.toFixed(3)}, playsIn=${playsIn.toFixed(0)}ms`);
        }
        const bundle = createOSCBundle(targetNTP, message.address, message.args);
        sendOSC(bundle);
      }
    }

    this.nextBeat += batchSize * beatsPerEvent;

    // Schedule next batch
    const nowAfter = getNTP();
    const nextBatchTime = timeline.getTimeAtBeat(this.nextBeat);
    const nextDelay = Math.max(50, (nextBatchTime - nowAfter) * 1000 - 500);
    this.timeoutId = setTimeout(() => this.running && this.scheduleBatch(), nextDelay);
  }

  stop(freeGroup = false) {
    this.running = false;
    this.nextBeat = 0;
    if (this.timeoutId) {
      clearTimeout(this.timeoutId);
      this.timeoutId = null;
    }
    if (freeGroup) {
      const encoded = oscFast.encodeMessage("/g_freeAll", [this.getGroup()]);
      sendOSC(encoded);
    }
    // Reset timeline if all schedulers stopped
    if (!arpScheduler.running && !kickScheduler.running && !amenScheduler.running) {
      timeline.reset();
    }
  }
}


// ===== SCHEDULERS =====
// All schedulers use the shared timeline for timing
// getBeatsPerEvent returns how many 8th-note beats between events
const arpScheduler = new Scheduler("Arp", {
  getBeatsPerEvent: () => 1,  // One 8th note per arp event
  createMessage: () => {
    if (!config) return null;
    if (state.synth === "none") return null;
    const scale = getMinorPentatonicScale(state.rootNote, state.octaves);
    const note = getArpNote(scale);

    return {
      address: "/s_new",
      args: [
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
      ],
    };
  },
  getGroup: () => config?.groups.GROUP_ARP ?? 100,
});

const kickScheduler = new Scheduler("Kick", {
  getBeatsPerEvent: () => 1,  // One 8th note per kick step
  createMessage: (counter) => {
    if (!config) return null;
    if (state.kickSample === "none") return null;
    const pattern = KICK_PATTERNS[state.kickPattern] || KICK_PATTERNS.four;
    const step = counter % 16;
    const shouldPlay = pattern[step];

    if (!shouldPlay) return null;

    const kickCfg = config.kickConfig?.[state.kickSample] || { buffer: 0 };
    return {
      address: "/s_new",
      args: [
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
      ],
    };
  },
  getGroup: () => config?.groups.GROUP_KICK ?? 103,
});

const amenScheduler = new Scheduler("Amen", {
  getBeatsPerEvent: () => {
    // Convert loop duration (in seconds at 120 BPM) to 8th-note beats
    // e.g., 2.0 seconds at 120 BPM = 16 8th notes
    if (!config) return 16;
    const loopConfig = config.loopConfig;
    const duration = loopConfig[state.loopSample]?.duration ?? 2;
    return duration / 0.125;  // Convert seconds to 8th notes (at 120 BPM base)
  },
  getBatchSize: () => 1,
  createMessage: () => {
    if (!config) return null;
    if (state.loopSample === "none") return null;
    const loopConfig = config.loopConfig;
    const cfg = loopConfig[state.loopSample] || { rate: 1, buffer: 1 };
    // Rate scales with BPM to keep loop in sync
    const bpmScale = timeline.bpm / 120;
    return {
      address: "/s_new",
      args: [
        "sonic-pi-basic_stereo_player",
        -1,
        0,
        config.groups.GROUP_LOOPS,
        "buf",
        cfg.buffer,
        "amp",
        0.5,
        "rate",
        cfg.rate * bpmScale,
        "out_bus",
        config.buses.FX_BUS_BEAT,
      ],
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
      // Update state values, but handle BPM changes specially
      if (data.bpm !== undefined && data.bpm !== state.bpm) {
        state.bpm = data.bpm;
        // Update timeline - this preserves current beat position (Ableton Link style)
        timeline.setBpm(data.bpm);
      }
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
        self.postMessage({ type: "started", scheduler: "arp", playbackStartNTP: timeline.anchor + LOOKAHEAD });
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
      timeline.reset();
      patternIndex = 0;
      arpDirection = 1;
      break;
  }
};

// Signal ready
self.postMessage({ type: "ready" });
