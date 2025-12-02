
import { SuperSonic } from '../dist/supersonic.js';

let orchestrator = null;
let messages = [];
let sentMessages = [];
let runCounter = 0;

// Development mode - enables console logging for debugging
/* DEMO_BUILD_CONFIG */ const DEV_MODE = false;

// Loop sample configuration: rate and duration for each sample
const LOOP_CONFIG = {
  'loop_amen': { rate: 0.8767, duration: 2.0 },
  'loop_breakbeat': { rate: 0.9524, duration: 2.0 },
  'loop_compus': { rate: 0.8108, duration: 8.0 },
  'loop_garzul': { rate: 1.0, duration: 8.0 },
  'loop_industrial': { rate: 0.8837, duration: 1.0 },
  'loop_tabla': { rate: 1.3342, duration: 8.0 }
};

// Buffer assignments for loop samples
const LOOP_BUFFERS = {
  'loop_amen': 1,
  'loop_breakbeat': 2,
  'loop_compus': 3,
  'loop_garzul': 4,
  'loop_industrial': 5,
  'loop_tabla': 6
};

// Central UI State Object
const uiState = {
  // Trackpad state
  padX: 0.5,
  padY: 0.5,
  padActive: false,

  // Slider values
  synth: 'beep',
  loopSample: 'loop_amen',
  rootNote: 60,
  octaves: 2,
  amplitude: 0.5,
  attack: 0,
  release: 0.1,
  randomArp: true,
  bpm: 120
};

// BPM scaling factor (relative to 120 BPM baseline)
function getBpmScale() {
  return uiState.bpm / 120;
}

// Make uiState globally accessible for debugging and audio algorithm access
window.uiState = uiState;

// Scope visualiser
let analyser = null;
let scopeCanvas = null;
let scopeCtx = null;
let scopeAnimationId = null;

// Trail effect
let trailCanvas = null;
let trailCtx = null;
let trailParticles = [];
let trailAnimationId = null;
let lastTrailX = null;
let lastTrailY = null;
let trailReleaseTime = null;

// DOM elements
const initButton = document.getElementById('init-button');
const resetButton = document.getElementById('reset-button');
const restartContainer = document.getElementById('restart-container');
const suspendedOverlay = document.getElementById('suspended-overlay');
const resumeButton = document.getElementById('resume-button');
// const statusBadge = document.getElementById('status-badge');
const messageInput = document.getElementById('message-input');
const messageForm = document.getElementById('message-form');
const messageHistory = document.getElementById('message-history');
const sentMessageHistory = document.getElementById('sent-message-history');
const errorDiv = document.getElementById('error-message');
const errorText = document.getElementById('error-text');
const warningDiv = document.getElementById('warning-message');
const warningText = document.getElementById('warning-text');
const debugLog = document.getElementById('debug-log');

// Scope canvas
scopeCanvas = document.getElementById('scope-canvas');
scopeCtx = scopeCanvas.getContext('2d');

// Trail canvas
trailCanvas = document.getElementById('trail-canvas');
if (trailCanvas) {
  trailCtx = trailCanvas.getContext('2d');
  // Set canvas size to match container
  const pad = document.getElementById('synth-pad');
  if (pad) {
    const resizeTrailCanvas = () => {
      trailCanvas.width = pad.offsetWidth;
      trailCanvas.height = pad.offsetHeight;
    };
    resizeTrailCanvas();
    window.addEventListener('resize', resizeTrailCanvas);
  }
}

// Helper functions
// Parse text to OSC message
function parseTextToOSC(text) {
  const parts = [];
  let current = '';
  let inQuotes = false;

  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (ch === '"') {
      inQuotes = !inQuotes;
    } else if (ch === ' ' && !inQuotes) {
      if (current) {
        parts.push(current);
        current = '';
      }
    } else {
      current += ch;
    }
  }
  if (current) parts.push(current);

  if (parts.length === 0) {
    throw new Error('Empty message');
  }

  const address = parts[0];
  if (!address.startsWith('/')) {
    throw new Error('OSC address must start with /');
  }

  const args = [];
  for (let j = 1; j < parts.length; j++) {
    const arg = parts[j];

    // Special handling for /d_recv with hex blob data
    if (address === '/d_recv' && /^[0-9a-fA-F]+$/.test(arg) && arg.length > 10) {
      // Convert hex string to Uint8Array blob
      const hexBytes = new Uint8Array(arg.length / 2);
      for (let i = 0; i < arg.length; i += 2) {
        hexBytes[i / 2] = parseInt(arg.substr(i, 2), 16);
      }
      args.push({ type: 'b', value: hexBytes });
    } else if (/^-?\d+$/.test(arg)) {
      args.push({ type: 'i', value: parseInt(arg, 10) });
    } else if (/^-?\d*\.\d+$/.test(arg)) {
      args.push({ type: 'f', value: parseFloat(arg) });
    } else {
      args.push({ type: 's', value: arg });
    }
  }

  return { address, args };
}

function formatBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes <= 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit++;
  }
  const precision = value < 10 && unit > 0 ? 2 : unit > 0 ? 1 : 0;
  return `${value.toFixed(precision)} ${units[unit]}`;
}

// Flash tab to indicate update
function flashTab(tabName) {
  const tabButton = document.querySelector(`[data-tab="${tabName}"]`);
  if (!tabButton || tabButton.classList.contains('active')) {
    return; // Don't flash if tab is active or doesn't exist
  }

  // Remove flash class if it exists (to restart animation)
  tabButton.classList.remove('flash');

  // Force reflow to restart animation
  void tabButton.offsetWidth;

  // Add flash class
  tabButton.classList.add('flash');

  // Remove flash class after animation completes
  setTimeout(() => {
    tabButton.classList.remove('flash');
  }, 1500);
}

// Helper functions
function updateStatus(status) {
  const initButtonContainer = document.getElementById('init-button-container');
  const scopeContainer = document.getElementById('scope-container');
  const synthUIContainer = document.getElementById('synth-ui-container');
  const clearButton = document.getElementById('clear-button');
  const loadAllButton = document.getElementById('load-all-button');
  const loadExampleButton = document.getElementById('load-example-button');

  // Update UI visibility based on status
  if (status === 'not_initialized' || status === 'error') {
    initButton.textContent = 'Boot';
    initButton.disabled = false;
    if (initButtonContainer) initButtonContainer.style.display = 'block';
    if (scopeContainer) scopeContainer.style.display = 'none';
    if (synthUIContainer) synthUIContainer.classList.remove('initialized');
    messageInput.disabled = true;
    messageForm.querySelector('button[type="submit"]').disabled = true;
    if (clearButton) clearButton.disabled = true;
    if (loadAllButton) loadAllButton.disabled = true;
    if (loadExampleButton) loadExampleButton.disabled = true;
  } else if (status === 'initializing') {
    initButton.textContent = 'Booting...';
    initButton.disabled = true;
  } else if (status === 'ready') {
    if (initButtonContainer) initButtonContainer.style.display = 'none';
    if (scopeContainer) scopeContainer.style.display = 'block';
    if (synthUIContainer) synthUIContainer.classList.add('initialized');
    if (restartContainer) restartContainer.style.display = 'block';
    if (resetButton) {
      resetButton.textContent = 'Restart';
      resetButton.disabled = false;
    }
    messageInput.disabled = false;
    messageForm.querySelector('button[type="submit"]').disabled = false;
    if (clearButton) clearButton.disabled = false;
    if (loadExampleButton) loadExampleButton.disabled = false;
  }
}

function showError(message) {
  errorText.textContent = message;
  errorDiv.classList.remove('hidden');
}

function hideError() {
  errorDiv.classList.add('hidden');
}

function updateMetrics(metrics) {
  // Main thread messages
  document.getElementById('metric-sent').textContent = metrics.main_messages_sent ?? 0;
  document.getElementById('metric-bytes-sent').textContent = formatBytes(metrics.main_bytes_sent ?? 0);
  document.getElementById('metric-direct-writes').textContent = metrics.prescheduler_bypassed ?? 0;
  document.getElementById('metric-received').textContent = metrics.osc_in_messages_received ?? 0;
  document.getElementById('metric-bytes-received').textContent = formatBytes(metrics.osc_in_bytes_received ?? 0);
  document.getElementById('metric-osc-in-dropped').textContent = metrics.osc_in_messages_dropped ?? 0;

  // Worklet processing
  document.getElementById('metric-messages-processed').textContent = metrics.worklet_messages_processed ?? 0;
  document.getElementById('metric-messages-dropped').textContent = metrics.worklet_messages_dropped ?? 0;
  document.getElementById('metric-sequence-gaps').textContent = metrics.worklet_sequence_gaps ?? 0;
  document.getElementById('metric-process-count').textContent = metrics.worklet_process_count ?? 0;

  // Worklet scheduler
  document.getElementById('metric-scheduler-depth').textContent = metrics.worklet_scheduler_depth ?? 0;
  document.getElementById('metric-scheduler-peak').textContent = metrics.worklet_scheduler_max ?? 0;
  document.getElementById('metric-scheduler-dropped').textContent = metrics.worklet_scheduler_dropped ?? 0;
  document.getElementById('metric-drift').textContent = (metrics.drift_offset_ms ?? 0) + 'ms';

  // PreScheduler
  document.getElementById('metric-prescheduler-pending').textContent = metrics.prescheduler_pending ?? 0;
  document.getElementById('metric-prescheduler-peak').textContent = metrics.prescheduler_peak ?? 0;
  document.getElementById('metric-prescheduler-sent').textContent = metrics.prescheduler_sent ?? 0;
  document.getElementById('metric-bundles-scheduled').textContent = metrics.prescheduler_bundles_scheduled ?? 0;
  document.getElementById('metric-events-cancelled').textContent = metrics.prescheduler_events_cancelled ?? 0;

  // Retries
  document.getElementById('metric-prescheduler-retries-succeeded').textContent = metrics.prescheduler_retries_succeeded ?? 0;
  document.getElementById('metric-prescheduler-retries-failed').textContent = metrics.prescheduler_retries_failed ?? 0;
  document.getElementById('metric-prescheduler-retry-queue-size').textContent = metrics.prescheduler_retry_queue_size ?? 0;
  document.getElementById('metric-prescheduler-retry-queue-max').textContent = metrics.prescheduler_retry_queue_max ?? 0;
  document.getElementById('metric-messages-retried').textContent = metrics.prescheduler_messages_retried ?? 0;
  document.getElementById('metric-total-dispatches').textContent = metrics.prescheduler_total_dispatches ?? 0;

  // Ring buffer usage
  const inUsage = metrics.in_buffer_usage ?? 0;
  const outUsage = metrics.out_buffer_usage ?? 0;
  const debugUsage = metrics.debug_buffer_usage ?? 0;
  document.getElementById('metric-in-usage').textContent = inUsage.toFixed(2) + '%';
  document.getElementById('metric-in-bar').style.width = inUsage + '%';
  document.getElementById('metric-out-usage').textContent = outUsage.toFixed(2) + '%';
  document.getElementById('metric-out-bar').style.width = outUsage + '%';
  document.getElementById('metric-debug-usage').textContent = debugUsage.toFixed(2) + '%';
  document.getElementById('metric-debug-bar').style.width = debugUsage + '%';

  // Debug worker
  document.getElementById('metric-debug-received').textContent = metrics.debug_messages_received ?? 0;
  document.getElementById('metric-debug-bytes-received').textContent = formatBytes(metrics.debug_bytes_received ?? 0);

  // Engine state
  document.getElementById('metric-audio-state').textContent = metrics.audio_context_state ?? '-';
  document.getElementById('metric-synthdefs').textContent = metrics.synthdef_count ?? 0;

  // Buffer pool
  document.getElementById('metric-buffer-used').textContent = formatBytes(metrics.buffer_pool_used ?? 0);
  document.getElementById('metric-buffer-free').textContent = formatBytes(metrics.buffer_pool_available ?? 0);
  document.getElementById('metric-buffer-allocs').textContent = metrics.buffer_pool_allocations ?? 0;
}

function addMessage(message) {
  messages.push(message);
  if (messages.length > 50) messages = messages.slice(-50);

  renderMessages();
}

function renderMessages() {
  // Flash tab if there are messages (don't flash for empty state)
  const hasMessages = messages.length > 0;

  if (messages.length === 0) {
    messageHistory.innerHTML = '<p class="message-empty">No messages received yet</p>';
  } else {
    messageHistory.innerHTML = messages.map(msg => {
      let content = '';

      // Decode OSC message
      if (msg.oscData) {
        try {
          const oscMsg = SuperSonic.osc.decode(msg.oscData);
          let args = '';

          if (Array.isArray(oscMsg.args)) {
            // Special handling for /s_new to color param names
            const isSnew = oscMsg.address === '/s_new';

            args = oscMsg.args.map((arg, index) => {
              let value = arg;
              let type = null;
              if (typeof arg === 'object' && arg.value !== undefined) {
                value = arg.value;
                type = arg.type;
              }

              // For /s_new: arg 0 is synthdef (keyword), args 4+ are param pairs
              const isParamName = isSnew && index >= 4 && (index - 4) % 2 === 0 && typeof value === 'string';
              const isSynthdefName = isSnew && index === 0 && typeof value === 'string';

              // Color by type
              if (type === 'b' || value instanceof Uint8Array || value instanceof ArrayBuffer ||
                  (Array.isArray(value) && value.length > 20)) {
                const size = value.length || value.byteLength || '?';
                return `<span class="osc-color-string">&lt;binary ${size} bytes&gt;</span>`;
              } else if (type === 'f' || (type === null && typeof value === 'number' && !Number.isInteger(value))) {
                return `<span class="osc-color-float">${value}</span>`;
              } else if (type === 'i' || (type === null && Number.isInteger(value))) {
                return `<span class="osc-color-int">${value}</span>`;
              } else if (isParamName) {
                return `<span class="osc-color-param">${value}</span>`;
              } else if (isSynthdefName) {
                return `<span class="osc-color-string">${value}</span>`;
              } else {
                return `<span class="osc-color-string">${value}</span>`;
              }
            }).join(' ');
          } else {
            args = oscMsg.args || '';
          }

          content = `<span class="osc-color-address">${oscMsg.address}</span>${args ? ' ' + args : ''}`;
        } catch (e) {
          console.error('[OSC Decode Error]', e, 'Type:', typeof e, 'Message:', e.message, 'Stack:', e.stack);
          content = `<span class="osc-color-error">Failed to decode OSC: ${e.message || e}</span>`;
        }
      } else {
        content = msg.text || 'Unknown message';
      }

      return `
        <div class="message-item">
          <span class="message-header">[${msg.sequence}]</span><span class="message-content">${content}</span>
        </div>
      `;
    }).join('');
  }

  // Auto-scroll to bottom (messageHistory has log-scroll-area class, so it's the scroll container)
  messageHistory.scrollTop = messageHistory.scrollHeight;

  // Flash tab to indicate update (only if there are messages)
  if (hasMessages) {
    flashTab('osc-in');
  }
}

/**
 * Parse the multiline OSC text area into scheduled and immediate commands.
 * Supports comments anywhere (# ...), optional timestamps, and plain text lines.
 */
function parseOscTextInput(rawText) {
  const scheduled = new Map(); // timestamp -> osc messages
  const immediate = [];
  const comments = [];
  const errors = [];

  const lines = rawText.split(/\r?\n/);
  let currentBundleTimestamp = null;

  for (const originalLine of lines) {
    const line = originalLine.trim();
    if (!line) continue;

    if (line.startsWith('#')) {
      comments.push(line);
      continue;
    }

    if (line.startsWith('-')) {
      if (currentBundleTimestamp === null) {
        errors.push('Bundle continuation (-) found before any timestamped bundle');
        continue;
      }
      const commandText = line.slice(1).trim();
      if (!commandText.startsWith('/')) {
        errors.push('Bundle continuation must contain a valid OSC command');
        continue;
      }
      try {
        const oscMessage = parseTextToOSC(commandText);
        scheduled.get(currentBundleTimestamp).push(oscMessage);
      } catch (err) {
        errors.push(err.message || `Failed to parse line: ${line}`);
      }
      continue;
    }

    const timestampMatch = line.match(/^(-?\d+(?:\.\d+)?)\s+(\/.*)$/);
    if (timestampMatch) {
      const timestamp = parseFloat(timestampMatch[1]);
      if (Number.isNaN(timestamp)) {
        errors.push(`Invalid timestamp: ${timestampMatch[1]}`);
        continue;
      }
      const commandText = timestampMatch[2];
      if (!commandText.startsWith('/')) {
        errors.push(`Invalid OSC command: ${commandText}`);
        continue;
      }
      try {
        const oscMessage = parseTextToOSC(commandText);
        if (!scheduled.has(timestamp)) {
          scheduled.set(timestamp, []);
        }
        scheduled.get(timestamp).push(oscMessage);
        currentBundleTimestamp = timestamp;
      } catch (err) {
        errors.push(err.message || `Failed to parse line: ${line}`);
      }
      continue;
    }

    if (line.startsWith('/')) {
      try {
        const oscMessage = parseTextToOSC(line);
        immediate.push(oscMessage);
      } catch (err) {
        errors.push(err.message || `Failed to parse line: ${line}`);
      }
      currentBundleTimestamp = null;
      continue;
    }

    errors.push(`Unrecognised line: ${line}`);
  }

  return { scheduled, immediate, comments, errors };
}

// Build HTML for a single message (extracted from renderSentMessages)
function buildMessageHTML(msg) {
  let content = '';

  // Check if it's a comment
  if (msg.comment) {
    content = `<span class="osc-color-comment">${msg.comment}</span>`;
  } else {
    // Decode OSC message or bundle
    try {
      const oscMsg = SuperSonic.osc.decode(msg.oscData);

      // Check if it's a bundle
      if (oscMsg.packets && Array.isArray(oscMsg.packets)) {
        // It's a bundle - show all messages
        const bundleContent = oscMsg.packets.map(packet => {
          let args = '';

          if (Array.isArray(packet.args)) {
            const isSnew = packet.address === '/s_new';

            args = packet.args.map((arg, index) => {
              let value = arg;
              let type = null;
              if (typeof arg === 'object' && arg.value !== undefined) {
                value = arg.value;
                type = arg.type;
              }

              const isParamName = isSnew && index >= 4 && (index - 4) % 2 === 0 && typeof value === 'string';
              const isSynthdefName = isSnew && index === 0 && typeof value === 'string';

              // Color by type
              if (type === 'b' || value instanceof Uint8Array || value instanceof ArrayBuffer ||
                  (Array.isArray(value) && value.length > 20)) {
                const size = value.length || value.byteLength || '?';
                return `<span class="osc-color-string">&lt;binary ${size} bytes&gt;</span>`;
              } else if (type === 'f' || (type === null && typeof value === 'number' && !Number.isInteger(value))) {
                return `<span class="osc-color-float">${value}</span>`;
              } else if (type === 'i' || (type === null && Number.isInteger(value))) {
                return `<span class="osc-color-int">${value}</span>`;
              } else if (isParamName) {
                return `<span class="osc-color-param">${value}</span>`;
              } else if (isSynthdefName) {
                return `<span class="osc-color-string">${value}</span>`;
              } else {
                return `<span class="osc-color-string">${value}</span>`;
              }
            }).join(' ');
          }

          return `<span class="osc-color-address">${packet.address}</span>${args ? ' ' + args : ''}`;
        }).join('<br>');
        content = `<span class="osc-color-string">Bundle (${oscMsg.packets.length})</span><br>${bundleContent}`;
      } else {
        // It's a single message
        let args = '';

        if (Array.isArray(oscMsg.args)) {
          const isSnew = oscMsg.address === '/s_new';

          args = oscMsg.args.map((arg, index) => {
            let value = arg;
            let type = null;
            if (typeof arg === 'object' && arg.value !== undefined) {
              value = arg.value;
              type = arg.type;
            }

            const isParamName = isSnew && index >= 4 && (index - 4) % 2 === 0 && typeof value === 'string';
            const isSynthdefName = isSnew && index === 0 && typeof value === 'string';

            // Color by type
            if (type === 'b' || value instanceof Uint8Array || value instanceof ArrayBuffer ||
                (Array.isArray(value) && value.length > 20)) {
              const size = value.length || value.byteLength || '?';
              return `<span class="osc-color-string">&lt;binary ${size} bytes&gt;</span>`;
            } else if (type === 'f' || (type === null && typeof value === 'number' && !Number.isInteger(value))) {
              return `<span class="osc-color-float">${value}</span>`;
            } else if (type === 'i' || (type === null && Number.isInteger(value))) {
              return `<span class="osc-color-int">${value}</span>`;
            } else if (isParamName) {
              return `<span class="osc-color-param">${value}</span>`;
            } else if (isSynthdefName) {
              return `<span class="osc-color-string">${value}</span>`;
            } else {
              return `<span class="osc-color-string">${value}</span>`;
            }
          }).join(' ');
        } else {
          args = oscMsg.args || '';
        }

        content = `<span class="osc-color-address">${oscMsg.address}</span>${args ? ' ' + args : ''}`;
      }
    } catch (e) {
      console.error('[OSC Decode Error]', e);
      content = `<span class="osc-color-error">Failed to decode OSC: ${e.message || e}</span>`;
    }
  }

  const time = new Date(msg.timestamp).toISOString().slice(11, 23);
  return `
    <div class="message-item">
      <span class="message-header">[${time}]</span><span class="message-content">${content}</span>
    </div>
  `;
}

function addSentMessage(oscData, comment = null) {
  if (!sentMessageHistory) return;  // Element may not exist

  const msg = { oscData, timestamp: Date.now(), comment };
  sentMessages.push(msg);

  // Build HTML for just this one new message
  const messageHtml = buildMessageHTML(msg);

  // Remove empty state message if present
  const emptyMessage = sentMessageHistory.querySelector('.message-empty');
  if (emptyMessage) {
    emptyMessage.remove();
  }

  // Append at the bottom
  sentMessageHistory.insertAdjacentHTML('beforeend', messageHtml);

  // Remove oldest message if over limit
  if (sentMessages.length > 50) {
    sentMessages.shift();
    sentMessageHistory.firstElementChild?.remove();
  }

  // Auto-scroll to bottom
  sentMessageHistory.scrollTop = sentMessageHistory.scrollHeight;

  // Flash tab to indicate update
  flashTab('osc-out');
}

function renderSentMessages() {
  if (!sentMessageHistory) return;  // Element may not exist on all pages

  // Full re-render using buildMessageHTML (used on initial load or full refresh)
  if (sentMessages.length === 0) {
    sentMessageHistory.innerHTML = '<p class="message-empty">No messages sent yet</p>';
  } else {
    sentMessageHistory.innerHTML = sentMessages.map(msg => buildMessageHTML(msg)).join('');
  }

  // Auto-scroll to bottom
  sentMessageHistory.scrollTop = sentMessageHistory.scrollHeight;

  // Flash tab to indicate update (only if there are messages)
  if (sentMessages.length > 0) {
    flashTab('osc-out');
  }
}

// Initialise button
initButton.addEventListener('click', async () => {
  try {
    updateStatus('initializing');
    hideError();

    orchestrator = new SuperSonic({
      workerBaseURL: 'dist/workers/',
      wasmBaseURL: 'dist/wasm/',
      sampleBaseURL: 'dist/samples/',
      synthdefBaseURL: 'dist/synthdefs/'
      // Optional: Override scsynth options (see js/scsynth_options.js for all options)
      // scsynthOptions: {
      //   numBuffers: 512,
      //   maxNodes: 256,
      //   realTimeMemorySize: 8192  // 8MB
      // }
    });

    // Set up event listeners
    orchestrator.on('ready', async (data) => {
      console.log('[App] System initialised', data);
      updateStatus('ready');

      // Expose to global scope for console access (development only)
      if (DEV_MODE) {
        window.orchestrator = orchestrator;
        console.log('[App] orchestrator exposed to window for dev console access');
      }

      // Preload all loop samples on boot
      await loadAllLoopSamples();
      console.log('[App] Loop samples preloaded on boot');

      // Initialize FX chain BEFORE enabling synth pad
      // This prevents race conditions where pad sends /n_set before nodes exist
      await initFXChain();
      console.log('[App] FX chain initialized on boot');

      // Enable synth controls
      const fillSynthBtns = document.querySelectorAll('.fill-synth-button');
      fillSynthBtns.forEach(btn => btn.disabled = false);

      // Enable the synth pad
      const synthPad = document.getElementById('synth-pad');
      if (synthPad) {
        synthPad.classList.remove('disabled');
        synthPad.classList.add('ready');
        console.log('[UI] Synth pad enabled');
      }

      // Initialise FX parameter display with default values
      const cutoffDisplay = document.getElementById('fx-cutoff-value');
      const mixDisplay = document.getElementById('fx-mix-value');
      if (cutoffDisplay) cutoffDisplay.textContent = '130.0';
      if (mixDisplay) mixDisplay.textContent = '0.30';

      // Display WASM version
      if (orchestrator.workletNode && orchestrator.workletNode.port) {
        orchestrator.workletNode.port.postMessage({ type: 'getVersion' });
      }

      // Setup scope visualiser
      setupScope();
    });

    orchestrator.on('message:raw', (message) => {
      addMessage(message);
      // Also log to console in development mode
      if (DEV_MODE && message.oscData) {
        try {
          const decoded = SuperSonic.osc.decode(message.oscData);
          const args = decoded.args ? decoded.args.map(a => a.value !== undefined ? a.value : a).join(' ') : '';
          console.log(`[OSC ←] ${decoded.address} ${args}`);
        } catch (e) {
          console.log(`[OSC ←] <decode error: ${e.message}>`);
        }
      }
    });

    orchestrator.on('message:sent', (oscData) => {
      addSentMessage(oscData);
      // Also log to console in development mode
      if (DEV_MODE) {
        try {
          const decoded = SuperSonic.osc.decode(oscData);
          // Truncate large binary arguments to avoid flooding console
          const args = decoded.args ? decoded.args.map(a => {
            const val = a.value !== undefined ? a.value : a;
            // If it's a Uint8Array (binary blob), truncate it
            if (val instanceof Uint8Array) {
              if (val.length > 64) {
                return `<binary ${val.length} bytes>`;
              }
              return `<binary ${val.length}b: ${Array.from(val.slice(0, 16)).join(',')}>`;
            }
            return val;
          }).join(' ') : '';
          const currentTime = orchestrator.audioContext ? orchestrator.audioContext.currentTime.toFixed(3) : 'N/A';
          const perfTime = performance.now().toFixed(2);
          console.log(`[OSC → t=${currentTime}s perf=${perfTime}ms] ${decoded.address} ${args}`);
        } catch (e) {
          // Bundles can't be easily decoded, skip logging individual messages
          // But we can extract the bundle timestamp
          const view = new DataView(oscData.buffer, oscData.byteOffset);
          if (oscData.length >= 16 && oscData[0] === 0x23) {
            const ntpSeconds = view.getUint32(8, false);
            const ntpFraction = view.getUint32(12, false);
            const ntpTime = ntpSeconds + ntpFraction / 0x100000000;
            const currentTime = orchestrator.audioContext ? orchestrator.audioContext.currentTime.toFixed(3) : 'N/A';
            const perfTime = performance.now().toFixed(2);
            console.log(`[OSC → t=${currentTime}s perf=${perfTime}ms] <bundle @ NTP ${ntpTime.toFixed(3)}>`);
          } else {
            console.log('[OSC →] <bundle>');
          }
        }
      }
    });

    orchestrator.on('metrics', (metrics) => {
      // Metrics are now consolidated from all sources (worklet, OSC, SuperSonic counters)
      // Map to snake_case and update UI
      const metricsWithUsage = {};

      // Define camelCase -> snake_case mapping
      const metricsMapping = {
        // Worklet metrics
        workletProcessCount: 'worklet_process_count',
        workletMessagesProcessed: 'worklet_messages_processed',
        workletMessagesDropped: 'worklet_messages_dropped',
        workletSchedulerDepth: 'worklet_scheduler_depth',
        workletSchedulerMax: 'worklet_scheduler_max',
        workletSchedulerDropped: 'worklet_scheduler_dropped',
        workletSequenceGaps: 'worklet_sequence_gaps',
        // Prescheduler metrics
        preschedulerPending: 'prescheduler_pending',
        preschedulerPeak: 'prescheduler_peak',
        preschedulerSent: 'prescheduler_sent',
        preschedulerRetriesSucceeded: 'prescheduler_retries_succeeded',
        preschedulerRetriesFailed: 'prescheduler_retries_failed',
        preschedulerRetryQueueSize: 'prescheduler_retry_queue_size',
        preschedulerRetryQueueMax: 'prescheduler_retry_queue_max',
        preschedulerBundlesScheduled: 'prescheduler_bundles_scheduled',
        preschedulerEventsCancelled: 'prescheduler_events_cancelled',
        preschedulerTotalDispatches: 'prescheduler_total_dispatches',
        preschedulerMessagesRetried: 'prescheduler_messages_retried',
        preschedulerBypassed: 'prescheduler_bypassed',
        // OSC In worker metrics
        oscInMessagesReceived: 'osc_in_messages_received',
        oscInMessagesDropped: 'osc_in_messages_dropped',
        oscInBytesReceived: 'osc_in_bytes_received',
        // Debug worker metrics
        debugMessagesReceived: 'debug_messages_received',
        debugBytesReceived: 'debug_bytes_received',
        // Main thread metrics
        mainMessagesSent: 'main_messages_sent',
        mainBytesSent: 'main_bytes_sent',
        // Timing
        driftOffsetMs: 'drift_offset_ms'
      };

      // Apply all simple mappings
      Object.entries(metricsMapping).forEach(([source, target]) => {
        if (metrics[source] !== undefined) {
          metricsWithUsage[target] = metrics[source];
        }
      });

      // Handle nested properties (buffer usage percentages)
      if (metrics.inBufferUsed?.percentage !== undefined) {
        metricsWithUsage.in_buffer_usage = metrics.inBufferUsed.percentage;
      }
      if (metrics.outBufferUsed?.percentage !== undefined) {
        metricsWithUsage.out_buffer_usage = metrics.outBufferUsed.percentage;
      }
      if (metrics.debugBufferUsed?.percentage !== undefined) {
        metricsWithUsage.debug_buffer_usage = metrics.debugBufferUsed.percentage;
      }

      // Buffer pool stats and engine state (from metrics)
      metricsWithUsage.buffer_pool_used = metrics.bufferPoolUsedBytes;
      metricsWithUsage.buffer_pool_available = metrics.bufferPoolAvailableBytes;
      metricsWithUsage.buffer_pool_allocations = metrics.bufferPoolAllocations;
      metricsWithUsage.synthdef_count = metrics.loadedSynthDefs;
      metricsWithUsage.audio_context_state = metrics.audioContextState;

      updateMetrics(metricsWithUsage);
    });

    orchestrator.on('error', (error) => {
      console.error('[App] Error:', error);
      showError(error.message);
      updateStatus('error');
    });

    orchestrator.on('debug', (msg) => {
      // Log to console in dev mode (easier to copy/paste full logs)
      if (DEV_MODE) {
        console.log(msg.text);
      }

      // Also log to UI debug panel
      debugLog.textContent += msg.text + '\n';
      if (window.debugAutoScroll !== false) {
        const scrollContainer = debugLog.parentElement;
        if (scrollContainer) {
          scrollContainer.scrollTop = scrollContainer.scrollHeight;
        }
      }
      flashTab('debug');
    });

    orchestrator.on('shutdown', () => {
      console.log('[App] Shutdown event received, resetting state flags');
      // Reset all state flags so they get re-initialized after reset
      fxChainInitialized = false;
      fxChainInitializing = null;
      fxSynthdefsLoaded = false;
      instrumentSynthdefsLoaded = false;
      kickSampleLoaded = false;
      amenSampleLoaded = false;
    });

    // AudioContext state change events
    orchestrator.on('audiocontext:suspended', () => {
      console.log('[App] AudioContext suspended');
      if (suspendedOverlay) {
        suspendedOverlay.style.display = 'flex';
      }
    });

    orchestrator.on('audiocontext:interrupted', () => {
      console.log('[App] AudioContext interrupted');
      if (suspendedOverlay) {
        suspendedOverlay.style.display = 'flex';
      }
    });

    orchestrator.on('audiocontext:resumed', () => {
      console.log('[App] AudioContext resumed');
      if (suspendedOverlay) {
        suspendedOverlay.style.display = 'none';
      }
    });

    /* DEMO_BUILD_CONFIG */ await orchestrator.init({ development: DEV_MODE });

    // Expose for debugging
    window.testOrchestrator = orchestrator;

  } catch (error) {
    console.error('[App] Initialization failed:', error);
    showError(error.message);
    updateStatus('error');
  }
});

// Reset button
if (resetButton) {
  resetButton.addEventListener('click', async () => {
    if (!orchestrator) return;
    try {
      resetButton.textContent = 'Restarting...';
      resetButton.disabled = true;

      // Log to debug panel
      debugLog.textContent += '\n[App] Rebooting...\n';

      // Stop all running loops/arpeggiators
      if (typeof stopArpeggiator === 'function') stopArpeggiator();
      if (typeof stopKickLoop === 'function') stopKickLoop();
      if (typeof stopAmenLoop === 'function') stopAmenLoop();

      // Disable synth pad to stop sending messages during reset
      const synthPad = document.getElementById('synth-pad');
      if (synthPad) {
        synthPad.classList.remove('ready');
        synthPad.classList.add('disabled');
      }

      await orchestrator.reset();
    } catch (error) {
      console.error('[App] Reset failed:', error);
      showError(error.message);
      resetButton.textContent = 'Restart';
      resetButton.disabled = false;
    }
  });
}

// Resume button (shown when audio is suspended)
if (resumeButton) {
  resumeButton.addEventListener('click', async () => {
    if (!orchestrator) return;
    try {
      resumeButton.textContent = 'Resuming...';
      resumeButton.disabled = true;

      // Log to debug panel
      debugLog.textContent += '\n[App] Resuming audio...\n';

      // Stop all running loops/arpeggiators
      if (typeof stopArpeggiator === 'function') stopArpeggiator();
      if (typeof stopKickLoop === 'function') stopKickLoop();
      if (typeof stopAmenLoop === 'function') stopAmenLoop();

      // Disable synth pad to stop sending messages during reset
      const synthPad = document.getElementById('synth-pad');
      if (synthPad) {
        synthPad.classList.remove('ready');
        synthPad.classList.add('disabled');
      }

      // Hide overlay immediately for better UX
      if (suspendedOverlay) {
        suspendedOverlay.style.display = 'none';
      }

      await orchestrator.reset();

      // Reset button state
      resumeButton.textContent = 'Resume Audio';
      resumeButton.disabled = false;
    } catch (error) {
      console.error('[App] Resume failed:', error);
      showError(error.message);
      resumeButton.textContent = 'Resume Audio';
      resumeButton.disabled = false;
      // Show overlay again if resume failed
      if (suspendedOverlay) {
        suspendedOverlay.style.display = 'flex';
      }
    }
  });
}

// Page visibility change handler - attempt audio recovery
document.addEventListener('visibilitychange', async () => {
  if (document.visibilityState !== 'visible' || !orchestrator) {
    return;
  }

  const resumed = await orchestrator.resume();
  if (!resumed && suspendedOverlay) {
    suspendedOverlay.style.display = 'flex';
  }
});

// Scope visualisation functions
function setupScope() {
  if (!orchestrator || !orchestrator.audioContext || !orchestrator.workletNode) return;

  // Create analyser node
  analyser = orchestrator.audioContext.createAnalyser();
  analyser.fftSize = 2048;
  analyser.smoothingTimeConstant = 0.8;

  // Connect: workletNode -> analyser -> destination
  orchestrator.workletNode.disconnect();
  orchestrator.workletNode.connect(analyser);
  analyser.connect(orchestrator.audioContext.destination);

  // Setup canvas
  const dpr = window.devicePixelRatio || 1;
  scopeCanvas.width = scopeCanvas.offsetWidth * dpr;
  scopeCanvas.height = scopeCanvas.offsetHeight * dpr;
  scopeCtx.scale(dpr, dpr);

  console.log('[Scope] Analyser connected');
  drawScope();
}

function drawScope() {
  scopeAnimationId = requestAnimationFrame(drawScope);

  if (!analyser) return;

  const bufferLength = analyser.fftSize;
  const dataArray = new Uint8Array(bufferLength);
  analyser.getByteTimeDomainData(dataArray);

  const width = scopeCanvas.offsetWidth;
  const height = scopeCanvas.offsetHeight;

  // Clear
  scopeCtx.fillStyle = '#000';
  scopeCtx.fillRect(0, 0, width, height);

  // Draw waveform
  scopeCtx.lineWidth = 3;
  scopeCtx.strokeStyle = '#ff6600';
  scopeCtx.beginPath();

  const sliceWidth = width / bufferLength;
  let x = 0;

  for (let i = 0; i < bufferLength; i++) {
    const v = dataArray[i] / 128.0;
    const y = (v * height) / 2;

    if (i === 0) {
      scopeCtx.moveTo(x, y);
    } else {
      scopeCtx.lineTo(x, y);
    }

    x += sliceWidth;
  }

  scopeCtx.lineTo(width, height / 2);
  scopeCtx.stroke();
}

function stopScope() {
  if (scopeAnimationId) {
    cancelAnimationFrame(scopeAnimationId);
    scopeAnimationId = null;
  }
  if (scopeCtx) {
    scopeCtx.fillStyle = '#000';
    scopeCtx.fillRect(0, 0, scopeCanvas.offsetWidth, scopeCanvas.offsetHeight);
  }
}

// Message form input handler removed - no character counter

messageForm.addEventListener('submit', async (e) => {
  e.preventDefault();
  const message = messageInput.value.trim();

  if (!message || !orchestrator) return;

  // Clear any previous errors
  hideError();

  try {
    const parsed = parseOscTextInput(message);

    if (parsed.errors.length > 0) {
      showError(parsed.errors[0]);
      return;
    }

    const runTag = `demo-run-${Date.now()}-${++runCounter}`;
    // Log comments immediately so the history shows them in order
    parsed.comments.forEach(comment => addSentMessage(null, comment));

    // Handle scheduled bundles if any timestamps were provided
    if (parsed.scheduled.size > 0) {
      // NTP epoch offset (seconds from 1900-01-01 to 1970-01-01)
      const NTP_EPOCH_OFFSET = 2208988800;

      // Get current system time in NTP
      const perfTimeMs = performance.timeOrigin + performance.now();
      const currentNTP = (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;

      console.log(`[APP] Bundle scheduling starting at currentNTP=${currentNTP.toFixed(6)}s`);

      function createOSCBundle(relativeTimeS, messages, isFirstBundle = false) {
        // Bundle timestamp = current system NTP + relative time + safety margin for processing overhead
        const ntpTimeS = currentNTP + relativeTimeS + 0.100;
        const ntpSeconds = Math.floor(ntpTimeS);
        const ntpFraction = Math.floor((ntpTimeS % 1) * 0x100000000);

        // DIAGNOSTIC LOGGING
        if (isFirstBundle) {
          const timetag64 = (BigInt(ntpSeconds) << 32n) | BigInt(ntpFraction);
          console.log(`[APP] FIRST BUNDLE CREATION:
  Current system NTP: ${currentNTP.toFixed(6)}s
  Relative time: ${relativeTimeS}s
  Safety margin: 0.100s
  Target NTP time: ${ntpTimeS.toFixed(6)}s
  Encoded seconds: ${ntpSeconds}
  Encoded fraction: ${ntpFraction}
  Combined timetag: ${timetag64}
  Delta (target - current): ${(ntpTimeS - currentNTP).toFixed(6)}s`);
        }

        const encodedMessages = messages.map(msg => SuperSonic.osc.encode(msg));

        let bundleSize = 8 + 8;
        encodedMessages.forEach(msg => {
          bundleSize += 4 + msg.byteLength;
        });

        const bundle = new Uint8Array(bundleSize);
        const view = new DataView(bundle.buffer);
        let offset = 0;

        bundle.set([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00], offset);
        offset += 8;
        view.setUint32(offset, ntpSeconds, false);
        offset += 4;
        view.setUint32(offset, ntpFraction, false);
        offset += 4;

        encodedMessages.forEach(msg => {
          view.setInt32(offset, msg.byteLength, false);
          offset += 4;
          bundle.set(msg, offset);
          offset += msg.byteLength;
        });

        return bundle;
      }

      const sortedTimes = Array.from(parsed.scheduled.keys()).sort((a, b) => a - b);
      const bundlePromises = [];

      for (let i = 0; i < sortedTimes.length; i++) {
        const timestamp = sortedTimes[i];
        const messagesAtTime = parsed.scheduled.get(timestamp);
        // Pass relative timestamp directly (safety margin added in createOSCBundle)
        const isFirstBundle = (i === 0);
        const bundle = createOSCBundle(timestamp, messagesAtTime, isFirstBundle);
        bundlePromises.push(orchestrator.sendOSC(bundle, { runTag }));
      }

      await Promise.all(bundlePromises);
    }

    // Immediate OSC commands
    for (const oscMessage of parsed.immediate) {
      const args = oscMessage.args.map(arg => arg.value);
      orchestrator.send(oscMessage.address, ...args);
    }

  } catch (error) {
    showError(error.message);
  }
});

// Auto-scroll enabled by default
window.debugAutoScroll = true;
window.debugLogWasm = false;

// Draggable divider functionality
const divider = document.getElementById('column-divider');
const leftColumn = document.querySelector('.left-column');
const rightColumn = document.querySelector('.right-column');

if (divider && leftColumn && rightColumn) {
  let isDragging = false;

  console.log('[Divider] Initialised', { divider, leftColumn, rightColumn });

  divider.addEventListener('mousedown', (e) => {
    e.preventDefault();
    isDragging = true;
    console.log('[Divider] Drag started');
    document.body.style.cursor = 'col-resize';
    document.body.style.userSelect = 'none';
  });

  document.addEventListener('mousemove', (e) => {
    if (!isDragging) return;

    const container = leftColumn.parentElement;
    const containerRect = container.getBoundingClientRect();
    const offsetX = e.clientX - containerRect.left;
    const containerWidth = containerRect.width;

    // Calculate percentage
    const leftPercent = (offsetX / containerWidth) * 100;

    // Constrain between 20% and 80%
    if (leftPercent >= 20 && leftPercent <= 80) {
      leftColumn.style.flex = `0 0 ${leftPercent}%`;
      rightColumn.style.flex = `0 0 ${100 - leftPercent}%`;
    }
  });

  document.addEventListener('mouseup', () => {
    if (isDragging) {
      console.log('[Divider] Drag ended');
      isDragging = false;
      document.body.style.cursor = '';
      document.body.style.userSelect = '';
    }
  });
} else {
  console.error('[Divider] Elements not found:', { divider, leftColumn, rightColumn });
}

// Main Tab functionality (Playground / OSC API)
const mainTabButtons = document.querySelectorAll('.main-tab-button');
const mainTabContents = document.querySelectorAll('.main-tab-content');

mainTabButtons.forEach(button => {
  button.addEventListener('click', () => {
    const tabName = button.dataset.mainTab;

    // Remove active class from all buttons and contents
    mainTabButtons.forEach(btn => btn.classList.remove('active'));
    mainTabContents.forEach(content => content.classList.remove('active'));

    // Add active class to clicked button and corresponding content
    button.classList.add('active');
    const targetContent = document.querySelector(`[data-main-tab-content="${tabName}"]`);
    if (targetContent) {
      targetContent.classList.add('active');
    }

    console.log('[UI] Switched to tab:', tabName);
  });
});

// Log tab functionality for mobile (OSC In/Out/Debug)
const tabButtons = document.querySelectorAll('.tab-button');
const tabContents = document.querySelectorAll('.tab-content');

tabButtons.forEach(button => {
  button.addEventListener('click', () => {
    const tabName = button.dataset.tab;

    // Remove active class from all buttons and contents
    tabButtons.forEach(btn => btn.classList.remove('active'));
    tabContents.forEach(content => content.classList.remove('active'));

    // Add active class to clicked button and corresponding content
    button.classList.add('active');
    const targetContent = document.querySelector(`[data-tab-content="${tabName}"]`);
    if (targetContent) {
      targetContent.classList.add('active');
    }
  });
});

// Fill Synth buttons - load binary synthdef directly
const fillSynthButtons = document.querySelectorAll('.fill-synth-button');
fillSynthButtons.forEach(button => {
  button.addEventListener('click', async () => {
    try {
      const synthName = button.getAttribute('data-synth');
      console.log('[App] Loading', synthName, 'synthdef...');
      await orchestrator.loadSynthDef(`../dist/extra/synthdefs/${synthName}.scsyndef`);
      console.log('[App] Loaded', synthName, 'synthdef');
    } catch (error) {
      console.error('[App] Fill synth error:', error);
      showError('Failed to load synthdef: ' + error.message);
    }
  });
});

// Clear button
const clearButton = document.getElementById('clear-button');
if (clearButton) {
  clearButton.addEventListener('click', () => {
    messageInput.value = '';
    console.log('[App] Cleared textarea');
  });
}

// Load Example button
const loadExampleButton = document.getElementById('load-example-button');
if (loadExampleButton) {
  loadExampleButton.addEventListener('click', async () => {
    try {
      // Load the synthdef required by the example
      console.log('[App] Loading sonic-pi-dsaw synthdef for example...');
      await orchestrator.loadSynthDefs(['sonic-pi-dsaw']);
      console.log('[App] Loaded sonic-pi-dsaw synthdef');

      const exampleCode = `# Cinematic DSaw Pad @ 120 BPM
0.0   /s_new sonic-pi-dsaw -1 0 0 note 52 amp 0.45 attack 0.4 release 3 detune 0.18 cutoff 95 pan -0.2
0.5   /s_new sonic-pi-dsaw -1 0 0 note 55 amp 0.40 attack 0.4 release 3 detune 0.22 cutoff 92 pan 0.2
1.0   /s_new sonic-pi-dsaw -1 0 0 note 59 amp 0.38 attack 0.4 release 3 detune 0.16 cutoff 90 pan -0.15
4.0   /s_new sonic-pi-dsaw -1 0 0 note 48 amp 0.45 attack 0.5 release 3 detune 0.2  cutoff 88 pan 0.15
8.0   /s_new sonic-pi-dsaw -1 0 0 note 47 amp 0.42 attack 0.5 release 4 detune 0.22 cutoff 86 pan -0.1
12.0  /s_new sonic-pi-dsaw -1 0 0 note 52 amp 0.45 attack 0.4 release 4 detune 0.18 cutoff 95 pan 0.1
12.0  /s_new sonic-pi-dsaw -1 0 0 note 28 amp 0.45 attack 0.4 release 5 detune 0.18 cutoff 85 pan 0.1`;

      messageInput.value = exampleCode;
      console.log('[App] Loaded example code into textarea');
    } catch (error) {
      console.error('[App] Load example error:', error);
      showError('Failed to load example synthdef: ' + error.message);
    }
  });
}

// ===== INTERACTIVE UI CONTROLS =====

// Synth Pad (Trackpad) Setup
const synthPad = document.getElementById('synth-pad');
const synthPadTouch = document.getElementById('synth-pad-touch');
const synthPadCrosshair = document.getElementById('synth-pad-crosshair');
const synthPadCrosshairH = document.getElementById('synth-pad-crosshair-h');
const synthPadCrosshairV = document.getElementById('synth-pad-crosshair-v');
const synthPadCoords = document.getElementById('synth-pad-coords');

if (synthPad) {
  // Initially disable the pad until system is booted
  synthPad.classList.add('disabled');

  let isPadActive = false;

  function updatePadPosition(clientX, clientY) {
    const rect = synthPad.getBoundingClientRect();
    const x = Math.max(0, Math.min(1, (clientX - rect.left) / rect.width));
    const y = Math.max(0, Math.min(1, 1 - (clientY - rect.top) / rect.height)); // Invert Y

    // Update state
    uiState.padX = x;
    uiState.padY = y;
    uiState.padActive = isPadActive;

    // Update visual elements
    const pixelX = x * rect.width;
    const pixelY = (1 - y) * rect.height;

    synthPadTouch.style.left = pixelX + 'px';
    synthPadTouch.style.top = pixelY + 'px';

    synthPadCrosshairH.style.top = pixelY + 'px';
    synthPadCrosshairV.style.left = pixelX + 'px';

    // Update X and Y display values
    const padXValue = document.getElementById('pad-x-value');
    const padYValue = document.getElementById('pad-y-value');
    if (padXValue) padXValue.textContent = x.toFixed(2);
    if (padYValue) padYValue.textContent = y.toFixed(2);

    // Update FX parameters if available
    if (typeof updateFXParameters === 'function') {
      updateFXParameters(x, y);
    }
  }

  function activatePad(clientX, clientY) {
    // Don't activate if the pad is disabled (not booted yet)
    if (synthPad.classList.contains('disabled')) {
      return;
    }

    isPadActive = true;
    synthPad.classList.add('active');
    synthPadTouch.classList.add('active');
    synthPadCrosshair.classList.add('active');
    updatePadPosition(clientX, clientY);

    // Start trail animation
    if (typeof startTrailAnimation === 'function') {
      startTrailAnimation();
    }

    // Start arpeggiator if available
    if (typeof startArpeggiator === 'function' && typeof arpeggiatorRunning !== 'undefined' && !arpeggiatorRunning) {
      startArpeggiator();
    }

    // Start kick if available
    if (typeof startKickLoop === 'function' && typeof kickRunning !== 'undefined' && !kickRunning) {
      startKickLoop();
    }

    // Start amen if available
    if (typeof startAmenLoop === 'function' && typeof amenRunning !== 'undefined' && !amenRunning) {
      startAmenLoop();
    }
  }

  function deactivatePad() {
    isPadActive = false;
    uiState.padActive = false;
    synthPad.classList.remove('active');
    synthPadTouch.classList.remove('active');
    synthPadCrosshair.classList.remove('active');
    console.log('[UI] Pad deactivated');

    // Stop trail animation
    if (typeof stopTrailAnimation === 'function') {
      stopTrailAnimation();
    }

    // Stop arpeggiator if available
    if (typeof stopArpeggiator === 'function' && typeof arpeggiatorRunning !== 'undefined' && arpeggiatorRunning) {
      stopArpeggiator();
    }

    // Stop kick if available
    if (typeof stopKickLoop === 'function' && typeof kickRunning !== 'undefined' && kickRunning) {
      stopKickLoop();
    }

    // Stop amen if available
    if (typeof stopAmenLoop === 'function' && typeof amenRunning !== 'undefined' && amenRunning) {
      stopAmenLoop();
    }
  }

  // Mouse events
  synthPad.addEventListener('mousedown', (e) => {
    e.preventDefault();
    activatePad(e.clientX, e.clientY);
  });

  document.addEventListener('mousemove', (e) => {
    if (isPadActive) {
      updatePadPosition(e.clientX, e.clientY);
    }
  });

  document.addEventListener('mouseup', () => {
    if (isPadActive) {
      deactivatePad();
    }
  });

  // Touch events
  synthPad.addEventListener('touchstart', (e) => {
    e.preventDefault();
    const touch = e.touches[0];
    activatePad(touch.clientX, touch.clientY);
  });

  synthPad.addEventListener('touchmove', (e) => {
    e.preventDefault();
    if (isPadActive && e.touches.length > 0) {
      const touch = e.touches[0];
      updatePadPosition(touch.clientX, touch.clientY);
    }
  });

  synthPad.addEventListener('touchend', (e) => {
    e.preventDefault();
    deactivatePad();
  });

  synthPad.addEventListener('touchcancel', (e) => {
    e.preventDefault();
    deactivatePad();
  });

  console.log('[UI] Synth pad initialised');
}

// Slider Controls Setup
function setupSlider(sliderId, valueId, stateKey, formatter = (v) => v) {
  const slider = document.getElementById(sliderId);
  const valueDisplay = document.getElementById(valueId);

  if (slider && valueDisplay) {
    slider.addEventListener('input', (e) => {
      const value = parseFloat(e.target.value);
      uiState[stateKey] = value;
      valueDisplay.textContent = formatter(value);
      console.log(`[UI] ${stateKey}:`, value);
    });

    console.log(`[UI] Slider ${stateKey} initialised`);
  }
}

// Synth selector
const synthSelect = document.getElementById('synth-select');
const synthValue = document.getElementById('synth-value');

if (synthSelect && synthValue) {
  synthSelect.addEventListener('change', (e) => {
    uiState.synth = e.target.value;
    synthValue.textContent = e.target.value;
    console.log('[UI] synth:', e.target.value);
  });

  console.log('[UI] Synth selector initialised');
}

// Loop sample selector
const loopSelect = document.getElementById('loop-select');
const loopValue = document.getElementById('loop-value');

if (loopSelect && loopValue) {
  loopSelect.addEventListener('change', (e) => {
    uiState.loopSample = e.target.value;
    // Update display to show just the name without "loop_" prefix
    const displayName = e.target.value.replace('loop_', '');
    loopValue.textContent = displayName;
    console.log('[UI] loop sample:', e.target.value);

    // Mark sample as not loaded so it will be reloaded with new selection
    amenSampleLoaded = false;
  });

  console.log('[UI] Loop selector initialised');
}

// Setup all sliders
setupSlider('bpm-slider', 'bpm-value', 'bpm', (v) => Math.round(v));
setupSlider('root-note-slider', 'root-note-value', 'rootNote', (v) => Math.round(v));
setupSlider('octaves-slider', 'octaves-value', 'octaves', (v) => Math.round(v));
setupSlider('attack-slider', 'attack-value', 'attack', (v) => v.toFixed(2));
setupSlider('release-slider', 'release-value', 'release', (v) => v.toFixed(2));

// Hue slider with custom handler to update CSS filter
const hueSlider = document.getElementById('hue-slider');
const synthUIContainer = document.getElementById('synth-ui-container');

if (hueSlider && synthUIContainer) {
  hueSlider.addEventListener('input', (e) => {
    const value = parseInt(e.target.value);
    synthUIContainer.style.filter = `hue-rotate(${value}deg) saturate(2)`;
  });
  console.log('[UI] Hue slider initialised');
}

// Random Arp toggle button
const randomArpToggle = document.getElementById('random-arp-toggle');

if (randomArpToggle) {
  randomArpToggle.addEventListener('click', () => {
    uiState.randomArp = !uiState.randomArp;
    randomArpToggle.setAttribute('data-active', uiState.randomArp.toString());
    randomArpToggle.textContent = uiState.randomArp ? 'On' : 'Off';
    console.log('[UI] randomArp:', uiState.randomArp);
  });

  console.log('[UI] Random Arp toggle initialised');
}

console.log('[UI] All interactive controls initialised');
console.log('[UI] Access current state via window.uiState');

// ===== ARPEGGIATOR & FX CHAIN =====

// Constants for FX chain and scheduling
const NTP_EPOCH_OFFSET = 2208988800;
const LOOKAHEAD = 0.1; // 100ms lookahead to prevent late messages
const GROUP_SOURCE = 100; // Legacy/general
const GROUP_ARP = 100; // Arpeggiator notes
const GROUP_FX = 101;
const GROUP_LOOPS = 102; // Loop samples
const GROUP_KICK = 103; // Kick drum
const FX_BUS_SYNTH_TO_LPF = 20;
const FX_BUS_LPF_TO_REVERB = 22;
const FX_BUS_OUTPUT = 0;
const FX_LPF_NODE = 2000;
const FX_REVERB_NODE = 2001;

// State
let fxChainInitialized = false;
let fxChainInitializing = null; // Promise to prevent concurrent initialization
let fxSynthdefsLoaded = false;
let instrumentSynthdefsLoaded = false;
let arpeggiatorRunning = false;
let arpeggiatorBeatCounter = 0;
let kickRunning = false;
let kickBeatCounter = 0;
let kickCurrentTime = null; // Current time threaded through for kick loop
let kickSampleLoaded = false;
let amenRunning = false;
let amenBeatCounter = 0;
let amenSampleLoaded = false;
let amenCurrentTime = null; // Current time threaded through for amen loop
let playbackStartNTP = null;
let currentPatternIndex = 0;
let beatPulseInterval = null;

/**
 * Load instrument synthdefs (called by startArpeggiator)
 */
async function loadInstrumentSynthdefs() {
  if (instrumentSynthdefsLoaded) return;

  const instrumentSynthdefs = [
    'sonic-pi-bass_foundation',
    'sonic-pi-beep',
    'sonic-pi-blade',
    'sonic-pi-bnoise',
    'sonic-pi-chipbass',
    'sonic-pi-chiplead',
    'sonic-pi-dark_ambience',
    'sonic-pi-dpulse',
    'sonic-pi-dsaw',
    'sonic-pi-dtri',
    'sonic-pi-fm',
    'sonic-pi-gabberkick',
    'sonic-pi-hollow',
    'sonic-pi-mod_dsaw',
    'sonic-pi-mod_fm',
    'sonic-pi-mod_pulse',
    'sonic-pi-mod_saw',
    'sonic-pi-mod_sine',
    'sonic-pi-mod_tri',
    'sonic-pi-noise',
    'sonic-pi-organ_tonewheel',
    'sonic-pi-pluck',
    'sonic-pi-pretty_bell',
    'sonic-pi-prophet',
    'sonic-pi-pulse',
    'sonic-pi-rhodey',
    'sonic-pi-rodeo',
    'sonic-pi-saw',
    'sonic-pi-square',
    'sonic-pi-subpulse',
    'sonic-pi-supersaw',
    'sonic-pi-tb303',
    'sonic-pi-tech_saws',
    'sonic-pi-tri',
    'sonic-pi-winwood_lead',
    'sonic-pi-zawa'
  ];

  console.log('[Arp] Loading instrument synthdefs:', instrumentSynthdefs);
  try {
    const results = await orchestrator.loadSynthDefs(instrumentSynthdefs);
    const successCount = Object.values(results).filter(r => r.success).length;
    console.log(`[Arp] Sent ${successCount}/${instrumentSynthdefs.length} instrument synthdef loads`);
    // Wait for scsynth to process them
    const syncId = Math.floor(Math.random() * 1000000);
    await orchestrator.sync(syncId);
    console.log(`[Arp] Loaded ${successCount}/${instrumentSynthdefs.length} instrument synthdefs`);
    instrumentSynthdefsLoaded = true;
  } catch (error) {
    console.error('[Arp] Failed to load instrument synthdefs:', error);
    throw error;
  }
}

/**
 * Load FX synthdefs (called by initFXChain)
 */
async function loadFXSynthdefs() {
  if (fxSynthdefsLoaded) return;

  const fxSynthdefs = ['sonic-pi-fx_lpf', 'sonic-pi-fx_reverb', 'sonic-pi-basic_stereo_player'];

  console.log('[FX] Loading FX synthdefs:', fxSynthdefs);
  try {
    const results = await orchestrator.loadSynthDefs(fxSynthdefs);
    const successCount = Object.values(results).filter(r => r.success).length;
    console.log(`[FX] Sent ${successCount}/${fxSynthdefs.length} FX synthdef loads`);
    fxSynthdefsLoaded = true;
  } catch (error) {
    console.error('[FX] Failed to load FX synthdefs:', error);
    throw error;
  }
}

/**
 * Initialise FX chain (LPF -> Reverb)
 * Uses promise-based guard to prevent concurrent initialization
 */
async function initFXChain() {
  if (!orchestrator) return;

  // If already initialized, return immediately
  if (fxChainInitialized) {
    console.log('[FX] FX chain already initialized');
    return;
  }

  // If currently initializing, wait for the existing initialization to complete
  if (fxChainInitializing) {
    console.log('[FX] Waiting for existing initialization to complete...');
    return await fxChainInitializing;
  }

  // Create the initialization promise
  fxChainInitializing = (async () => {
    console.log('[FX] Initializing FX chain...');

    try {
      await loadFXSynthdefs();

      // Create groups and synth nodes
      orchestrator.send('/g_new', GROUP_SOURCE, 0, 0);
      orchestrator.send('/g_new', GROUP_FX, 3, GROUP_SOURCE);
      orchestrator.send('/g_new', GROUP_LOOPS, 0, 0);
      orchestrator.send('/g_new', GROUP_KICK, 0, 0);
      orchestrator.send('/s_new', 'sonic-pi-fx_lpf', FX_LPF_NODE, 0, GROUP_FX,
        'in_bus', FX_BUS_SYNTH_TO_LPF, 'out_bus', FX_BUS_LPF_TO_REVERB, 'cutoff', 130.0, 'res', 0.5);
      orchestrator.send('/s_new', 'sonic-pi-fx_reverb', FX_REVERB_NODE, 3, FX_LPF_NODE,
        'in_bus', FX_BUS_LPF_TO_REVERB, 'out_bus', FX_BUS_OUTPUT, 'mix', 0.3, 'room', 0.6);

      // Wait for scsynth to process all synthdef loads and node creations
      const syncId = Math.floor(Math.random() * 1000000);
      await orchestrator.sync(syncId);
      console.log('[FX] All FX synthdefs and nodes synced');

      // Mark as initialized only after successful completion
      fxChainInitialized = true;
      console.log('[FX] FX chain initialised successfully');
    } catch (error) {
      console.error('[FX] Failed to initialise FX chain:', error);
      console.error('[FX] Make sure fx_lpf and fx_reverb synthdefs are loaded!');
      throw error; // Re-throw to indicate failure
    } finally {
      // Clear the initializing promise
      fxChainInitializing = null;
    }
  })();

  return await fxChainInitializing;
}

/**
 * Update FX parameters based on trackpad position
 */
function updateFXParameters(x, y) {
  // X axis = reverb mix (0 = dry, 1 = wet)
  const reverbMix = x;

  // Y axis = LPF cutoff (MIDI: 30 = low, 130 = high)
  const cutoff = 30 + y * 100;

  // Always update UI display
  const cutoffDisplay = document.getElementById('fx-cutoff-value');
  const mixDisplay = document.getElementById('fx-mix-value');
  if (cutoffDisplay) cutoffDisplay.textContent = cutoff.toFixed(1);
  if (mixDisplay) mixDisplay.textContent = reverbMix.toFixed(2);

  // Send to audio engine if initialized
  if (fxChainInitialized && orchestrator) {
    try {
      orchestrator.send('/n_set', FX_LPF_NODE, 'cutoff', cutoff);
      orchestrator.send('/n_set', FX_REVERB_NODE, 'mix', reverbMix);
    } catch (error) {
      console.warn('[FX] Error updating FX parameters:', error);
    }
  }
}

/**
 * Generate minor pentatonic scale from root note
 * Minor pentatonic intervals: 0, 3, 5, 7, 10, 12 (root, m3, P4, P5, m7, octave)
 */
function getMinorPentatonicScale(rootNote, octaves) {
  const intervals = [0, 3, 5, 7, 10];
  const scale = [];

  for (let oct = 0; oct < octaves; oct++) {
    for (let interval of intervals) {
      scale.push(rootNote + interval + (oct * 12));
    }
  }

  return scale;
}

/**
 * Start arpeggiator
 */
async function startArpeggiator() {
  if (!orchestrator) return;

  // Load instrument synthdefs first
  if (!instrumentSynthdefsLoaded) {
    await loadInstrumentSynthdefs();
  }

  // Initialise FX chain if needed
  if (!fxChainInitialized) {
    await initFXChain();
  }

  if (arpeggiatorRunning) return;
  arpeggiatorRunning = true;

  const NOTE_INTERVAL = 0.125 / getBpmScale(); // Scale timing by BPM
  const GRID_INTERVAL = 0.5 / getBpmScale();   // Snap to grid (scaled by BPM)

  // Get current time
  const perfTimeMs = performance.timeOrigin + performance.now();
  const currentNTP = (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;

  // Find the next 0.5s grid boundary
  const nextGridBoundary = Math.ceil(currentNTP / GRID_INTERVAL) * GRID_INTERVAL;

  // Initialise the global timeline to the next grid boundary (only if not set)
  if (playbackStartNTP === null) {
    playbackStartNTP = nextGridBoundary;
    arpeggiatorBeatCounter = 0;
    currentPatternIndex = 0;
  } else {
    // Calculate which beat we should start from relative to the global grid
    const elapsed = currentNTP - playbackStartNTP;
    arpeggiatorBeatCounter = Math.ceil(elapsed / NOTE_INTERVAL);
    currentPatternIndex = 0;
  }

  // Calculate delay until our start beat
  const startTime = playbackStartNTP + (arpeggiatorBeatCounter * NOTE_INTERVAL);
  const delayUntilStart = (startTime - currentNTP) * 1000; // Convert to ms

  console.log(`[Arp] Starting at beat ${arpeggiatorBeatCounter}, NTP=${startTime.toFixed(3)}s (delay: ${delayUntilStart.toFixed(1)}ms)`);

  // Wait until the start time before scheduling
  setTimeout(() => {
    if (arpeggiatorRunning) {
      scheduleArpeggiatorBatch();
    }
  }, Math.max(0, delayUntilStart));

  // Start beat pulse visual
  startBeatPulse();
}

/**
 * Schedule next batch of 4 notes
 */
function scheduleArpeggiatorBatch() {
  if (!arpeggiatorRunning) return;

  const NOTE_INTERVAL = 0.125 / getBpmScale(); // Scale timing by BPM
  const NOTES_PER_BATCH = 4;

  // Get current scale from uiState
  const scale = getMinorPentatonicScale(uiState.rootNote, uiState.octaves);
  const synthName = `sonic-pi-${uiState.synth}`;

  // Schedule 4 notes
  for (let i = 0; i < NOTES_PER_BATCH; i++) {
    const beatNumber = arpeggiatorBeatCounter + i;
    const targetNTP = playbackStartNTP + (beatNumber * NOTE_INTERVAL) + LOOKAHEAD;

    // Pick note: random or sequential
    let note;
    if (uiState.randomArp) {
      note = scale[Math.floor(Math.random() * scale.length)];
    } else {
      note = scale[currentPatternIndex % scale.length];
      currentPatternIndex++;
    }

    // Create OSC message
    const message = {
      address: '/s_new',
      args: [
        { type: 's', value: synthName },
        { type: 'i', value: -1 }, // Auto-assign node ID (we don't need to track)
        { type: 'i', value: 0 }, // addAction: add to head
        { type: 'i', value: GROUP_SOURCE },
        { type: 's', value: 'note' },
        { type: 'i', value: note },
        { type: 's', value: 'out_bus' },
        { type: 'i', value: FX_BUS_SYNTH_TO_LPF },
        { type: 's', value: 'amp' },
        { type: 'f', value: uiState.amplitude * 0.5 }, // Scale down to prevent clipping
        { type: 's', value: 'attack' },
        { type: 'f', value: uiState.attack },
        { type: 's', value: 'release' },
        { type: 'f', value: uiState.release }
      ]
    };

    // Create OSC bundle with NTP timestamp
    const ntpSeconds = Math.floor(targetNTP);
    const ntpFraction = Math.floor((targetNTP % 1) * 0x100000000);

    const encodedMessage = SuperSonic.osc.encode(message);
    const bundleSize = 8 + 8 + 4 + encodedMessage.byteLength;
    const bundle = new Uint8Array(bundleSize);
    const view = new DataView(bundle.buffer);
    let offset = 0;

    // Write "#bundle\0"
    bundle.set([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00], offset);
    offset += 8;

    // Write NTP timestamp
    view.setUint32(offset, ntpSeconds, false);
    offset += 4;
    view.setUint32(offset, ntpFraction, false);
    offset += 4;

    // Write message size
    view.setInt32(offset, encodedMessage.byteLength, false);
    offset += 4;

    // Write message data
    bundle.set(encodedMessage, offset);

    orchestrator.sendOSC(bundle);
  }

  // Advance counter
  arpeggiatorBeatCounter += NOTES_PER_BATCH;

  // Calculate when to schedule next batch (500ms before it's needed)
  const nextBatchStartTime = playbackStartNTP + (arpeggiatorBeatCounter * NOTE_INTERVAL);
  const perfTimeMs = performance.timeOrigin + performance.now();
  const currentNTP = (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;
  const timeUntilNextBatch = (nextBatchStartTime - currentNTP) * 1000;
  const scheduleDelay = Math.max(0, timeUntilNextBatch - 500);

  setTimeout(() => {
    if (arpeggiatorRunning) {
      scheduleArpeggiatorBatch();
    }
  }, scheduleDelay);
}

/**
 * Stop arpeggiator
 */
function stopArpeggiator() {
  arpeggiatorRunning = false;

  // Stop beat pulse visual
  stopBeatPulse();

  // Only reset playback time if kick and amen are also stopped
  if (!kickRunning && !amenRunning) {
    playbackStartNTP = null;
  }

  console.log('[Arp] Stopped');
}

/**
 * Start visual beat pulse on pad touch indicator
 */
function startBeatPulse() {
  const NOTE_INTERVAL = 0.125 / getBpmScale(); // Scale timing by BPM
  const touchElement = document.getElementById('synth-pad-touch');

  if (!touchElement) return;

  // Calculate time until next beat
  const perfTimeMs = performance.timeOrigin + performance.now();
  const currentNTP = (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;
  const timeSinceStart = currentNTP - playbackStartNTP;
  const nextBeatIn = NOTE_INTERVAL - (timeSinceStart % NOTE_INTERVAL);

  // Start pulsing at the next beat
  setTimeout(() => {
    triggerBeatPulse();
    beatPulseInterval = setInterval(triggerBeatPulse, NOTE_INTERVAL * 1000);
  }, nextBeatIn * 1000);
}

/**
 * Stop visual beat pulse
 */
function stopBeatPulse() {
  if (beatPulseInterval) {
    clearInterval(beatPulseInterval);
    beatPulseInterval = null;
  }
}

/**
 * Trigger one beat pulse animation
 */
function triggerBeatPulse() {
  const touchElement = document.getElementById('synth-pad-touch');
  if (!touchElement) return;

  // Remove class and force reflow to restart animation
  touchElement.classList.remove('beat-pulse');
  void touchElement.offsetWidth; // Force reflow
  touchElement.classList.add('beat-pulse');

  // Spawn trail particle
  spawnTrailParticle();
}

/**
 * Spawn a new trail particle at the current dot position
 */
function spawnTrailParticle(force = false) {
  if (!trailCanvas || !uiState.padActive) return;

  const x = uiState.padX * trailCanvas.width;
  const y = (1 - uiState.padY) * trailCanvas.height;

  // Spawn multiple small particles for smoother effect
  for (let i = 0; i < 3; i++) {
    const angle = Math.random() * Math.PI * 2;
    const speed = 0.3 + Math.random() * 0.5;

    trailParticles.push({
      x: x + (Math.random() - 0.5) * 10,
      y: y + (Math.random() - 0.5) * 10,
      vx: Math.cos(angle) * speed * 0.2,
      vy: Math.sin(angle) * speed * 0.2,
      life: 1.0,
      maxLife: 0.6 + Math.random() * 0.4, // Variable lifetime
      size: 8 + Math.random() * 8
    });
  }
}

/**
 * Update and render trail particles
 */
function updateTrail() {
  if (!trailCtx || !trailCanvas) return;

  const now = performance.now();

  // Check if we're in the release phase (continuing after pad release)
  const inRelease = !uiState.padActive && trailReleaseTime !== null;
  const releaseElapsed = inRelease ? (now - trailReleaseTime) / 1000 : 0;
  const releaseComplete = releaseElapsed > (uiState.release || 0.1);

  // Spawn particles while active OR during release period
  const shouldSpawn = uiState.padActive || (inRelease && !releaseComplete);

  if (shouldSpawn) {
    // Fade during release
    const spawnIntensity = inRelease ? Math.max(0, 1 - releaseElapsed / (uiState.release || 0.1)) : 1.0;

    // Reduce spawning during release
    if (Math.random() < spawnIntensity) {
      spawnTrailParticle();
    }
  }

  // If no particles and spawn is done, clear completely and stop
  if (trailParticles.length === 0 && !shouldSpawn) {
    trailCtx.clearRect(0, 0, trailCanvas.width, trailCanvas.height);
    trailAnimationId = null;
    trailReleaseTime = null;
    return;
  }

  // Clear with slight fade for smoother effect
  trailCtx.fillStyle = 'rgba(0, 0, 0, 0.08)';
  trailCtx.fillRect(0, 0, trailCanvas.width, trailCanvas.height);

  const dt = 16.67; // Assume 60fps

  // Update and draw particles
  trailCtx.globalCompositeOperation = 'lighter';

  trailParticles = trailParticles.filter(particle => {
    // Update particle
    particle.life -= dt / (particle.maxLife * 1000);

    if (particle.life <= 0) return false;

    // Move particle (slow drift)
    particle.x += particle.vx;
    particle.y += particle.vy;

    // Slow down over time
    particle.vx *= 0.98;
    particle.vy *= 0.98;

    // Draw particle
    const alpha = particle.life;
    const size = particle.size * (0.5 + particle.life * 0.5);

    const gradient = trailCtx.createRadialGradient(
      particle.x, particle.y, 0,
      particle.x, particle.y, size
    );

    gradient.addColorStop(0, `rgba(255, 215, 0, ${alpha * 0.9})`);
    gradient.addColorStop(0.5, `rgba(255, 160, 0, ${alpha * 0.7})`);
    gradient.addColorStop(0.8, `rgba(255, 102, 0, ${alpha * 0.4})`);
    gradient.addColorStop(1, 'rgba(255, 102, 0, 0)');

    trailCtx.fillStyle = gradient;
    trailCtx.beginPath();
    trailCtx.arc(particle.x, particle.y, size, 0, Math.PI * 2);
    trailCtx.fill();

    return true;
  });

  trailCtx.globalCompositeOperation = 'source-over';

  // Continue animation
  trailAnimationId = requestAnimationFrame(updateTrail);
}

/**
 * Start trail animation loop
 */
function startTrailAnimation() {
  trailReleaseTime = null; // Reset release time
  if (!trailAnimationId) {
    trailAnimationId = requestAnimationFrame(updateTrail);
  }
}

/**
 * Stop trail animation loop
 */
function stopTrailAnimation() {
  // Mark when release started to continue trail during audio release
  trailReleaseTime = performance.now();
  lastTrailX = null;
  lastTrailY = null;
  // Animation will continue during release period, then stop and clear
}

/**
 * Load kick sample
 */
async function loadKickSample() {
  if (kickSampleLoaded) return;

  console.log('[Kick] Loading bd_haus sample...');
  await orchestrator.send('/b_allocRead', 0, 'bd_haus.flac');
  kickSampleLoaded = true;
  console.log('[Kick] bd_haus sample loaded to buffer 0');
}

/**
 * Start kick drum loop
 */
async function startKickLoop() {
  if (!orchestrator) return;

  // Initialise FX chain if needed
  if (!fxChainInitialized) {
    await initFXChain();
  }

  // Load sample if needed
  if (!kickSampleLoaded) {
    await loadKickSample();
  }

  if (kickRunning) return;
  kickRunning = true;

  const KICK_INTERVAL = 0.5 / getBpmScale(); // Scale timing by BPM
  const GRID_INTERVAL = 0.5 / getBpmScale(); // Snap to grid (scaled by BPM)

  // Get current time
  const perfTimeMs = performance.timeOrigin + performance.now();
  const currentNTP = (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;

  // Find the next 0.5s grid boundary
  const nextGridBoundary = Math.ceil(currentNTP / GRID_INTERVAL) * GRID_INTERVAL;

  // Initialise the global timeline to the next grid boundary (only if not set)
  if (playbackStartNTP === null) {
    playbackStartNTP = nextGridBoundary;
    kickBeatCounter = 0;
    kickCurrentTime = playbackStartNTP;
  } else {
    // Calculate which beat we should start from relative to the global grid
    const elapsed = currentNTP - playbackStartNTP;
    kickBeatCounter = Math.ceil(elapsed / KICK_INTERVAL);
    kickCurrentTime = playbackStartNTP + (kickBeatCounter * KICK_INTERVAL);
  }

  // Calculate delay until our start beat
  const startTime = kickCurrentTime;
  const delayUntilStart = (startTime - currentNTP) * 1000; // Convert to ms

  console.log(`[Kick] Starting at beat ${kickBeatCounter}, NTP=${startTime.toFixed(3)}s (delay: ${delayUntilStart.toFixed(1)}ms)`);

  // Wait until the start time before scheduling
  setTimeout(() => {
    if (kickRunning) {
      scheduleKickBatch();
    }
  }, Math.max(0, delayUntilStart));
}

/**
 * Schedule next batch of kick drums
 */
function scheduleKickBatch() {
  if (!kickRunning) return;

  const KICK_INTERVAL = 0.5 / getBpmScale(); // Scale timing by BPM
  const KICKS_PER_BATCH = 4;

  // Schedule 4 kicks
  for (let i = 0; i < KICKS_PER_BATCH; i++) {
    // Thread time through - use current time and increment
    const targetNTP = kickCurrentTime + LOOKAHEAD;
    kickCurrentTime += KICK_INTERVAL;

    // Create OSC message for kick
    const message = {
      address: '/s_new',
      args: [
        { type: 's', value: 'sonic-pi-basic_stereo_player' },
        { type: 'i', value: -1 }, // Auto-assign node ID
        { type: 'i', value: 0 }, // addAction: add to head
        { type: 'i', value: GROUP_SOURCE },
        { type: 's', value: 'buf' },
        { type: 'i', value: 0 }, // buffer 0 (bd_haus)
        { type: 's', value: 'out_bus' },
        { type: 'i', value: FX_BUS_SYNTH_TO_LPF },
        { type: 's', value: 'amp' },
        { type: 'f', value: 0.3 }
      ]
    };

    // Create OSC bundle with NTP timestamp
    const ntpSeconds = Math.floor(targetNTP);
    const ntpFraction = Math.floor((targetNTP % 1) * 0x100000000);

    const encodedMessage = SuperSonic.osc.encode(message);
    const bundleSize = 8 + 8 + 4 + encodedMessage.byteLength;
    const bundle = new Uint8Array(bundleSize);
    const view = new DataView(bundle.buffer);
    let offset = 0;

    // Write "#bundle\0"
    bundle.set([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00], offset);
    offset += 8;

    // Write NTP timestamp
    view.setUint32(offset, ntpSeconds, false);
    offset += 4;
    view.setUint32(offset, ntpFraction, false);
    offset += 4;

    // Write message size
    view.setInt32(offset, encodedMessage.byteLength, false);
    offset += 4;

    // Write message data
    bundle.set(encodedMessage, offset);

    orchestrator.sendOSC(bundle);
  }

  // Advance counter
  kickBeatCounter += KICKS_PER_BATCH;

  // Calculate when to schedule next batch (500ms before the first event in next batch)
  const perfTimeMs = performance.timeOrigin + performance.now();
  const currentNTP = (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;
  const timeUntilNextBatch = (kickCurrentTime - currentNTP) * 1000;
  const scheduleDelay = Math.max(0, timeUntilNextBatch - 500);

  setTimeout(() => {
    if (kickRunning) {
      scheduleKickBatch();
    }
  }, scheduleDelay);
}

/**
 * Stop kick drum loop
 */
function stopKickLoop() {
  kickRunning = false;
  kickCurrentTime = null;

  // Only reset playback time if arpeggiator and amen are also stopped
  if (!arpeggiatorRunning && !amenRunning) {
    playbackStartNTP = null;
  }

  console.log('[Kick] Stopped');
}

/**
 * Load all loop samples into buffers
 */
async function loadAllLoopSamples() {
  if (amenSampleLoaded) return;
  if (!orchestrator) return;

  console.log('[Samples] Loading all loop samples...');
  const loopNames = Object.keys(LOOP_BUFFERS);

  // Load all samples in parallel
  await Promise.all(loopNames.map(loopName => {
    const bufNum = LOOP_BUFFERS[loopName];
    const fileName = `${loopName}.flac`;
    console.log(`[Samples] Loading ${fileName} to buffer ${bufNum}`);
    return orchestrator.send('/b_allocRead', bufNum, fileName);
  }));

  amenSampleLoaded = true;
  console.log(`[Samples] Loaded ${loopNames.length} loop samples`);
}

/**
 * Start amen break loop
 */
async function startAmenLoop() {
  if (!orchestrator) return;

  // Initialise FX chain if needed
  if (!fxChainInitialized) {
    await initFXChain();
  }

  // Load samples if needed
  if (!amenSampleLoaded) {
    await loadAllLoopSamples();
  }

  if (amenRunning) return;
  amenRunning = true;

  // Get the config for the currently selected loop sample to calculate scaled interval
  const loopConfig = LOOP_CONFIG[uiState.loopSample] || { rate: 1.0, duration: 2.0 };
  const LOOP_INTERVAL = loopConfig.duration / getBpmScale(); // Scale timing by BPM
  const GRID_INTERVAL = 0.5 / getBpmScale(); // Snap to grid (scaled by BPM)

  // Get current time
  const perfTimeMs = performance.timeOrigin + performance.now();
  const currentNTP = (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;

  // Find the next 0.5s grid boundary
  const nextGridBoundary = Math.ceil(currentNTP / GRID_INTERVAL) * GRID_INTERVAL;

  // Initialise the global timeline to the next grid boundary (only if not set)
  if (playbackStartNTP === null) {
    playbackStartNTP = nextGridBoundary;
    amenBeatCounter = 0;
    amenCurrentTime = playbackStartNTP;
  } else {
    // Calculate which beat we should start from relative to the global grid
    const elapsed = currentNTP - playbackStartNTP;
    amenBeatCounter = Math.ceil(elapsed / LOOP_INTERVAL);
    amenCurrentTime = playbackStartNTP + (amenBeatCounter * LOOP_INTERVAL);
  }

  // Calculate delay until our start beat
  const startTime = amenCurrentTime;
  const delayUntilStart = (startTime - currentNTP) * 1000; // Convert to ms

  console.log(`[Amen] Starting at beat ${amenBeatCounter}, NTP=${startTime.toFixed(3)}s (delay: ${delayUntilStart.toFixed(1)}ms)`);

  // Wait until the start time before scheduling
  setTimeout(() => {
    if (amenRunning) {
      scheduleAmenBatch();
    }
  }, Math.max(0, delayUntilStart));
}

/**
 * Schedule next batch of amen breaks
 */
function scheduleAmenBatch() {
  if (!amenRunning || !orchestrator) return;

  // Get the config for the currently selected loop sample
  const loopConfig = LOOP_CONFIG[uiState.loopSample] || { rate: 1.0, duration: 2.0 };
  const bpmScale = getBpmScale();
  const loopRate = loopConfig.rate * bpmScale; // Scale rate by BPM (faster BPM = faster playback)
  const loopInterval = loopConfig.duration / bpmScale; // Scale timing by BPM
  const LOOPS_PER_BATCH = 1; // Schedule 1 at a time for faster stop

  console.log(`[Amen] Scheduling batch starting at beat ${amenBeatCounter}`);

  for (let i = 0; i < LOOPS_PER_BATCH; i++) {
    // Thread time through - use current time and increment
    const targetNTP = amenCurrentTime + LOOKAHEAD;
    amenCurrentTime += loopInterval;

    // Create OSC message for loop sample
    const message = {
      address: '/s_new',
      args: [
        { type: 's', value: 'sonic-pi-basic_stereo_player' },
        { type: 'i', value: -1 },
        { type: 'i', value: 0 },
        { type: 'i', value: GROUP_LOOPS },
        { type: 's', value: 'buf' },
        { type: 'i', value: LOOP_BUFFERS[uiState.loopSample] || 1 },
        { type: 's', value: 'amp' },
        { type: 'f', value: 0.5 },
        { type: 's', value: 'rate' },
        { type: 'f', value: loopRate },
        { type: 's', value: 'out_bus' },
        { type: 'i', value: FX_BUS_SYNTH_TO_LPF }
      ]
    };

    const encodedMessage = SuperSonic.osc.encode(message);

    // Build bundle manually with NTP timestamp
    const bundleSize = 8 + 8 + 4 + encodedMessage.byteLength;
    const bundle = new Uint8Array(bundleSize);
    const view = new DataView(bundle.buffer);

    let offset = 0;

    // Write bundle tag "#bundle\0"
    bundle.set([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00], offset);
    offset += 8;

    // Write NTP timestamp
    const ntpSeconds = Math.floor(targetNTP);
    const ntpFraction = Math.floor((targetNTP % 1) * 0x100000000);

    view.setUint32(offset, ntpSeconds, false);
    offset += 4;
    view.setUint32(offset, ntpFraction, false);
    offset += 4;

    // Write message size
    view.setInt32(offset, encodedMessage.byteLength, false);
    offset += 4;

    // Write message data
    bundle.set(encodedMessage, offset);

    orchestrator.sendOSC(bundle);
  }

  // Advance counter
  amenBeatCounter += LOOPS_PER_BATCH;

  // Calculate when to schedule next batch (500ms before the first event in next batch)
  const perfTimeMs = performance.timeOrigin + performance.now();
  const currentNTP = (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;
  const timeUntilNextBatch = (amenCurrentTime - currentNTP) * 1000;
  const scheduleDelay = Math.max(0, timeUntilNextBatch - 500);

  setTimeout(() => {
    if (amenRunning) {
      scheduleAmenBatch();
    }
  }, scheduleDelay);
}

/**
 * Stop amen break loop
 */
function stopAmenLoop() {
  amenRunning = false;
  amenCurrentTime = null;

  // Free all loop samples immediately to prevent 8s tail
  if (orchestrator) {
    orchestrator.send('/g_freeAll', GROUP_LOOPS);
    console.log('[Amen] Freed all loop samples');
  }

  // Only reset playback time if arpeggiator and kick are also stopped
  if (!arpeggiatorRunning && !kickRunning) {
    playbackStartNTP = null;
  }

  console.log('[Amen] Stopped');
}

// Expose functions globally for debugging and event handlers
window.startArpeggiator = startArpeggiator;
window.stopArpeggiator = stopArpeggiator;
window.startKickLoop = startKickLoop;
window.stopKickLoop = stopKickLoop;
window.startAmenLoop = startAmenLoop;
window.stopAmenLoop = stopAmenLoop;
window.arpeggiatorRunning = arpeggiatorRunning;
window.kickRunning = kickRunning;
window.amenRunning = amenRunning;

console.log('[Arp] Arpeggiator, kick drum, and amen break system initialised');
