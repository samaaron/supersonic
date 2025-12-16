import { SuperSonic } from "../dist/supersonic.js";
import { NodeTreeViz } from "./node_tree_viz.js";

// ===== CONFIGURATION =====
const DEV_MODE = false;
const NTP_EPOCH_OFFSET = 2208988800; // Seconds between Unix epoch (1970) and NTP epoch (1900)
const LOOKAHEAD = 0.1; // Seconds to schedule ahead for timing accuracy

// OSC bundle header: ASCII "#bundle\0"
const OSC_BUNDLE_HEADER = new Uint8Array([
  0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00,
]);

// Scsynth node groups (keep synths organized in the node tree)
const GROUP_ARP = 100;
const GROUP_FX = 101;
const GROUP_LOOPS = 102;
const GROUP_KICK = 103;

// Audio buses for FX chain routing
const FX_BUS_SYNTH_TO_LPF = 20; // Synths output here
const FX_BUS_LPF_TO_REVERB = 22; // LPF outputs to reverb
const FX_BUS_OUTPUT = 0; // Final output (main out)

// FX node IDs
const FX_LPF_NODE = 2000;
const FX_REVERB_NODE = 2001;

// Available synthdefs (Sonic Pi synths)
const FX_SYNTHDEFS = [
  "sonic-pi-fx_lpf",
  "sonic-pi-fx_reverb",
  "sonic-pi-basic_stereo_player",
];
const INSTRUMENT_SYNTHDEFS = [
  "sonic-pi-bass_foundation",
  "sonic-pi-beep",
  "sonic-pi-blade",
  "sonic-pi-bnoise",
  "sonic-pi-chipbass",
  "sonic-pi-chiplead",
  "sonic-pi-dark_ambience",
  "sonic-pi-dpulse",
  "sonic-pi-dsaw",
  "sonic-pi-dtri",
  "sonic-pi-fm",
  "sonic-pi-gabberkick",
  "sonic-pi-hollow",
  "sonic-pi-mod_dsaw",
  "sonic-pi-mod_fm",
  "sonic-pi-mod_pulse",
  "sonic-pi-mod_saw",
  "sonic-pi-mod_sine",
  "sonic-pi-mod_tri",
  "sonic-pi-noise",
  "sonic-pi-organ_tonewheel",
  "sonic-pi-pluck",
  "sonic-pi-pretty_bell",
  "sonic-pi-prophet",
  "sonic-pi-pulse",
  "sonic-pi-rhodey",
  "sonic-pi-rodeo",
  "sonic-pi-saw",
  "sonic-pi-square",
  "sonic-pi-subpulse",
  "sonic-pi-supersaw",
  "sonic-pi-tb303",
  "sonic-pi-tech_saws",
  "sonic-pi-tri",
  "sonic-pi-winwood_lead",
  "sonic-pi-zawa",
];

// Loop sample configuration
const LOOP_CONFIG = {
  loop_amen: { rate: 0.8767, duration: 2.0, buffer: 1 },
  loop_breakbeat: { rate: 0.9524, duration: 2.0, buffer: 2 },
  loop_compus: { rate: 0.8108, duration: 8.0, buffer: 3 },
  loop_garzul: { rate: 1.0, duration: 8.0, buffer: 4 },
  loop_industrial: { rate: 0.8837, duration: 1.0, buffer: 5 },
  loop_tabla: { rate: 1.3342, duration: 8.0, buffer: 6 },
};

// ===== STATE =====
let orchestrator = null;
let nodeTreeViz = null;
let messages = [],
  sentMessages = [];
let analyser = null,
  scopeAnimationId = null;
let trailParticles = [],
  trailAnimationId = null,
  trailReleaseTime = null;
let fxChainInitialized = false,
  fxChainInitializing = null;
let synthdefsLoaded = { fx: false, instruments: false };
let samplesLoaded = { kick: false, loops: false };
let playbackStartNTP = null;
let beatPulseInterval = null;

const uiState = {
  padX: 0.5,
  padY: 0.5,
  padActive: false,
  synth: "beep",
  loopSample: "loop_amen",
  rootNote: 60,
  octaves: 2,
  amplitude: 0.5,
  attack: 0,
  release: 0.1,
  randomArp: true,
  bpm: 120,
};
window.uiState = uiState;

// ===== HELPERS =====
const $ = (id) => document.getElementById(id);
const $$ = (sel) => document.querySelectorAll(sel);
const getBpmScale = () => uiState.bpm / 120;
const getNTP = () =>
  (performance.timeOrigin + performance.now()) / 1000 + NTP_EPOCH_OFFSET;

// Overlay visibility helpers
const showOverlay = (id) => {
  const el = $(id);
  if (el) el.style.display = "flex";
};
const hideOverlay = (id) => {
  const el = $(id);
  if (el) el.style.display = "none";
};

// Synth pad state helper
function setSynthPadState(state) {
  const pad = $("synth-pad");
  if (!pad) return;
  pad.classList.remove("ready", "disabled");
  pad.classList.add(state);
}

// OSC typed args builder - reduces verbosity in scheduler messages
function oscArgs(...args) {
  return args.map((v) => {
    if (typeof v === "string") return { type: "s", value: v };
    if (Number.isInteger(v)) return { type: "i", value: v };
    return { type: "f", value: v };
  });
}

// Generic tab system handler with ARIA support
function setupTabSystem(buttonSelector, contentSelector, tabAttr, contentAttr) {
  $$(buttonSelector).forEach((btn) => {
    btn.addEventListener("click", () => {
      // Update buttons
      $$(buttonSelector).forEach((b) => {
        b.classList.remove("active");
        b.setAttribute("aria-selected", "false");
      });
      btn.classList.add("active");
      btn.setAttribute("aria-selected", "true");

      // Update content panels
      $$(contentSelector).forEach((c) => {
        c.classList.remove("active");
        c.setAttribute("hidden", "");
      });
      const panel = document.querySelector(
        `[${contentAttr}="${btn.dataset[tabAttr]}"]`,
      );
      if (panel) {
        panel.classList.add("active");
        panel.removeAttribute("hidden");
      }
    });
  });
}

function formatBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes <= 0) return "0 B";
  const units = ["B", "KB", "MB", "GB"];
  let i = 0;
  while (bytes >= 1024 && i < units.length - 1) {
    bytes /= 1024;
    i++;
  }
  return `${bytes.toFixed(i > 0 ? 1 : 0)} ${units[i]}`;
}

// ===== OSC HELPERS =====
function parseTextToOSC(text) {
  const parts = [];
  let current = "",
    inQuotes = false;
  for (const ch of text) {
    if (ch === '"') inQuotes = !inQuotes;
    else if (ch === " " && !inQuotes) {
      if (current) {
        parts.push(current);
        current = "";
      }
    } else current += ch;
  }
  if (current) parts.push(current);
  if (!parts.length) throw new Error("Empty message");

  const address = parts[0];
  if (!address.startsWith("/"))
    throw new Error("OSC address must start with /");

  const args = parts.slice(1).map((arg) => {
    if (
      address === "/d_recv" &&
      /^[0-9a-fA-F]+$/.test(arg) &&
      arg.length > 10
    ) {
      const bytes = new Uint8Array(arg.length / 2);
      for (let i = 0; i < arg.length; i += 2)
        bytes[i / 2] = parseInt(arg.substr(i, 2), 16);
      return { type: "b", value: bytes };
    }
    if (/^-?\d+$/.test(arg)) return { type: "i", value: parseInt(arg, 10) };
    if (/^-?\d*\.\d+$/.test(arg)) return { type: "f", value: parseFloat(arg) };
    return { type: "s", value: arg };
  });
  return { address, args };
}

function createOSCBundle(ntpTime, messages) {
  const encoded = messages.map((m) => SuperSonic.osc.encode(m));
  const size = 16 + encoded.reduce((sum, m) => sum + 4 + m.byteLength, 0);
  const bundle = new Uint8Array(size);
  const view = new DataView(bundle.buffer);

  // Write header and NTP timestamp (8 bytes header + 8 bytes timestamp)
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

function colorizeOSCArgs(oscMsg) {
  const { address, args } = oscMsg;
  if (!Array.isArray(args)) return args || "";

  const isSnew = address === "/s_new";
  return args
    .map((arg, i) => {
      let value = arg,
        type = null;
      if (typeof arg === "object" && arg.value !== undefined) {
        value = arg.value;
        type = arg.type;
      }

      if (type === "b" || value instanceof Uint8Array) {
        return `<span class="osc-color-string">&lt;binary ${value.length || "?"} bytes&gt;</span>`;
      }
      const isFloat =
        type === "f" ||
        (type === null &&
          typeof value === "number" &&
          !Number.isInteger(value));
      const isInt = type === "i" || (type === null && Number.isInteger(value));
      const isParam =
        isSnew && i >= 4 && (i - 4) % 2 === 0 && typeof value === "string";

      if (isFloat) return `<span class="osc-color-float">${value}</span>`;
      if (isInt) return `<span class="osc-color-int">${value}</span>`;
      if (isParam) return `<span class="osc-color-param">${value}</span>`;
      return `<span class="osc-color-string">${value}</span>`;
    })
    .join(" ");
}

function parseOscTextInput(rawText) {
  const scheduled = new Map(),
    immediate = [],
    comments = [],
    errors = [];
  let currentTs = null;

  for (const line of rawText
    .split(/\r?\n/)
    .map((l) => l.trim())
    .filter(Boolean)) {
    if (line.startsWith("#")) {
      comments.push(line);
      continue;
    }

    if (line.startsWith("-")) {
      if (currentTs === null) {
        errors.push("Bundle continuation before timestamp");
        continue;
      }
      const cmd = line.slice(1).trim();
      if (!cmd.startsWith("/")) {
        errors.push("Invalid bundle continuation");
        continue;
      }
      try {
        scheduled.get(currentTs).push(parseTextToOSC(cmd));
      } catch (e) {
        errors.push(e.message);
      }
      continue;
    }

    const match = line.match(/^(-?\d+(?:\.\d+)?)\s+(\/.*)$/);
    if (match) {
      const ts = parseFloat(match[1]);
      if (isNaN(ts)) {
        errors.push(`Invalid timestamp: ${match[1]}`);
        continue;
      }
      try {
        if (!scheduled.has(ts)) scheduled.set(ts, []);
        scheduled.get(ts).push(parseTextToOSC(match[2]));
        currentTs = ts;
      } catch (e) {
        errors.push(e.message);
      }
      continue;
    }

    if (line.startsWith("/")) {
      try {
        immediate.push(parseTextToOSC(line));
      } catch (e) {
        errors.push(e.message);
      }
      currentTs = null;
      continue;
    }
    errors.push(`Unrecognised: ${line}`);
  }
  return { scheduled, immediate, comments, errors };
}

// ===== UI HELPERS =====
function flashTab(tabName) {
  const btn = document.querySelector(`[data-tab="${tabName}"]`);
  if (!btn || btn.classList.contains("active")) return;
  btn.classList.remove("flash");
  void btn.offsetWidth;
  btn.classList.add("flash");
  setTimeout(() => btn.classList.remove("flash"), 1500);
}

function showError(msg) {
  $("error-text").textContent = msg;
  $("error-message").classList.remove("hidden");
}

function hideError() {
  $("error-message").classList.add("hidden");
}

function updateStatus(status) {
  const initBtn = $("init-button"),
    initContainer = $("init-button-container");
  const scopeContainer = $("scope-container"),
    uiContainer = $("synth-ui-container");
  const restartContainer = $("restart-container");

  if (status === "not_initialized" || status === "error") {
    stopBootAnimation();
    initBtn.textContent = "Boot";
    initBtn.disabled = false;
    if (initContainer) initContainer.style.display = "block";
    if (scopeContainer) scopeContainer.style.display = "none";
    if (uiContainer) uiContainer.classList.remove("initialized");
    $("message-input").disabled = true;
    $$("#message-form button").forEach((b) => (b.disabled = true));
  } else if (status === "initializing") {
    startBootAnimation();
    initBtn.disabled = true;
  } else if (status === "loading_assets") {
    if (uiContainer) uiContainer.classList.remove("initialized");
  } else if (status === "ready") {
    stopBootAnimation();
    if (initContainer) initContainer.style.display = "none";
    if (scopeContainer) scopeContainer.style.display = "block";
    if (uiContainer) uiContainer.classList.add("initialized");
    if (restartContainer) restartContainer.style.display = "block";
    $("message-input").disabled = false;
    $$("#message-form button").forEach((b) => (b.disabled = false));
  }
}

// Boot animation
let bootAnimationInterval = null,
  bootDotCount = 0;
function startBootAnimation() {
  if (bootAnimationInterval) return;
  const btn = $("init-button");
  btn.style.minWidth = btn.offsetWidth + "px";
  btn.textContent = "Booting.  ";
  bootAnimationInterval = setInterval(() => {
    bootDotCount = (bootDotCount + 1) % 4;
    const dots =
      ".".repeat(bootDotCount || 1) + "\u2008".repeat(3 - (bootDotCount || 1));
    btn.textContent = `Booting${dots}`;
  }, 400);
}

function stopBootAnimation() {
  if (bootAnimationInterval) {
    clearInterval(bootAnimationInterval);
    bootAnimationInterval = null;
  }
}

// Loading log
const loadingLog = $("loading-log");
const loadingLogContent = $("loading-log-content");
const spinnerFrames = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"];
let spinnerInterval = null,
  spinnerIndex = 0;

function startLoadingSpinner(msg) {
  const title = loadingLog?.querySelector(".loading-log-title");
  if (title) title.textContent = msg;
  spinnerIndex = 0;
  const spinner = $("loading-spinner");
  if (spinnerInterval) clearInterval(spinnerInterval);
  spinnerInterval = setInterval(() => {
    if (spinner)
      spinner.textContent =
        spinnerFrames[spinnerIndex++ % spinnerFrames.length];
  }, 80);
}

function stopLoadingSpinner() {
  if (spinnerInterval) {
    clearInterval(spinnerInterval);
    spinnerInterval = null;
  }
  const spinner = $("loading-spinner");
  if (spinner) spinner.textContent = "";
}

function addLoadingLogEntry(msg, type = "") {
  if (!loadingLogContent) return;
  const entry = document.createElement("div");
  entry.className = `log-entry ${type}`;
  entry.textContent = msg;
  loadingLogContent.appendChild(entry);
  loadingLogContent.scrollTop = loadingLogContent.scrollHeight;
}

function positionLoadingLog() {
  if (!loadingLog) return;
  const pad = document.querySelector(".synth-pad-container");
  const panel = document.querySelector(".dark-panel");
  if (pad && panel) {
    const pr = pad.getBoundingClientRect(),
      panelR = panel.getBoundingClientRect();
    Object.assign(loadingLog.style, {
      top: `${pr.top - panelR.top}px`,
      left: `${pr.left - panelR.left}px`,
      width: `${pr.width}px`,
      height: `${pr.height}px`,
    });
  }
}

function showLoadingLog() {
  if (loadingLog) {
    positionLoadingLog();
    loadingLog.classList.add("visible");
  }
  window.addEventListener("resize", positionLoadingLog);
}

function hideLoadingLog() {
  if (loadingLog) loadingLog.classList.remove("visible");
  window.removeEventListener("resize", positionLoadingLog);
}

function clearLoadingLog() {
  if (loadingLogContent) loadingLogContent.innerHTML = "";
}

// Composite loading helpers
function beginLoadingSequence(spinnerMsg) {
  clearLoadingLog();
  showLoadingLog();
  startLoadingSpinner(spinnerMsg);
}

function endLoadingSequence(success = true) {
  stopLoadingSpinner();
  addLoadingLogEntry(
    success ? "Ready" : "Failed",
    success ? "complete" : "error",
  );
  hideLoadingLog();
}

// ===== MESSAGE RENDERING =====
function renderOSCMessage(oscData, showSequence = null) {
  try {
    const msg = SuperSonic.osc.decode(oscData);
    if (msg.packets) {
      const content = msg.packets
        .map(
          (p) =>
            `<span class="osc-color-address">${p.address}</span> ${colorizeOSCArgs(p)}`,
        )
        .join("<br>");
      return `<span class="osc-color-string">Bundle (${msg.packets.length})</span><br>${content}`;
    }
    return `<span class="osc-color-address">${msg.address}</span> ${colorizeOSCArgs(msg)}`;
  } catch (e) {
    return `<span class="osc-color-error">Decode error: ${e.message}</span>`;
  }
}

function addMessage(msg) {
  messages.push(msg);
  if (messages.length > 50) messages = messages.slice(-50);

  const history = $("message-history");
  if (!messages.length) {
    history.innerHTML = '<p class="message-empty">No messages received yet</p>';
  } else {
    history.innerHTML = messages
      .map(
        (m) => `
      <div class="message-item">
        <span class="message-header">[${m.sequence}]</span>
        <span class="message-content">${m.oscData ? renderOSCMessage(m.oscData) : m.text || "Unknown"}</span>
      </div>
    `,
      )
      .join("");
  }
  history.scrollTop = history.scrollHeight;
  flashTab("osc-in");
}

function addSentMessage(oscData, comment = null) {
  const history = $("sent-message-history");
  if (!history) return;

  const msg = { oscData, timestamp: Date.now(), comment };
  sentMessages.push(msg);
  if (sentMessages.length > 50) sentMessages.shift();

  const time = new Date(msg.timestamp).toISOString().slice(11, 23);
  const content = comment
    ? `<span class="osc-color-comment">${comment}</span>`
    : renderOSCMessage(oscData);

  const empty = history.querySelector(".message-empty");
  if (empty) empty.remove();

  history.insertAdjacentHTML(
    "beforeend",
    `
    <div class="message-item">
      <span class="message-header">[${time}]</span>
      <span class="message-content">${content}</span>
    </div>
  `,
  );
  history.scrollTop = history.scrollHeight;
  flashTab("osc-out");
}

// ===== METRICS =====
const METRICS_MAP = {
  workletProcessCount: "worklet_process_count",
  workletMessagesProcessed: "worklet_messages_processed",
  workletMessagesDropped: "worklet_messages_dropped",
  workletSchedulerDepth: "worklet_scheduler_depth",
  workletSchedulerMax: "worklet_scheduler_max",
  workletSchedulerDropped: "worklet_scheduler_dropped",
  workletSequenceGaps: "worklet_sequence_gaps",
  preschedulerPending: "prescheduler_pending",
  preschedulerPeak: "prescheduler_peak",
  preschedulerSent: "prescheduler_sent",
  preschedulerRetriesSucceeded: "prescheduler_retries_succeeded",
  preschedulerRetriesFailed: "prescheduler_retries_failed",
  preschedulerRetryQueueSize: "prescheduler_retry_queue_size",
  preschedulerRetryQueueMax: "prescheduler_retry_queue_max",
  preschedulerBundlesScheduled: "prescheduler_bundles_scheduled",
  preschedulerEventsCancelled: "prescheduler_events_cancelled",
  preschedulerTotalDispatches: "prescheduler_total_dispatches",
  preschedulerMessagesRetried: "prescheduler_messages_retried",
  preschedulerBypassed: "prescheduler_bypassed",
  oscInMessagesReceived: "osc_in_messages_received",
  oscInMessagesDropped: "osc_in_messages_dropped",
  oscInBytesReceived: "osc_in_bytes_received",
  debugMessagesReceived: "debug_messages_received",
  debugBytesReceived: "debug_bytes_received",
  mainMessagesSent: "main_messages_sent",
  mainBytesSent: "main_bytes_sent",
  driftOffsetMs: "drift_offset_ms",
};

function updateMetrics(m) {
  const mapped = {};
  for (const [src, dst] of Object.entries(METRICS_MAP)) {
    if (m[src] !== undefined) mapped[dst] = m[src];
  }

  // Buffer usage
  if (m.inBufferUsed?.percentage !== undefined)
    mapped.in_buffer_usage = m.inBufferUsed.percentage;
  if (m.outBufferUsed?.percentage !== undefined)
    mapped.out_buffer_usage = m.outBufferUsed.percentage;
  if (m.debugBufferUsed?.percentage !== undefined)
    mapped.debug_buffer_usage = m.debugBufferUsed.percentage;

  // Engine state
  mapped.buffer_pool_used = m.bufferPoolUsedBytes;
  mapped.buffer_pool_available = m.bufferPoolAvailableBytes;
  mapped.buffer_pool_allocations = m.bufferPoolAllocations;
  mapped.synthdef_count = m.loadedSynthDefs;
  mapped.audio_context_state = m.audioContextState;

  // Update DOM
  const updates = {
    "metric-sent": mapped.main_messages_sent ?? 0,
    "metric-bytes-sent": formatBytes(mapped.main_bytes_sent ?? 0),
    "metric-direct-writes": mapped.prescheduler_bypassed ?? 0,
    "metric-received": mapped.osc_in_messages_received ?? 0,
    "metric-bytes-received": formatBytes(mapped.osc_in_bytes_received ?? 0),
    "metric-osc-in-dropped": mapped.osc_in_messages_dropped ?? 0,
    "metric-messages-processed": mapped.worklet_messages_processed ?? 0,
    "metric-messages-dropped": mapped.worklet_messages_dropped ?? 0,
    "metric-sequence-gaps": mapped.worklet_sequence_gaps ?? 0,
    "metric-process-count": mapped.worklet_process_count ?? 0,
    "metric-scheduler-depth": mapped.worklet_scheduler_depth ?? 0,
    "metric-scheduler-peak": mapped.worklet_scheduler_max ?? 0,
    "metric-scheduler-dropped": mapped.worklet_scheduler_dropped ?? 0,
    "metric-drift": (mapped.drift_offset_ms ?? 0) + "ms",
    "metric-prescheduler-pending": mapped.prescheduler_pending ?? 0,
    "metric-prescheduler-peak": mapped.prescheduler_peak ?? 0,
    "metric-prescheduler-sent": mapped.prescheduler_sent ?? 0,
    "metric-bundles-scheduled": mapped.prescheduler_bundles_scheduled ?? 0,
    "metric-events-cancelled": mapped.prescheduler_events_cancelled ?? 0,
    "metric-prescheduler-retries-succeeded":
      mapped.prescheduler_retries_succeeded ?? 0,
    "metric-prescheduler-retries-failed":
      mapped.prescheduler_retries_failed ?? 0,
    "metric-prescheduler-retry-queue-size":
      mapped.prescheduler_retry_queue_size ?? 0,
    "metric-prescheduler-retry-queue-max":
      mapped.prescheduler_retry_queue_max ?? 0,
    "metric-messages-retried": mapped.prescheduler_messages_retried ?? 0,
    "metric-total-dispatches": mapped.prescheduler_total_dispatches ?? 0,
    "metric-debug-received": mapped.debug_messages_received ?? 0,
    "metric-debug-bytes-received": formatBytes(
      mapped.debug_bytes_received ?? 0,
    ),
    "metric-audio-state": mapped.audio_context_state ?? "-",
    "metric-synthdefs": mapped.synthdef_count ?? 0,
    "metric-buffer-used": formatBytes(mapped.buffer_pool_used ?? 0),
    "metric-buffer-free": formatBytes(mapped.buffer_pool_available ?? 0),
    "metric-buffer-allocs": mapped.buffer_pool_allocations ?? 0,
  };

  for (const [id, val] of Object.entries(updates)) {
    const el = $(id);
    if (el) el.textContent = val;
  }

  // Buffer bars
  for (const [name, color] of [
    ["in", "#1e90ff"],
    ["out", "#4a4"],
    ["debug", "#a4a"],
  ]) {
    const usage = mapped[`${name}_buffer_usage`] ?? 0;
    const bar = $(`metric-${name}-bar`),
      label = $(`metric-${name}-usage`);
    if (bar) bar.style.width = usage + "%";
    if (label) label.textContent = usage.toFixed(2) + "%";
  }
}

// ===== SCOPE VISUALISER =====
const scopeCanvas = $("scope-canvas");
const scopeCtx = scopeCanvas?.getContext("2d");

function setupScope() {
  if (!orchestrator?.audioContext || !orchestrator.workletNode) return;

  analyser = orchestrator.audioContext.createAnalyser();
  analyser.fftSize = 2048;
  analyser.smoothingTimeConstant = 0.8;

  orchestrator.workletNode.disconnect();
  orchestrator.workletNode.connect(analyser);
  analyser.connect(orchestrator.audioContext.destination);

  const dpr = window.devicePixelRatio || 1;
  scopeCanvas.width = scopeCanvas.offsetWidth * dpr;
  scopeCanvas.height = scopeCanvas.offsetHeight * dpr;
  scopeCtx.scale(dpr, dpr);
  drawScope();
}

function drawScope() {
  scopeAnimationId = requestAnimationFrame(drawScope);
  if (!analyser) return;

  const data = new Uint8Array(analyser.fftSize);
  analyser.getByteTimeDomainData(data);

  const w = scopeCanvas.offsetWidth,
    h = scopeCanvas.offsetHeight;
  scopeCtx.fillStyle = "#000";
  scopeCtx.fillRect(0, 0, w, h);
  scopeCtx.lineWidth = 3;
  scopeCtx.strokeStyle = "#ff6600";
  scopeCtx.beginPath();

  const slice = w / data.length;
  for (let i = 0; i < data.length; i++) {
    const y = ((data[i] / 128.0) * h) / 2;
    i === 0 ? scopeCtx.moveTo(0, y) : scopeCtx.lineTo(i * slice, y);
  }
  scopeCtx.lineTo(w, h / 2);
  scopeCtx.stroke();
}

// ===== TRAIL EFFECT =====
const trailCanvas = $("trail-canvas");
const trailCtx = trailCanvas?.getContext("2d");

if (trailCanvas) {
  const pad = $("synth-pad");
  if (pad) {
    const resize = () => {
      trailCanvas.width = pad.offsetWidth;
      trailCanvas.height = pad.offsetHeight;
    };
    resize();
    window.addEventListener("resize", resize);
  }
}

function spawnTrailParticle() {
  if (!trailCanvas || !uiState.padActive) return;
  const x = uiState.padX * trailCanvas.width,
    y = (1 - uiState.padY) * trailCanvas.height;

  for (let i = 0; i < 3; i++) {
    const angle = Math.random() * Math.PI * 2,
      speed = 0.3 + Math.random() * 0.5;
    trailParticles.push({
      x: x + (Math.random() - 0.5) * 10,
      y: y + (Math.random() - 0.5) * 10,
      vx: Math.cos(angle) * speed * 0.2,
      vy: Math.sin(angle) * speed * 0.2,
      life: 1.0,
      maxLife: 0.6 + Math.random() * 0.4,
      size: 8 + Math.random() * 8,
    });
  }
}

function updateTrail() {
  if (!trailCtx) return;

  const inRelease = !uiState.padActive && trailReleaseTime !== null;
  const elapsed = inRelease ? (performance.now() - trailReleaseTime) / 1000 : 0;
  const done = elapsed > (uiState.release || 0.1);
  const shouldSpawn = uiState.padActive || (inRelease && !done);

  if (
    shouldSpawn &&
    Math.random() <
      (inRelease ? Math.max(0, 1 - elapsed / (uiState.release || 0.1)) : 1)
  ) {
    spawnTrailParticle();
  }

  if (!trailParticles.length && !shouldSpawn) {
    trailCtx.clearRect(0, 0, trailCanvas.width, trailCanvas.height);
    trailAnimationId = null;
    trailReleaseTime = null;
    return;
  }

  trailCtx.fillStyle = "rgba(0, 0, 0, 0.08)";
  trailCtx.fillRect(0, 0, trailCanvas.width, trailCanvas.height);
  trailCtx.globalCompositeOperation = "lighter";

  trailParticles = trailParticles.filter((p) => {
    p.life -= 16.67 / (p.maxLife * 1000);
    if (p.life <= 0) return false;

    p.x += p.vx;
    p.y += p.vy;
    p.vx *= 0.98;
    p.vy *= 0.98;

    const size = p.size * (0.5 + p.life * 0.5);
    const grad = trailCtx.createRadialGradient(p.x, p.y, 0, p.x, p.y, size);
    grad.addColorStop(0, `rgba(255, 215, 0, ${p.life * 0.9})`);
    grad.addColorStop(0.5, `rgba(255, 160, 0, ${p.life * 0.7})`);
    grad.addColorStop(1, "rgba(255, 102, 0, 0)");

    trailCtx.fillStyle = grad;
    trailCtx.beginPath();
    trailCtx.arc(p.x, p.y, size, 0, Math.PI * 2);
    trailCtx.fill();
    return true;
  });

  trailCtx.globalCompositeOperation = "source-over";
  trailAnimationId = requestAnimationFrame(updateTrail);
}

function startTrailAnimation() {
  trailReleaseTime = null;
  if (!trailAnimationId) trailAnimationId = requestAnimationFrame(updateTrail);
}

function stopTrailAnimation() {
  trailReleaseTime = performance.now();
}

// ===== LOOP SCHEDULER =====
class LoopScheduler {
  constructor(
    name,
    { getInterval, getBatchSize = () => 4, createMessage, group },
  ) {
    this.name = name;
    this.getInterval = getInterval;
    this.getBatchSize = getBatchSize;
    this.createMessage = createMessage;
    this.group = group;
    this.running = false;
    this.counter = 0;
    this.currentTime = null;
  }

  start() {
    if (this.running) return;
    this.running = true;

    const interval = this.getInterval();
    const gridInterval = 0.5 / getBpmScale();
    const now = getNTP();
    const nextGrid = Math.ceil(now / gridInterval) * gridInterval;

    if (playbackStartNTP === null) {
      playbackStartNTP = nextGrid;
      this.counter = 0;
      this.currentTime = playbackStartNTP;
    } else {
      const elapsed = now - playbackStartNTP;
      this.counter = Math.ceil(elapsed / interval);
      this.currentTime = playbackStartNTP + this.counter * interval;
    }

    const delay = Math.max(0, (this.currentTime - now) * 1000);
    setTimeout(() => this.running && this.scheduleBatch(), delay);
  }

  scheduleBatch() {
    if (!this.running || !orchestrator) return;

    const interval = this.getInterval();
    const batchSize = this.getBatchSize();

    for (let i = 0; i < batchSize; i++) {
      const targetNTP = this.currentTime + LOOKAHEAD;
      this.currentTime += interval;

      const message = this.createMessage(this.counter + i);
      const bundle = createOSCBundle(targetNTP, [message]);
      orchestrator.sendOSC(bundle);
    }

    this.counter += batchSize;

    const now = getNTP();
    const nextDelay = Math.max(0, (this.currentTime - now) * 1000 - 500);
    setTimeout(() => this.running && this.scheduleBatch(), nextDelay);
  }

  stop(freeGroup = false) {
    this.running = false;
    this.currentTime = null;
    if (freeGroup && orchestrator) orchestrator.send("/g_freeAll", this.group);
    if (
      !arpScheduler?.running &&
      !kickScheduler?.running &&
      !amenScheduler?.running
    ) {
      playbackStartNTP = null;
    }
  }
}

// ===== FX CHAIN =====
async function loadSynthdefs(type) {
  if (synthdefsLoaded[type]) return;

  const defs = type === "fx" ? FX_SYNTHDEFS : INSTRUMENT_SYNTHDEFS;
  await orchestrator.loadSynthDefs(defs);
  if (type === "instruments")
    await orchestrator.sync(Math.floor(Math.random() * 1000000));
  synthdefsLoaded[type] = true;
}

async function initFXChain() {
  if (!orchestrator || fxChainInitialized) return;
  if (fxChainInitializing) return await fxChainInitializing;

  fxChainInitializing = (async () => {
    await loadSynthdefs("fx");

    orchestrator.send("/g_new", GROUP_ARP, 0, 0);
    orchestrator.send("/g_new", GROUP_LOOPS, 0, 0);
    orchestrator.send("/g_new", GROUP_KICK, 0, 0);
    orchestrator.send("/g_new", GROUP_FX, 1, 0);
    orchestrator.send(
      "/s_new",
      "sonic-pi-fx_lpf",
      FX_LPF_NODE,
      0,
      GROUP_FX,
      "in_bus",
      FX_BUS_SYNTH_TO_LPF,
      "out_bus",
      FX_BUS_LPF_TO_REVERB,
      "cutoff",
      130.0,
      "res",
      0.5,
    );
    orchestrator.send(
      "/s_new",
      "sonic-pi-fx_reverb",
      FX_REVERB_NODE,
      3,
      FX_LPF_NODE,
      "in_bus",
      FX_BUS_LPF_TO_REVERB,
      "out_bus",
      FX_BUS_OUTPUT,
      "mix",
      0.3,
      "room",
      0.6,
    );

    await orchestrator.sync(Math.floor(Math.random() * 1000000));
    fxChainInitialized = true;
    fxChainInitializing = null;
  })();

  return await fxChainInitializing;
}

function updateFXParameters(x, y) {
  const mix = x,
    cutoff = 30 + y * 100;

  const cutoffEl = $("fx-cutoff-value"),
    mixEl = $("fx-mix-value");
  if (cutoffEl) cutoffEl.textContent = cutoff.toFixed(1);
  if (mixEl) mixEl.textContent = mix.toFixed(2);

  if (fxChainInitialized && orchestrator) {
    orchestrator.send("/n_set", FX_LPF_NODE, "cutoff", cutoff);
    orchestrator.send("/n_set", FX_REVERB_NODE, "mix", mix);
  }
}

// ===== SCHEDULERS =====
function getMinorPentatonicScale(root, octaves) {
  const intervals = [0, 3, 5, 7, 10];
  const scale = [];
  for (let o = 0; o < octaves; o++) {
    for (const i of intervals) scale.push(root + i + o * 12);
  }
  return scale;
}

let patternIndex = 0;

const arpScheduler = new LoopScheduler("Arp", {
  getInterval: () => 0.125 / getBpmScale(),
  createMessage: () => {
    const scale = getMinorPentatonicScale(uiState.rootNote, uiState.octaves);
    const note = uiState.randomArp
      ? scale[Math.floor(Math.random() * scale.length)]
      : scale[patternIndex++ % scale.length];

    return {
      address: "/s_new",
      args: oscArgs(
        `sonic-pi-${uiState.synth}`,
        -1,
        0,
        GROUP_ARP,
        "note",
        note,
        "out_bus",
        FX_BUS_SYNTH_TO_LPF,
        "amp",
        uiState.amplitude * 0.5,
        "attack",
        uiState.attack,
        "release",
        uiState.release,
      ),
    };
  },
  group: GROUP_ARP,
});

const kickScheduler = new LoopScheduler("Kick", {
  getInterval: () => 0.5 / getBpmScale(),
  createMessage: () => ({
    address: "/s_new",
    args: oscArgs(
      "sonic-pi-basic_stereo_player",
      -1,
      0,
      GROUP_KICK,
      "buf",
      0,
      "out_bus",
      FX_BUS_SYNTH_TO_LPF,
      "amp",
      0.3,
    ),
  }),
  group: GROUP_KICK,
});

const amenScheduler = new LoopScheduler("Amen", {
  getInterval: () =>
    (LOOP_CONFIG[uiState.loopSample]?.duration ?? 2) / getBpmScale(),
  getBatchSize: () => 1,
  createMessage: () => {
    const cfg = LOOP_CONFIG[uiState.loopSample] || { rate: 1, buffer: 1 };
    return {
      address: "/s_new",
      args: oscArgs(
        "sonic-pi-basic_stereo_player",
        -1,
        0,
        GROUP_LOOPS,
        "buf",
        cfg.buffer,
        "amp",
        0.5,
        "rate",
        cfg.rate * getBpmScale(),
        "out_bus",
        FX_BUS_SYNTH_TO_LPF,
      ),
    };
  },
  group: GROUP_LOOPS,
});

// Beat pulse
function startBeatPulse() {
  const interval = 0.125 / getBpmScale();
  const touch = $("synth-pad-touch");
  if (!touch) return;

  const timeSince = getNTP() - playbackStartNTP;
  const nextIn = interval - (timeSince % interval);

  setTimeout(() => {
    triggerBeatPulse();
    beatPulseInterval = setInterval(triggerBeatPulse, interval * 1000);
  }, nextIn * 1000);
}

function stopBeatPulse() {
  if (beatPulseInterval) {
    clearInterval(beatPulseInterval);
    beatPulseInterval = null;
  }
}

function triggerBeatPulse() {
  const touch = $("synth-pad-touch");
  if (!touch) return;
  touch.classList.remove("beat-pulse");
  void touch.offsetWidth;
  touch.classList.add("beat-pulse");
  spawnTrailParticle();
}

// Wrapper functions for global access
async function startArpeggiator() {
  if (!orchestrator) return;
  if (!synthdefsLoaded.instruments) await loadSynthdefs("instruments");
  if (!fxChainInitialized) await initFXChain();
  arpScheduler.start();
  startBeatPulse();
}

function stopArpeggiator() {
  arpScheduler.stop();
  stopBeatPulse();
}

async function startKickLoop() {
  if (!orchestrator) return;
  if (!fxChainInitialized) await initFXChain();
  if (!samplesLoaded.kick) {
    await orchestrator.send("/b_allocRead", 0, "bd_haus.flac");
    samplesLoaded.kick = true;
  }
  kickScheduler.start();
}

function stopKickLoop() {
  kickScheduler.stop();
}

async function loadAllLoopSamples() {
  if (samplesLoaded.loops) return;
  await Promise.all(
    Object.entries(LOOP_CONFIG).map(([name, cfg]) =>
      orchestrator.loadSample(cfg.buffer, `${name}.flac`),
    ),
  );
  samplesLoaded.loops = true;
}

async function startAmenLoop() {
  if (!orchestrator) return;
  if (!fxChainInitialized) await initFXChain();
  if (!samplesLoaded.loops) await loadAllLoopSamples();
  amenScheduler.start();
}

function stopAmenLoop() {
  amenScheduler.stop(true);
}

// Expose globally
window.startArpeggiator = startArpeggiator;
window.stopArpeggiator = stopArpeggiator;
window.startKickLoop = startKickLoop;
window.stopKickLoop = stopKickLoop;
window.startAmenLoop = startAmenLoop;
window.stopAmenLoop = stopAmenLoop;

// ===== SYNTH PAD =====
const synthPad = $("synth-pad");
if (synthPad) {
  synthPad.classList.add("disabled");
  let isPadActive = false;

  function updatePadPosition(clientX, clientY) {
    const rect = synthPad.getBoundingClientRect();
    const x = Math.max(0, Math.min(1, (clientX - rect.left) / rect.width));
    const y = Math.max(0, Math.min(1, 1 - (clientY - rect.top) / rect.height));

    uiState.padX = x;
    uiState.padY = y;
    uiState.padActive = isPadActive;

    const px = x * rect.width,
      py = (1 - y) * rect.height;
    $("synth-pad-touch").style.left = px + "px";
    $("synth-pad-touch").style.top = py + "px";
    $("synth-pad-crosshair-h").style.top = py + "px";
    $("synth-pad-crosshair-v").style.left = px + "px";

    const xVal = $("pad-x-value"),
      yVal = $("pad-y-value");
    if (xVal) xVal.textContent = x.toFixed(2);
    if (yVal) yVal.textContent = y.toFixed(2);

    updateFXParameters(x, y);
  }

  function activatePad(clientX, clientY) {
    if (synthPad.classList.contains("disabled")) return;
    isPadActive = true;
    synthPad.classList.add("active");
    $("synth-pad-touch").classList.add("active");
    $("synth-pad-crosshair").classList.add("active");
    updatePadPosition(clientX, clientY);
    startTrailAnimation();
    if (!arpScheduler.running) startArpeggiator();
    if (!kickScheduler.running) startKickLoop();
    if (!amenScheduler.running) startAmenLoop();
  }

  function deactivatePad() {
    isPadActive = false;
    uiState.padActive = false;
    synthPad.classList.remove("active");
    $("synth-pad-touch").classList.remove("active");
    $("synth-pad-crosshair").classList.remove("active");
    stopTrailAnimation();
    if (arpScheduler.running) stopArpeggiator();
    if (kickScheduler.running) stopKickLoop();
    if (amenScheduler.running) stopAmenLoop();
  }

  synthPad.addEventListener("mousedown", (e) => {
    e.preventDefault();
    activatePad(e.clientX, e.clientY);
  });
  document.addEventListener(
    "mousemove",
    (e) => isPadActive && updatePadPosition(e.clientX, e.clientY),
  );
  document.addEventListener("mouseup", () => isPadActive && deactivatePad());

  synthPad.addEventListener("touchstart", (e) => {
    e.preventDefault();
    activatePad(e.touches[0].clientX, e.touches[0].clientY);
  });
  synthPad.addEventListener("touchmove", (e) => {
    e.preventDefault();
    isPadActive &&
      e.touches.length &&
      updatePadPosition(e.touches[0].clientX, e.touches[0].clientY);
  });
  synthPad.addEventListener("touchend", (e) => {
    e.preventDefault();
    deactivatePad();
  });
  synthPad.addEventListener("touchcancel", (e) => {
    e.preventDefault();
    deactivatePad();
  });
}

// ===== SLIDERS =====
function setupSlider(sliderId, valueId, stateKey, fmt = (v) => v) {
  const slider = $(sliderId),
    display = $(valueId);
  if (slider && display) {
    slider.addEventListener("input", (e) => {
      const val = parseFloat(e.target.value);
      uiState[stateKey] = val;
      display.textContent = fmt(val);
    });
  }
}

setupSlider("bpm-slider", "bpm-value", "bpm", (v) => Math.round(v));
setupSlider("root-note-slider", "root-note-value", "rootNote", (v) =>
  Math.round(v),
);
setupSlider("octaves-slider", "octaves-value", "octaves", (v) => Math.round(v));
setupSlider("attack-slider", "attack-value", "attack", (v) => v.toFixed(2));
setupSlider("release-slider", "release-value", "release", (v) => v.toFixed(2));

// Synth/loop selectors
$("synth-select")?.addEventListener(
  "change",
  (e) => (uiState.synth = e.target.value),
);
$("loop-select")?.addEventListener("change", (e) => {
  uiState.loopSample = e.target.value;
  samplesLoaded.loops = false;
});

// Random arp toggle
$("random-arp-toggle")?.addEventListener("click", function () {
  uiState.randomArp = !uiState.randomArp;
  this.setAttribute("data-active", uiState.randomArp.toString());
  this.textContent = uiState.randomArp ? "On" : "Off";
});

// Hue slider with rainbow mode at extremes (slow left, fast right)
let rainbowMode = false,
  rainbowHue = 0,
  rainbowSpeed = 0.2,
  rainbowAnimationId = null;

const hueLabel = $("hue-label");

function applyHueFilter(hue) {
  const filter = `hue-rotate(${hue}deg) saturate(2)`;
  const container = $("synth-ui-container");
  if (container) container.style.filter = filter;
  if (loadingLog) loadingLog.style.filter = filter;
}

function animateRainbow() {
  if (!rainbowMode) return;
  rainbowHue = (rainbowHue + rainbowSpeed) % 360;
  applyHueFilter(rainbowHue);
  rainbowAnimationId = requestAnimationFrame(animateRainbow);
}

$("hue-slider")?.addEventListener("input", (e) => {
  const val = parseInt(e.target.value);
  if (val === 0) {
    rainbowSpeed = 0.2;
    if (hueLabel) hueLabel.textContent = "Hue (Slow Rainbow)";
    if (!rainbowMode) {
      rainbowMode = true;
      animateRainbow();
    }
  } else if (val === 360) {
    rainbowSpeed = 1.5;
    if (hueLabel) hueLabel.textContent = "Hue (Fast Rainbow)";
    if (!rainbowMode) {
      rainbowMode = true;
      animateRainbow();
    }
  } else {
    if (hueLabel) hueLabel.textContent = "Hue";
    if (rainbowMode) {
      rainbowMode = false;
      cancelAnimationFrame(rainbowAnimationId);
    }
    applyHueFilter(val);
  }
});

// ===== TABS =====
setupTabSystem(
  ".main-tab-button",
  ".main-tab-content",
  "mainTab",
  "data-main-tab-content",
);
setupTabSystem(".tab-button", ".tab-content", "tab", "data-tab-content");

// ===== DIVIDER =====
const divider = $("column-divider"),
  leftCol = document.querySelector(".left-column"),
  rightCol = document.querySelector(".right-column");
if (divider && leftCol && rightCol) {
  let dragging = false;
  divider.addEventListener("mousedown", (e) => {
    e.preventDefault();
    dragging = true;
    document.body.style.cursor = "col-resize";
  });
  document.addEventListener("mousemove", (e) => {
    if (!dragging) return;
    const container = leftCol.parentElement.getBoundingClientRect();
    const pct = ((e.clientX - container.left) / container.width) * 100;
    if (pct >= 20 && pct <= 80) {
      leftCol.style.flex = `0 0 ${pct}%`;
      rightCol.style.flex = `0 0 ${100 - pct}%`;
    }
  });
  document.addEventListener("mouseup", () => {
    if (dragging) {
      dragging = false;
      document.body.style.cursor = "";
    }
  });
}

// ===== INIT =====
$("init-button").addEventListener("click", async () => {
  try {
    updateStatus("initializing");
    hideError();
    clearLoadingLog();
    showLoadingLog();
    startLoadingSpinner("Loading");
    addLoadingLogEntry("Initialising SuperSonic...");

    orchestrator = new SuperSonic({
      workerBaseURL: "dist/workers/",
      wasmBaseURL: "dist/wasm/",
      sampleBaseURL: "dist/samples/",
      synthdefBaseURL: "dist/synthdefs/",
    });

    let bootPhase = true;
    const loadingEntries = new Map();

    orchestrator.on("loading:start", (e) => {
      if (!bootPhase) return;
      const entry = document.createElement("div");
      entry.className = `log-entry ${e.type}`;
      entry.textContent = `${e.type}: ${e.name}${e.size ? ` (${(e.size / 1024).toFixed(0)}KB)` : ""}`;
      loadingLogContent?.appendChild(entry);
      loadingLogContent &&
        (loadingLogContent.scrollTop = loadingLogContent.scrollHeight);
      loadingEntries.set(`${e.type}:${e.name}`, entry);
    });

    orchestrator.on("loading:complete", (e) => {
      const entry = loadingEntries.get(`${e.type}:${e.name}`);
      if (entry) {
        entry.classList.add("complete");
        loadingEntries.delete(`${e.type}:${e.name}`);
      }
    });

    orchestrator.on("ready", async () => {
      updateStatus("loading_assets");
      if (DEV_MODE) window.orchestrator = orchestrator;

      await Promise.all([loadAllLoopSamples(), initFXChain()]);

      bootPhase = false;
      stopLoadingSpinner();
      addLoadingLogEntry("Ready", "complete");
      updateStatus("ready");
      hideLoadingLog();

      setSynthPadState("ready");

      const cutoff = $("fx-cutoff-value"),
        mix = $("fx-mix-value");
      if (cutoff) cutoff.textContent = "130.0";
      if (mix) mix.textContent = "0.30";

      setupScope();
    });

    orchestrator.on("message:raw", addMessage);
    orchestrator.on("message:sent", (oscData) => addSentMessage(oscData));
    orchestrator.on("metrics", updateMetrics);
    orchestrator.on("error", (e) => {
      showError(e.message);
      updateStatus("error");
    });

    orchestrator.on("debug", (msg) => {
      $("debug-log").textContent += msg.text + "\n";
      const scroll = $("debug-log").parentElement;
      if (scroll) scroll.scrollTop = scroll.scrollHeight;
      flashTab("debug");
    });

    orchestrator.on("shutdown", () => {
      fxChainInitialized = false;
      fxChainInitializing = null;
      synthdefsLoaded = { fx: false, instruments: false };
      samplesLoaded = { kick: false, loops: false };
    });

    orchestrator.on("audiocontext:suspended", () =>
      showOverlay("suspended-overlay"),
    );
    orchestrator.on("audiocontext:interrupted", () =>
      showOverlay("suspended-overlay"),
    );
    orchestrator.on("audiocontext:resumed", () =>
      hideOverlay("suspended-overlay"),
    );

    await orchestrator.init({ development: DEV_MODE });

    const c2d = $("node-tree-container-2d"),
      c3d = $("node-tree-container-3d");
    if (c2d && c3d) {
      nodeTreeViz = new NodeTreeViz(c2d, c3d, orchestrator);
      await nodeTreeViz.init();
      $("dim-btn")?.addEventListener("click", function () {
        this.textContent = nodeTreeViz.toggleDimensions();
      });
    }
  } catch (e) {
    showError(e.message);
    updateStatus("error");
  }
});

// Reset button
$("reset-button")?.addEventListener("click", async () => {
  if (!orchestrator) return;
  const btn = $("reset-button");
  btn.textContent = "Restarting...";
  btn.disabled = true;

  stopArpeggiator();
  stopKickLoop();
  stopAmenLoop();
  setSynthPadState("disabled");

  await orchestrator.reset();
});

// Resume button
$("resume-button")?.addEventListener("click", async () => {
  if (!orchestrator) return;
  const btn = $("resume-button");
  btn.textContent = "Resuming...";
  btn.disabled = true;

  beginLoadingSequence("Recovering");

  stopArpeggiator();
  stopKickLoop();
  stopAmenLoop();
  setSynthPadState("disabled");
  hideOverlay("suspended-overlay");

  try {
    await orchestrator.recover();
    setSynthPadState("ready");
    endLoadingSequence(true);
  } catch (e) {
    showError(e.message);
    endLoadingSequence(false);
    showOverlay("suspended-overlay");
  }

  btn.textContent = "Resume Audio";
  btn.disabled = false;
});

// Visibility change
document.addEventListener("visibilitychange", () => {
  if (
    document.visibilityState === "visible" &&
    orchestrator?.audioContext?.state === "suspended"
  ) {
    showOverlay("suspended-overlay");
  }
});

// Message form
$("message-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const msg = $("message-input").value.trim();
  if (!msg || !orchestrator) return;

  hideError();

  try {
    const parsed = parseOscTextInput(msg);
    if (parsed.errors.length) {
      showError(parsed.errors[0]);
      return;
    }

    parsed.comments.forEach((c) => addSentMessage(null, c));

    if (parsed.scheduled.size) {
      const now = getNTP();
      for (const [ts, msgs] of [...parsed.scheduled.entries()].sort(
        (a, b) => a[0] - b[0],
      )) {
        const bundle = createOSCBundle(now + ts + 0.1, msgs);
        orchestrator.sendOSC(bundle);
      }
    }

    for (const osc of parsed.immediate) {
      orchestrator.send(osc.address, ...osc.args.map((a) => a.value));
    }
  } catch (e) {
    showError(e.message);
  }
});

// Clear button
$("clear-button")?.addEventListener(
  "click",
  () => ($("message-input").value = ""),
);

// Load example button
$("load-example-button")?.addEventListener("click", async () => {
  await orchestrator?.loadSynthDefs(["sonic-pi-dsaw"]);
  $("message-input").value = `# Cinematic DSaw Pad @ 120 BPM
0.0   /s_new sonic-pi-dsaw -1 0 0 note 52 amp 0.45 attack 0.4 release 3 detune 0.18 cutoff 95 pan -0.2
0.5   /s_new sonic-pi-dsaw -1 0 0 note 55 amp 0.40 attack 0.4 release 3 detune 0.22 cutoff 92 pan 0.2
1.0   /s_new sonic-pi-dsaw -1 0 0 note 59 amp 0.38 attack 0.4 release 3 detune 0.16 cutoff 90 pan -0.15
4.0   /s_new sonic-pi-dsaw -1 0 0 note 48 amp 0.45 attack 0.5 release 3 detune 0.2  cutoff 88 pan 0.15
8.0   /s_new sonic-pi-dsaw -1 0 0 note 47 amp 0.42 attack 0.5 release 4 detune 0.22 cutoff 86 pan -0.1
12.0  /s_new sonic-pi-dsaw -1 0 0 note 52 amp 0.45 attack 0.4 release 4 detune 0.18 cutoff 95 pan 0.1
12.0  /s_new sonic-pi-dsaw -1 0 0 note 28 amp 0.45 attack 0.4 release 5 detune 0.18 cutoff 85 pan 0.1`;
});
