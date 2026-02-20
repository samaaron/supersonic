import { SuperSonic } from "../dist/supersonic.js";
import { NodeTreeViz } from "./node_tree_viz.js";

// ===== CONFIGURATION =====
const DEV_MODE = false;
const NTP_EPOCH_OFFSET = 2208988800; // Seconds between Unix epoch (1970) and NTP epoch (1900)
const LOOKAHEAD = 0.5; // Seconds to schedule ahead for timing accuracy

// Scsynth node groups (keep synths organized in the node tree)
const GROUP_ARP = 100;
const GROUP_FX = 101;
const GROUP_LOOPS = 102;
const GROUP_KICK = 103;

// Audio buses for FX chain routing
const FX_BUS_ARP = 24; // Arp synths output here
const FX_BUS_BEAT = 26; // Beat (kick + loop) outputs here
const FX_BUS_SYNTH_TO_LPF = 20; // Mixed output to LPF
const FX_BUS_LPF_TO_REVERB = 22; // LPF outputs to reverb
const FX_BUS_OUTPUT = 0; // Final output (main out)

// FX node IDs
const FX_LPF_NODE = 2000;
const FX_REVERB_NODE = 2001;
const FX_ARP_LEVEL_NODE = 2002;
const FX_BEAT_LEVEL_NODE = 2003;

// Available synthdefs (Sonic Pi synths)
const FX_SYNTHDEFS = [
  "sonic-pi-fx_lpf",
  "sonic-pi-fx_reverb",
  "sonic-pi-fx_level",
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
  "sonic-pi-zawa",
];

// Kick sample configuration (all stereo)
const KICK_CONFIG = {
  bd_haus: { buffer: 0 },
  bd_boom: { buffer: 7 },
  bd_tek: { buffer: 8 },
  bd_zome: { buffer: 9 },
};

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
let nodeTreeVizActive = false;
let messages = [],
  sentMessages = [],
  sentMessageSeq = 0;

// Batched log updates - collect messages and update DOM in batches
const LOG_BATCH_INTERVAL = 100; // Minimum ms between DOM updates
const LOG_MAX_ITEMS = 50; // Max items to keep in each log

const logBatch = {
  debug: { pending: [], scheduled: false, lastUpdate: 0 },
  oscIn: { pending: [], scheduled: false, lastUpdate: 0 },
  oscOut: { pending: [], scheduled: false, lastUpdate: 0 },
};

// Helper to batch updates with throttling
function batchedUpdate(batch, callback) {
  if (!batch.scheduled) {
    batch.scheduled = true;
    const now = performance.now();
    const elapsed = now - batch.lastUpdate;
    const delay = Math.max(0, LOG_BATCH_INTERVAL - elapsed);

    setTimeout(() => {
      requestAnimationFrame(() => {
        if (batch.pending.length > 0) {
          callback(batch.pending);
          batch.pending = [];
        }
        batch.scheduled = false;
        batch.lastUpdate = performance.now();
      });
    }, delay);
  }
}

let analyser = null,
  scopeAnimationId = null;
let trailParticles = [],
  trailActive = false,
  trailReleaseTime = null;
let fxChainInitialized = false,
  fxChainInitializing = null;
let synthdefsLoaded = { fx: false, instruments: false };
let samplesLoaded = { kick: false, loops: false };
let playbackStartNTP = null;
let beatPulseInterval = null;
let beatPulseTimeout = null;
let appStartTime = null;

const uiState = {
  padX: 0.5,
  padY: 0.5,
  padActive: false,
  synth: "beep",
  loopSample: "loop_amen",
  kickSample: "bd_haus",
  rootNote: 60,
  octaves: 2,
  amplitude: 0.5,
  attack: 0,
  release: 0.1,
  arpMode: "random",
  bpm: 120,
  kickPattern: "four",
  mix: 50, // 0 = full arp, 100 = full beat
};
window.uiState = uiState;

let isAutoPlaying = false;
let runState = 'idle'; // 'idle' | 'running' | 'finishing'
let runGeneration = 0;
let metricsActive = false;
let isIdle = false, idleTimeoutId = null;
const IDLE_DELAY_MS = 2000;

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
      btn.classList.remove("tab-ping");
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

  // Parse args as plain values (osc_fast infers types)
  const args = parts.slice(1).map((arg) => {
    if (
      address === "/d_recv" &&
      /^[0-9a-fA-F]+$/.test(arg) &&
      arg.length > 10
    ) {
      const bytes = new Uint8Array(arg.length / 2);
      for (let i = 0; i < arg.length; i += 2)
        bytes[i / 2] = parseInt(arg.substr(i, 2), 16);
      return bytes;
    }
    if (/^-?\d+$/.test(arg)) return parseInt(arg, 10);
    if (/^-?\d*\.\d+$/.test(arg)) return parseFloat(arg);
    return arg;
  });
  return { address, args };
}

function createOSCBundle(ntpTime, messages) {
  const packets = messages.map((m) => [m.address, ...m.args]);
  return SuperSonic.osc.encodeBundle(ntpTime, packets);
}

function colorizeOSCArgs(oscMsg) {
  const address = oscMsg[0];
  const args = oscMsg.slice(1);
  if (args.length === 0) return "";

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

      if (isFloat) return `<span class="osc-color-float">${parseFloat(value.toFixed(3))}</span>`;
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
  btn.classList.remove("tab-ping");
  void btn.offsetWidth;
  btn.classList.add("tab-ping");
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
  const restartContainer = $("restart-container"),
    hueBar = $("hue-bar");

  if (status === "not_initialized" || status === "error") {
    stopBootAnimation();
    initBtn.textContent = "Boot";
    initBtn.disabled = false;
    if (initContainer) initContainer.style.display = "flex";
    if (scopeContainer) scopeContainer.style.display = "block";
    if (uiContainer) uiContainer.classList.remove("initialized");
    if (hueBar) hueBar.style.display = "none";
    if (restartContainer) restartContainer.style.display = "none";
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
    if (restartContainer) restartContainer.style.display = "flex";
    if (hueBar) hueBar.style.display = "flex";
    $("message-input").disabled = false;
    $$("#message-form button").forEach((b) => (b.disabled = false));
    // Enable footer controls
    $$("#restart-container button").forEach((b) => (b.disabled = false));
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

function positionInitOverlay() {
  const initOverlay = $("init-button-container");
  const pad = $("synth-pad");
  const panel = document.querySelector(".dark-panel");
  if (!initOverlay || !pad || !panel) return;
  const pr = pad.getBoundingClientRect(),
    panelR = panel.getBoundingClientRect();
  Object.assign(initOverlay.style, {
    top: `${pr.top - panelR.top}px`,
    left: `${pr.left - panelR.left}px`,
    width: `${pr.width}px`,
    height: `${pr.height}px`,
  });
}

// Position init overlay on load and resize
positionInitOverlay();
window.addEventListener("resize", positionInitOverlay);

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
            `<span class="osc-color-address">${p[0]}</span> ${colorizeOSCArgs(p)}`,
        )
        .join("<br>");
      // For single-packet bundles, just show the content
      if (msg.packets.length === 1) return content;
      return `<span class="osc-color-string">Bundle (${msg.packets.length})</span><br>${content}`;
    }
    return `<span class="osc-color-address">${msg[0]}</span> ${colorizeOSCArgs(msg)}`;
  } catch (e) {
    return `<span class="osc-color-error">Decode error: ${e.message}</span>`;
  }
}

function addMessage(msg) {
  logBatch.oscIn.pending.push(msg);
  batchedUpdate(logBatch.oscIn, (pending) => {
    const history = $("message-history");
    if (!history) return;

    const empty = history.querySelector(".message-empty");
    if (empty) empty.remove();

    let html = "";
    for (const m of pending) {
      messages.push(m);
      if (messages.length > LOG_MAX_ITEMS) messages.shift();

      const initTime = orchestrator?.initTime || 0;
      const relativeTime = initTime && m.timestamp ? ((m.timestamp - initTime)).toFixed(2) : "";
      const timeStr = relativeTime ? ` ${relativeTime}` : "";
      html += `
        <div class="message-item">
          <span class="message-header">[${m.sequence}]${timeStr}</span>
          <span class="message-content">${m.oscData ? renderOSCMessage(m.oscData) : m.text || "Unknown"}</span>
        </div>
      `;
    }

    history.insertAdjacentHTML("beforeend", html);

    // Trim excess DOM nodes if over limit
    while (history.children.length > LOG_MAX_ITEMS) {
      history.removeChild(history.firstChild);
    }

    history.scrollTop = history.scrollHeight;
    flashTab("osc-in");
  });
}

function addSentMessage({ oscData, sourceId, sequence, timestamp, scheduledTime, comment } = {}) {
  const msg = { oscData, timestamp, comment, sequence, sourceId, scheduledTime };
  logBatch.oscOut.pending.push(msg);
  batchedUpdate(logBatch.oscOut, (pending) => {
    const history = $("sent-message-history");
    if (!history) return;

    const empty = history.querySelector(".message-empty");
    if (empty) empty.remove();

    let html = "";
    for (const m of pending) {
      sentMessages.push(m);
      if (sentMessages.length > LOG_MAX_ITEMS) sentMessages.shift();

      const initTime = orchestrator?.initTime || 0;
      const relativeTime = initTime && m.timestamp ? ((m.timestamp - initTime)).toFixed(2) : "";
      const content = m.comment
        ? `<span class="osc-color-comment">${m.comment}</span>`
        : renderOSCMessage(m.oscData);

      const src = m.sourceId !== null && m.sourceId !== undefined ? ` ch${m.sourceId}` : "";
      const timeStr = relativeTime ? ` ${relativeTime}` : "";
      html += `
        <div class="message-item">
          <span class="message-header">[${m.sequence ?? "?"}]${timeStr}${src}</span>
          <span class="message-content">${content}</span>
        </div>
      `;
    }

    history.insertAdjacentHTML("beforeend", html);

    // Trim excess DOM nodes if over limit
    while (history.children.length > LOG_MAX_ITEMS) {
      history.removeChild(history.firstChild);
    }

    history.scrollTop = history.scrollHeight;
    flashTab("osc-out");
  });
}

// ===== METRICS =====
// Metrics rendering is handled by the <supersonic-metrics> web component.
// See js/lib/metrics_component.js for implementation.
const metricsEl = $("performance-metrics");
metricsEl?.buildFromSchema(SuperSonic);

// ===== SCOPE VISUALISER =====
const scopeCanvas = $("scope-canvas");
const scopeCtx = scopeCanvas?.getContext("2d");
let scopeDataBuffer = null; // Reused buffer for scope data
let scopeW = 0, scopeH = 0;

function resizeScope() {
  if (!scopeCanvas || !scopeCtx) return;
  const dpr = window.devicePixelRatio || 1;
  scopeW = scopeCanvas.offsetWidth;
  scopeH = scopeCanvas.offsetHeight;
  scopeCanvas.width = scopeW * dpr;
  scopeCanvas.height = scopeH * dpr;
  scopeCtx.setTransform(1, 0, 0, 1, 0, 0); // Reset transform before scaling
  scopeCtx.scale(dpr, dpr);
}

// Draw a silent (flat line) scope before boot
function drawSilentScope() {
  if (!scopeCanvas || !scopeCtx) return;
  resizeScope();
  scopeCtx.fillStyle = "#000";
  scopeCtx.fillRect(0, 0, scopeW, scopeH);
  scopeCtx.lineWidth = 3;
  scopeCtx.strokeStyle = "#666";
  scopeCtx.beginPath();
  scopeCtx.moveTo(0, scopeH / 2);
  scopeCtx.lineTo(scopeW, scopeH / 2);
  scopeCtx.stroke();
}

// Draw silent scope on load
drawSilentScope();
window.addEventListener("resize", drawSilentScope);

function setupScope() {
  if (!orchestrator?.node?.context) return;

  // Remove silent scope resize listener
  window.removeEventListener("resize", drawSilentScope);

  analyser = orchestrator.node.context.createAnalyser();
  analyser.fftSize = 2048;
  analyser.smoothingTimeConstant = 0.8;
  scopeDataBuffer = new Uint8Array(analyser.fftSize); // Allocate once

  orchestrator.node.disconnect();
  orchestrator.node.connect(analyser);
  analyser.connect(orchestrator.node.context.destination);

  resizeScope();
  window.addEventListener("resize", resizeScope);
  drawScope();
}

function drawScope() {
  scopeAnimationId = requestAnimationFrame(drawScope);
  if (!analyser || !scopeDataBuffer) return;

  analyser.getByteTimeDomainData(scopeDataBuffer);

  scopeCtx.fillStyle = "#000";
  scopeCtx.fillRect(0, 0, scopeW, scopeH);
  scopeCtx.lineWidth = 3;
  scopeCtx.strokeStyle = "#ff6600";
  scopeCtx.beginPath();

  const slice = scopeW / scopeDataBuffer.length;
  for (let i = 0; i < scopeDataBuffer.length; i++) {
    const y = ((scopeDataBuffer[i] / 128.0) * scopeH) / 2;
    i === 0 ? scopeCtx.moveTo(0, y) : scopeCtx.lineTo(i * slice, y);
  }
  scopeCtx.lineTo(scopeW, scopeH / 2);
  scopeCtx.stroke();

  if (trailActive) updateTrail();
  if (nodeTreeVizActive) nodeTreeViz.update();
}

// ===== TRAIL EFFECT =====
const trailCanvas = $("trail-canvas");
const trailCtx = trailCanvas?.getContext("2d");

// Pre-rendered gradient sprites to avoid per-frame gradient/string allocations
// Creates 10 opacity levels of the glow sprite, drawn once at startup
const SPRITE_LEVELS = 10;
const SPRITE_SIZE = 64; // Base size, will be scaled during draw
const trailSprites = [];

function initTrailSprites() {
  for (let level = 0; level < SPRITE_LEVELS; level++) {
    const alpha = (level + 1) / SPRITE_LEVELS; // 0.1 to 1.0
    const canvas = document.createElement("canvas");
    canvas.width = SPRITE_SIZE;
    canvas.height = SPRITE_SIZE;
    const ctx = canvas.getContext("2d");

    const cx = SPRITE_SIZE / 2;
    const grad = ctx.createRadialGradient(cx, cx, 0, cx, cx, cx);
    grad.addColorStop(0, `rgba(255, 215, 0, ${alpha * 0.9})`);
    grad.addColorStop(0.5, `rgba(255, 160, 0, ${alpha * 0.7})`);
    grad.addColorStop(1, "rgba(255, 102, 0, 0)");

    ctx.fillStyle = grad;
    ctx.fillRect(0, 0, SPRITE_SIZE, SPRITE_SIZE);
    trailSprites.push(canvas);
  }
}

if (trailCtx) initTrailSprites();

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

let lastSpawnTime = 0;
const MIN_SPAWN_INTERVAL = 50; // Max 20 spawns/sec = 60 particles/sec

function spawnTrailParticle() {
  if (!trailCanvas || !uiState.padActive) return;

  const now = performance.now();
  if (now - lastSpawnTime < MIN_SPAWN_INTERVAL) return;
  lastSpawnTime = now;

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
      maxLife: 1.2 + Math.random() * 0.8,
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
    trailActive = false;
    trailReleaseTime = null;
    return;
  }

  trailCtx.fillStyle = "rgba(0, 0, 0, 0.08)";
  trailCtx.fillRect(0, 0, trailCanvas.width, trailCanvas.height);
  trailCtx.globalCompositeOperation = "lighter";

  // In-place compaction to avoid array allocation every frame
  let writeIdx = 0;
  for (let i = 0; i < trailParticles.length; i++) {
    const p = trailParticles[i];
    p.life -= 16.67 / (p.maxLife * 1000);
    if (p.life <= 0) continue;

    p.x += p.vx;
    p.y += p.vy;
    p.vx *= 0.98;
    p.vy *= 0.98;

    const size = p.size * (0.5 + p.life * 0.5);
    // Use pre-rendered sprite instead of creating gradient per particle
    const spriteIdx = Math.min(SPRITE_LEVELS - 1, Math.floor(p.life * SPRITE_LEVELS));
    const sprite = trailSprites[spriteIdx];
    const drawSize = size * 2;
    trailCtx.drawImage(sprite, p.x - size, p.y - size, drawSize, drawSize);

    trailParticles[writeIdx++] = p;
  }
  trailParticles.length = writeIdx;

  trailCtx.globalCompositeOperation = "source-over";
}

function startTrailAnimation() {
  trailReleaseTime = null;
  trailActive = true;
}

function stopTrailAnimation() {
  trailReleaseTime = performance.now();
}

// ===== VISIBILITY OBSERVER (CSS animation gating) =====
let visibilityObserver = null;

function createVisibilityObserver() {
  visibilityObserver = new IntersectionObserver((entries) => {
    for (const entry of entries) {
      entry.target.classList.toggle("in-view", entry.isIntersecting);
    }
  });
  observeGlowElements();
}

function observeGlowElements() {
  if (!visibilityObserver) return;
  const btnInit = $("boot-button");
  if (btnInit) visibilityObserver.observe(btnInit);
  for (const el of $$(".sponsor-item")) {
    visibilityObserver.observe(el);
  }
}

function disconnectVisibilityObserver() {
  if (!visibilityObserver) return;
  visibilityObserver.disconnect();
  $("boot-button")?.classList.remove("in-view");
  for (const el of $$(".sponsor-item")) {
    el.classList.remove("in-view");
  }
}

// ===== IDLE SUSPEND =====
function checkIdle() {
  const anyActive = schedulerRunning.arp || schedulerRunning.kick || schedulerRunning.amen || isAutoPlaying;
  if (anyActive) {
    if (idleTimeoutId !== null) {
      clearTimeout(idleTimeoutId);
      idleTimeoutId = null;
    }
    if (isIdle) wakeUp();
    return;
  }
  if (idleTimeoutId === null && !isIdle) {
    idleTimeoutId = setTimeout(goIdle, IDLE_DELAY_MS);
  }
}

async function goIdle() {
  if (isIdle) return;
  isIdle = true;
  idleTimeoutId = null;
  runState = 'idle';

  if (scopeAnimationId) {
    cancelAnimationFrame(scopeAnimationId);
    scopeAnimationId = null;
  }
  trailActive = false;

  if (rainbowAnimationId) {
    cancelAnimationFrame(rainbowAnimationId);
    rainbowAnimationId = null;
  }

  nodeTreeVizActive = false;

  metricsEl?.disconnect();
  metricsActive = false;

  disconnectVisibilityObserver();
  drawSilentScope();
  await orchestrator?.purge();
  orchestrator?.suspend();
}

async function wakeUp() {
  if (!isIdle) return;
  isIdle = false;

  await orchestrator?.resume();

  if (analyser && !scopeAnimationId) {
    drawScope();
  }

  if (rainbowMode && !rainbowAnimationId) {
    animateRainbow();
  }

  if (uiState.padActive || isAutoPlaying) {
    trailActive = true;
  }

  if (nodeTreeViz) {
    nodeTreeVizActive = true;
  }

  if (orchestrator && !metricsActive) {
    metricsEl?.connect(orchestrator, { refreshRate: 10 });
    metricsActive = true;
  }

  observeGlowElements();
}

createVisibilityObserver();

// ===== SCHEDULER WORKER =====
let schedulerWorker = null;
let schedulerRunning = { arp: false, kick: false, amen: false };

function initSchedulerWorker() {
  if (schedulerWorker) return;
  if (!orchestrator) {
    console.error("Cannot init scheduler worker before orchestrator");
    return;
  }

  schedulerWorker = new Worker("assets/scheduler-worker.js", { type: "module" });

  schedulerWorker.onmessage = (e) => {
    const { type } = e.data;
    if (type === "ready") {
      if (DEV_MODE) console.log("Scheduler worker ready, sending OscChannel...");
      // Create an OscChannel and transfer it to the worker for direct worklet communication
      const channel = orchestrator.createOscChannel();
      schedulerWorker.postMessage(
        {
          type: "initChannel",
          channel: channel.transferable,
          config: {
            groups: { GROUP_ARP, GROUP_LOOPS, GROUP_KICK },
            buses: { FX_BUS_ARP, FX_BUS_BEAT },
            loopConfig: LOOP_CONFIG,
            kickConfig: KICK_CONFIG,
          },
        },
        channel.transferList
      );
    } else if (type === "channelReady") {
      if (DEV_MODE) console.log("Scheduler worker has direct worklet connection");
      $("play-toggle")?.classList.remove("disabled");
    } else if (type === "started" && e.data.scheduler === "arp") {
      // Sync main thread timing with worker for beat pulse
      playbackStartNTP = e.data.playbackStartNTP;
      startBeatPulse();
      startTrailAnimation();
    }
  };

  schedulerWorker.onerror = (e) => {
    console.error("Scheduler worker error:", e.message);
  };

  // Send initial state
  sendStateToWorker();
}

function sendStateToWorker(updates = null) {
  if (!schedulerWorker) return;

  const stateToSend = updates || {
    synth: uiState.synth,
    rootNote: uiState.rootNote,
    octaves: uiState.octaves,
    amplitude: uiState.amplitude,
    attack: uiState.attack,
    release: uiState.release,
    arpMode: uiState.arpMode,
    bpm: uiState.bpm,
    kickPattern: uiState.kickPattern,
    loopSample: uiState.loopSample,
  };

  schedulerWorker.postMessage({ type: "state", ...stateToSend });
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

    // Level controls for arp/beat mix - must run BEFORE LPF
    // Action 1 = add to tail of group
    orchestrator.send(
      "/s_new",
      "sonic-pi-fx_level",
      FX_ARP_LEVEL_NODE,
      1, // add to tail
      GROUP_FX,
      "in_bus",
      FX_BUS_ARP,
      "out_bus",
      FX_BUS_SYNTH_TO_LPF,
      "amp",
      0.5, // Start at 50% (mix = 50)
    );
    orchestrator.send(
      "/s_new",
      "sonic-pi-fx_level",
      FX_BEAT_LEVEL_NODE,
      3, // add after previous
      FX_ARP_LEVEL_NODE,
      "in_bus",
      FX_BUS_BEAT,
      "out_bus",
      FX_BUS_SYNTH_TO_LPF,
      "amp",
      0.5, // Start at 50% (mix = 50)
    );

    // LPF after level synths
    orchestrator.send(
      "/s_new",
      "sonic-pi-fx_lpf",
      FX_LPF_NODE,
      3, // add after beat level
      FX_BEAT_LEVEL_NODE,
      "in_bus",
      FX_BUS_SYNTH_TO_LPF,
      "out_bus",
      FX_BUS_LPF_TO_REVERB,
      "cutoff",
      130.0,
      "res",
      0.5,
    );
    // Reverb after LPF
    orchestrator.send(
      "/s_new",
      "sonic-pi-fx_reverb",
      FX_REVERB_NODE,
      3, // add after LPF
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


// Beat pulse
function startBeatPulse() {
  // Don't start if already running
  if (beatPulseInterval || beatPulseTimeout) return;

  const interval = Math.max(0.15, 0.125 / getBpmScale());
  const touch = $("synth-pad-touch");
  if (!touch || !playbackStartNTP) return;

  const now = getNTP();
  const timeSince = now - playbackStartNTP;

  // Calculate time until next beat (handles both before and after playbackStartNTP)
  let nextIn;
  if (timeSince < 0) {
    // We're before the first beat - wait for it
    nextIn = -timeSince;
  } else {
    // We're after the start - sync to next beat boundary
    nextIn = interval - (timeSince % interval);
  }

  beatPulseTimeout = setTimeout(() => {
    beatPulseTimeout = null;
    triggerBeatPulse();
    beatPulseInterval = setInterval(triggerBeatPulse, interval * 1000);
  }, nextIn * 1000);
}

function stopBeatPulse() {
  if (beatPulseTimeout) {
    clearTimeout(beatPulseTimeout);
    beatPulseTimeout = null;
  }
  if (beatPulseInterval) {
    clearInterval(beatPulseInterval);
    beatPulseInterval = null;
  }
}

function restartBeatPulse() {
  // Only restart if currently running
  if (!beatPulseInterval && !beatPulseTimeout) return;
  stopBeatPulse();
  startBeatPulse();
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
  if (!schedulerWorker) initSchedulerWorker();
  schedulerWorker.postMessage({ type: "start", scheduler: "arp" });
  schedulerRunning.arp = true;
  // Beat pulse is started via worker's "started" message for proper sync
}

function stopArpeggiator() {
  if (schedulerWorker) {
    schedulerWorker.postMessage({ type: "stop", scheduler: "arp" });
  }
  schedulerRunning.arp = false;
  stopBeatPulse();
}

async function loadAllKickSamples() {
  await Promise.all(
    Object.entries(KICK_CONFIG).map(([name, cfg]) =>
      orchestrator.loadSample(cfg.buffer, `${name}.flac`),
    ),
  );
  samplesLoaded.kick = true;
}

async function startKickLoop() {
  if (!orchestrator) return;
  if (!fxChainInitialized) await initFXChain();
  if (!samplesLoaded.kick) {
    await loadAllKickSamples();
  }
  if (!schedulerWorker) initSchedulerWorker();
  schedulerWorker.postMessage({ type: "start", scheduler: "kick" });
  schedulerRunning.kick = true;
}

function stopKickLoop() {
  if (schedulerWorker) {
    schedulerWorker.postMessage({ type: "stop", scheduler: "kick" });
  }
  schedulerRunning.kick = false;
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
  if (!schedulerWorker) initSchedulerWorker();
  schedulerWorker.postMessage({ type: "start", scheduler: "amen" });
  schedulerRunning.amen = true;
}

function stopAmenLoop() {
  if (schedulerWorker) {
    schedulerWorker.postMessage({ type: "stop", scheduler: "amen" });
  }
  schedulerRunning.amen = false;
}

// Mute the final mixer with a ramp to avoid pops/clicks
// Returns a promise that resolves when the ramp is complete
let muteTimeoutIds = [];

function cancelPendingMute() {
  for (const id of muteTimeoutIds) clearTimeout(id);
  muteTimeoutIds = [];
}

function muteOutput(durationMs = 500) {
  if (!orchestrator || !fxChainInitialized) return Promise.resolve();
  cancelPendingMute();
  const steps = 10;
  const interval = durationMs / steps;

  return new Promise(resolve => {
    for (let i = 0; i <= steps; i++) {
      const id = setTimeout(() => {
        const amp = 1 - (i / steps);
        const arpAmp = amp * (1 - uiState.mix / 100);
        const beatAmp = amp * (uiState.mix / 100);
        orchestrator.send("/n_set", FX_ARP_LEVEL_NODE, "amp", arpAmp);
        orchestrator.send("/n_set", FX_BEAT_LEVEL_NODE, "amp", beatAmp);
        if (i === steps) resolve();
      }, i * interval);
      muteTimeoutIds.push(id);
    }
  });
}

function unmuteOutput() {
  cancelPendingMute();
  if (!orchestrator || !fxChainInitialized) return;
  const arpAmp = 1 - uiState.mix / 100;
  const beatAmp = uiState.mix / 100;
  orchestrator.send("/n_set", FX_ARP_LEVEL_NODE, "amp", arpAmp);
  orchestrator.send("/n_set", FX_BEAT_LEVEL_NODE, "amp", beatAmp);
}

// Start all schedulers together (for initial start or restart)
// Uses a generation counter so that if stopAll() or another startAll()
// fires while we're mid-async, the stale invocation bails out.
async function startAll() {
  if (!orchestrator) return;
  if (runState === 'running') return;

  const gen = ++runGeneration;
  runState = 'running';
  const stale = () => gen !== runGeneration;

  if (isIdle) {
    // Waking from idle — resume handles purge
    await wakeUp();
    if (stale()) return;
  } else {
    // Quick restart — cancel pending idle timer, clean up stale state
    if (idleTimeoutId !== null) {
      clearTimeout(idleTimeoutId);
      idleTimeoutId = null;
    }
    // Cancel any in-flight JS-level mute ramp from the previous stopAll()
    cancelPendingMute();
    // Clear all pending scheduled messages (both prescheduler and WASM scheduler)
    await orchestrator.purge();
    if (stale()) return;
    // Send a fast ramp-down as timestamped OSC bundles — all at once,
    // with incrementing timestamps so the WASM scheduler spreads them over
    // real audio frames. This avoids setTimeout races entirely.
    const STARTUP_DELAY_MS = 100;  // Headroom for ring buffer transit (matches scheduler concept)
    const rampMs = 50;
    const rampSteps = 10;
    const stepMs = rampMs / rampSteps;
    const baseNTP = getNTP() + STARTUP_DELAY_MS / 1000;
    const arpRatio = 1 - uiState.mix / 100;
    const beatRatio = uiState.mix / 100;
    for (let i = 0; i <= rampSteps; i++) {
      const amp = 1 - (i / rampSteps);
      const bundle = createOSCBundle(baseNTP + (i * stepMs) / 1000, [
        { address: "/n_set", args: [FX_ARP_LEVEL_NODE, "amp", amp * arpRatio] },
        { address: "/n_set", args: [FX_BEAT_LEVEL_NODE, "amp", amp * beatRatio] },
      ]);
      orchestrator.sendOSC(bundle);
    }
    // Wait for the ramp to complete at the audio level
    await new Promise(r => setTimeout(r, STARTUP_DELAY_MS + rampMs + 10));
    if (stale()) return;
    // Free source groups to kill any still-sounding synths (preserves FX chain)
    orchestrator.send("/g_freeAll", GROUP_ARP);
    orchestrator.send("/g_freeAll", GROUP_LOOPS);
    orchestrator.send("/g_freeAll", GROUP_KICK);
  }

  // Load everything needed (bail after each await if superseded)
  if (!synthdefsLoaded.instruments) { await loadSynthdefs("instruments"); if (stale()) return; }
  if (!fxChainInitialized) { await initFXChain(); if (stale()) return; }
  if (!samplesLoaded.kick) { await loadAllKickSamples(); if (stale()) return; }
  if (!samplesLoaded.loops) { await loadAllLoopSamples(); if (stale()) return; }
  if (!schedulerWorker) initSchedulerWorker();
  // Unmute and reset scheduler state before starting
  unmuteOutput();
  schedulerWorker.postMessage({ type: "reset" });
  // Send single message to start all schedulers atomically
  schedulerWorker.postMessage({ type: "start", scheduler: "all" });
  schedulerRunning.arp = true;
  schedulerRunning.kick = true;
  schedulerRunning.amen = true;
}

function stopAll() {
  ++runGeneration; // Invalidate any in-flight startAll
  if (schedulerWorker) {
    schedulerWorker.postMessage({ type: "stop", scheduler: "all" });
  }
  schedulerRunning.arp = false;
  schedulerRunning.kick = false;
  schedulerRunning.amen = false;
  stopBeatPulse();
  if (runState === 'running') {
    // Graceful finish: ramp down over 500ms, then transition to idle
    runState = 'finishing';
    muteOutput().then(() => {
      if (runState === 'finishing') runState = 'idle';
    });
  } else {
    cancelPendingMute();
    runState = 'idle';
  }
  checkIdle();
}

function resetAutoPlay() {
  isAutoPlaying = false;
  const btn = $("play-toggle");
  if (btn) {
    btn.setAttribute("aria-pressed", "false");
    btn.textContent = "Autoplay";
  }
}

// Expose globally
window.startArpeggiator = startArpeggiator;
window.stopArpeggiator = stopArpeggiator;
window.startKickLoop = startKickLoop;
window.stopKickLoop = stopKickLoop;
window.startAmenLoop = startAmenLoop;
window.stopAmenLoop = stopAmenLoop;
window.startAll = startAll;
window.stopAll = stopAll;

// ===== SYNTH PAD =====
const synthPad = $("synth-pad");
// Cache pad element references to avoid DOM lookups in hot path
const padTouch = $("synth-pad-touch");
const padCrosshairH = $("synth-pad-crosshair-h");
const padCrosshairV = $("synth-pad-crosshair-v");
const padXVal = $("pad-x-value");
const padYVal = $("pad-y-value");

if (synthPad) {
  synthPad.classList.add("disabled");
  $("play-toggle")?.classList.add("disabled");
  let isPadActive = false;

  function updatePadPosition(clientX, clientY) {
    const rect = synthPad.getBoundingClientRect();
    const x = Math.max(0, Math.min(1, (clientX - rect.left) / rect.width));
    const y = Math.max(0, Math.min(1, 1 - (clientY - rect.top) / rect.height));

    uiState.padX = x;
    uiState.padY = y;
    // Preserve padActive during autoplay (don't let mouse-up override it)
    if (!isAutoPlaying) uiState.padActive = isPadActive;

    const px = x * rect.width,
      py = (1 - y) * rect.height;
    padTouch.style.left = px + "px";
    padTouch.style.top = py + "px";
    padCrosshairH.style.top = py + "px";
    padCrosshairV.style.left = px + "px";

    if (padXVal) padXVal.textContent = x.toFixed(2);
    if (padYVal) padYVal.textContent = y.toFixed(2);

    updateFXParameters(x, y);
  }

  function activatePad(clientX, clientY) {
    if (synthPad.classList.contains("disabled")) return;
    isPadActive = true;
    synthPad.classList.add("active");
    padTouch.classList.add("active");
    $("synth-pad-crosshair").classList.add("active");
    updatePadPosition(clientX, clientY);
    startAll(); // Handles wakeUp, re-entrance, and finishing abort
  }

  function deactivatePad() {
    isPadActive = false;
    // Only fully deactivate if not in autoplay mode
    if (!isAutoPlaying) {
      uiState.padActive = false;
      synthPad.classList.remove("active");
      padTouch.classList.remove("active");
      $("synth-pad-crosshair").classList.remove("active");
      stopTrailAnimation();
      stopAll();
    }
  }

  synthPad.addEventListener("mousedown", (e) => {
    if (e.target.closest("#play-toggle")) return;
    e.preventDefault();
    activatePad(e.clientX, e.clientY);
  });
  document.addEventListener(
    "mousemove",
    (e) => isPadActive && updatePadPosition(e.clientX, e.clientY),
  );
  document.addEventListener("mouseup", () => isPadActive && deactivatePad());

  synthPad.addEventListener("touchstart", (e) => {
    if (e.target.closest("#play-toggle")) return;
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
    if (e.target.closest("#play-toggle")) return;
    e.preventDefault();
    deactivatePad();
  });
  synthPad.addEventListener("touchcancel", (e) => {
    if (e.target.closest("#play-toggle")) return;
    e.preventDefault();
    deactivatePad();
  });
}

// ===== PLAY TOGGLE =====
$("play-toggle")?.addEventListener("click", async function () {
  const btn = this;
  if (btn.classList.contains("disabled")) return;

  isAutoPlaying = !isAutoPlaying;
  btn.setAttribute("aria-pressed", isAutoPlaying.toString());
  btn.textContent = isAutoPlaying ? "Stop" : "Autoplay";

  const crosshair = $("synth-pad-crosshair");

  if (isAutoPlaying) {
    // Start playback with visual state
    synthPad?.classList.add("active");
    padTouch?.classList.add("active");
    crosshair?.classList.add("active");

    // Animate pointer to top-left area
    if (synthPad && padTouch) {
      const rect = synthPad.getBoundingClientRect();
      const padding = 60;
      const targetPx = padding;
      const targetPy = padding;

      // Get current position (default to center if not set)
      const startPx = parseFloat(padTouch.style.left) || rect.width / 2;
      const startPy = parseFloat(padTouch.style.top) || rect.height / 2;

      // Enable trail animation
      uiState.padActive = true;

      const duration = 1200; // ms
      const startTime = performance.now();

      function animatePointer(currentTime) {
        const elapsed = currentTime - startTime;
        const progress = Math.min(elapsed / duration, 1);
        // Ease out cubic for smooth deceleration
        const eased = 1 - Math.pow(1 - progress, 3);

        const px = startPx + (targetPx - startPx) * eased;
        const py = startPy + (targetPy - startPy) * eased;

        padTouch.style.left = px + "px";
        padTouch.style.top = py + "px";
        if (padCrosshairH) padCrosshairH.style.top = py + "px";
        if (padCrosshairV) padCrosshairV.style.left = px + "px";

        // Update normalized coordinates (y is inverted: 0=bottom, 1=top)
        const x = px / rect.width;
        const y = 1 - py / rect.height;
        uiState.padX = x;
        uiState.padY = y;
        if (padXVal) padXVal.textContent = x.toFixed(2);
        if (padYVal) padYVal.textContent = y.toFixed(2);
        updateFXParameters(x, y);

        if (progress < 1) {
          requestAnimationFrame(animatePointer);
        }
      }
      requestAnimationFrame(animatePointer);
    }

    // Trail animation starts via "started" message for proper sync
    startAll();
  } else {
    // Stop playback and remove visual state
    synthPad?.classList.remove("active");
    padTouch?.classList.remove("active");
    crosshair?.classList.remove("active");
    stopTrailAnimation();
    stopAll();
  }
});

// ===== SLIDERS =====
function setupSlider(sliderId, valueId, stateKey, fmt = (v) => v) {
  const slider = $(sliderId),
    display = $(valueId);
  if (slider && display) {
    slider.addEventListener("input", (e) => {
      const val = parseFloat(e.target.value);
      uiState[stateKey] = val;
      display.textContent = fmt(val);
      sendStateToWorker({ [stateKey]: val });
    });
  }
}

setupSlider("bpm-slider", "bpm-value", "bpm", (v) => Math.round(v));
// Also restart beat pulse animation when BPM changes
$("bpm-slider")?.addEventListener("input", () => restartBeatPulse());
$("root-note-select")?.addEventListener("change", (e) => {
  uiState.rootNote = parseInt(e.target.value, 10);
  sendStateToWorker({ rootNote: uiState.rootNote });
});
setupSlider("octaves-slider", "octaves-value", "octaves", (v) => Math.round(v));
setupSlider("attack-slider", "attack-value", "attack", (v) => v.toFixed(2));
setupSlider("release-slider", "release-value", "release", (v) => v.toFixed(2));

// Synth/loop selectors
$("synth-select")?.addEventListener("change", (e) => {
  uiState.synth = e.target.value;
  sendStateToWorker({ synth: uiState.synth });
});
$("loop-select")?.addEventListener("change", (e) => {
  uiState.loopSample = e.target.value;
  samplesLoaded.loops = false;
  sendStateToWorker({ loopSample: uiState.loopSample });
});

// Arp mode selector
$("arp-mode-select")?.addEventListener("change", (e) => {
  uiState.arpMode = e.target.value;
  sendStateToWorker({ arpMode: uiState.arpMode });
});

// Kick pattern selector
$("kick-pattern-select")?.addEventListener("change", (e) => {
  uiState.kickPattern = e.target.value;
  sendStateToWorker({ kickPattern: uiState.kickPattern });
});

// Kick sample selector
$("kick-select")?.addEventListener("change", (e) => {
  uiState.kickSample = e.target.value;
  sendStateToWorker({ kickSample: uiState.kickSample });
});

// Mix slider (arp <-> beat crossfade) - live control via /n_set
$("mix-slider")?.addEventListener("input", (e) => {
  uiState.mix = parseInt(e.target.value, 10);
  if (orchestrator && fxChainInitialized) {
    const arpAmp = 1 - uiState.mix / 100;
    const beatAmp = uiState.mix / 100;
    orchestrator.send("/n_set", FX_ARP_LEVEL_NODE, "amp", arpAmp);
    orchestrator.send("/n_set", FX_BEAT_LEVEL_NODE, "amp", beatAmp);
  }
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
  ".segmented-toggle__btn",
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
    // Only allow dragging when initialized
    if (!$("synth-ui-container")?.classList.contains("initialized")) return;
    e.preventDefault();
    dragging = true;
    document.body.style.cursor = "col-resize";
  });
  document.addEventListener("mousemove", (e) => {
    if (!dragging) return;
    const container = leftCol.parentElement.getBoundingClientRect();
    const pct = ((e.clientX - container.left) / container.width) * 100;
    if (pct >= 25 && pct <= 75) {
      // Use flex-grow ratios - flexbox handles the divider/gap automatically
      leftCol.style.flex = `${pct} 1 0`;
      rightCol.style.flex = `${100 - pct} 1 0`;
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
    appStartTime = Date.now();
    updateStatus("initializing");
    hideError();
    clearLoadingLog();
    showLoadingLog();
    startLoadingSpinner("Loading");
    addLoadingLogEntry("Initialising SuperSonic...");

    orchestrator = new SuperSonic({
      baseURL: "dist/",
      mode: "sab",
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

      // Samples and FX chain are initialized in 'setup' event
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

      // Start idle detection — nothing is playing after boot,
      // so this will trigger the idle timeout and suspend the engine
      checkIdle();
    });

    orchestrator.on("message:raw", addMessage);
    orchestrator.on("message:sent", addSentMessage);
    // Connect the <supersonic-metrics> web component (schema-driven, zero-alloc hot path)
    metricsEl?.connect(orchestrator, { refreshRate: 10 });
    metricsActive = true; // Flag for idle/wakeUp to know metrics are active
    orchestrator.on("error", (e) => {
      showError(e.message);
      updateStatus("error");
    });

    orchestrator.on("debug", (msg) => {
      logBatch.debug.pending.push(msg.text);
      batchedUpdate(logBatch.debug, (pending) => {
        const log = $("debug-log");
        const newText = pending.join("\n") + "\n";
        log.textContent += newText;
        // Trim if too long (keep last 50KB)
        if (log.textContent.length > 50000) {
          log.textContent = log.textContent.slice(-40000);
        }
        const scroll = log.parentElement;
        if (scroll) scroll.scrollTop = scroll.scrollHeight;
        flashTab("debug");
      });
    });

    // Rebuild group structure on setup (fires on both init and recover)
    orchestrator.on("setup", async () => {
      // Reset state - in postMessage mode, WASM memory is destroyed on recover
      fxChainInitialized = false;
      fxChainInitializing = null;
      synthdefsLoaded = { fx: false, instruments: false };
      samplesLoaded = { kick: false, loops: false };
      playbackStartNTP = null;

      // Re-initialize scheduler worker with new OscChannel after reset
      if (schedulerWorker) {
        schedulerWorker.postMessage({ type: "reset" });
        const channel = orchestrator.createOscChannel();
        schedulerWorker.postMessage(
          {
            type: "initChannel",
            channel: channel.transferable,
            config: {
              groups: { GROUP_ARP, GROUP_LOOPS, GROUP_KICK },
              buses: { FX_BUS_ARP, FX_BUS_BEAT },
              loopConfig: LOOP_CONFIG,
              kickConfig: KICK_CONFIG,
            },
          },
          channel.transferList
        );
      }

      await loadAllLoopSamples();
      await loadAllKickSamples();
      await loadSynthdefs("instruments");
      await initFXChain();
      initSchedulerWorker();
    });

    orchestrator.on("audiocontext:suspended", () => {
      if (!isIdle) {
        // Kill schedulers immediately so they don't catch up and flood
        // the engine with hundreds of past-due events while suspended.
        stopAll();
        resetAutoPlay();
        showOverlay("suspended-overlay");
      }
    });
    orchestrator.on("audiocontext:interrupted", () => {
      if (!isIdle) {
        stopAll();
        resetAutoPlay();
        showOverlay("suspended-overlay");
      }
    });
    orchestrator.on("audiocontext:resumed", () =>
      hideOverlay("suspended-overlay"),
    );

    await orchestrator.init();

    const c2d = $("node-tree-container-2d"),
      c3d = $("node-tree-container-3d");
    if (c2d && c3d) {
      nodeTreeViz = new NodeTreeViz(c2d, c3d, orchestrator);
      await nodeTreeViz.init();
      // Stop self-polling — the unified drawScope loop drives updates
      nodeTreeViz.stopPolling();
      nodeTreeVizActive = true;
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

  stopAll();
  resetAutoPlay();
  setSynthPadState("disabled");

  try {
    await orchestrator.recover();
    setSynthPadState("ready");
  } catch (e) {
    showError(`Restart failed: ${e.message}`);
  } finally {
    btn.textContent = "Restart";
    btn.disabled = false;
  }
});

// Resume button
$("resume-button")?.addEventListener("click", async () => {
  if (!orchestrator) return;
  const btn = $("resume-button");
  btn.textContent = "Resuming...";
  btn.disabled = true;

  beginLoadingSequence("Recovering");

  stopAll();
  resetAutoPlay();
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

// Fullscreen button
$("fullscreen-button")?.addEventListener("click", () => {
  const container = $("synth-ui-container");
  if (!container) return;

  if (document.fullscreenElement) {
    document.exitFullscreen();
  } else {
    container.requestFullscreen().catch(err => {
      console.warn("Fullscreen request failed:", err);
    });
  }
});

// Visibility change
document.addEventListener("visibilitychange", () => {
  if (isIdle) return;
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

  // Wake engine if idle so OSC messages are actually processed
  await wakeUp();

  try {
    const parsed = parseOscTextInput(msg);
    if (parsed.errors.length) {
      showError(parsed.errors[0]);
      return;
    }

    parsed.comments.forEach((c) => addSentMessage({ comment: c }));

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

// Copy metrics button
$("copy-metrics-btn")?.addEventListener("click", async () => {
  if (!orchestrator) return;

  const snapshot = orchestrator.getSnapshot();

  try {
    await navigator.clipboard.writeText(JSON.stringify(snapshot, null, 2));
    const btn = $("copy-metrics-btn");
    btn.classList.add("copied");
    setTimeout(() => btn.classList.remove("copied"), 1500);
  } catch (err) {
    console.error("Failed to copy metrics:", err);
  }
});

// Metrics tooltips are now applied by the <supersonic-metrics> component from schema
