
import { SuperSonic } from '../dist/supersonic.js';

let orchestrator = null;
let messagesSent = 0;
let messagesReceived = 0;
let messages = [];
let sentMessages = [];

// Scope visualiser
let analyser = null;
let scopeCanvas = null;
let scopeCtx = null;
let scopeAnimationId = null;

// DOM elements
const initButton = document.getElementById('init-button');
// const resetButton = document.getElementById('reset-button');
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
  const loadSynthdefsButton = document.getElementById('load-synthdefs-button');
  const loadSamplesButton = document.getElementById('load-samples-button');
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
    messageInput.disabled = false;
    messageForm.querySelector('button[type="submit"]').disabled = false;
    if (clearButton) clearButton.disabled = false;
    if (loadSynthdefsButton) loadSynthdefsButton.disabled = false;
    if (loadSamplesButton) loadSamplesButton.disabled = false;
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
  // Message stats
  if (metrics.messages_sent !== undefined) {
    document.getElementById('metric-sent').textContent = metrics.messages_sent;
  }
  if (metrics.messages_received !== undefined) {
    document.getElementById('metric-received').textContent = metrics.messages_received;
    messagesReceived = metrics.messages_received;
  }
  if (metrics.messages_dropped !== undefined || metrics.dropped_messages !== undefined) {
    const dropped = metrics.messages_dropped || metrics.dropped_messages || 0;
    document.getElementById('metric-dropped').textContent = dropped;
  }

  // WASM stats
  if (metrics.process_count !== undefined) {
    document.getElementById('metric-process-count').textContent = metrics.process_count;
  }
  if (metrics.buffer_overruns !== undefined) {
    document.getElementById('metric-overruns').textContent = metrics.buffer_overruns;
  }

  // Buffer usage
  if (metrics.in_buffer_usage !== undefined) {
    document.getElementById('metric-in-usage').textContent = metrics.in_buffer_usage + '%';
    document.getElementById('metric-in-bar').style.width = metrics.in_buffer_usage + '%';
  }
  if (metrics.out_buffer_usage !== undefined) {
    document.getElementById('metric-out-usage').textContent = metrics.out_buffer_usage + '%';
    document.getElementById('metric-out-bar').style.width = metrics.out_buffer_usage + '%';
  }
  if (metrics.debug_buffer_usage !== undefined) {
    document.getElementById('metric-debug-usage').textContent = metrics.debug_buffer_usage + '%';
    document.getElementById('metric-debug-bar').style.width = metrics.debug_buffer_usage + '%';
  }

  if (metrics.buffer_allocated_count !== undefined) {
    document.getElementById('metric-buffer-count').textContent = metrics.buffer_allocated_count;
  }
  if (metrics.buffer_pending !== undefined) {
    document.getElementById('metric-buffer-pending').textContent = metrics.buffer_pending;
  }
  if (metrics.buffer_bytes_active !== undefined) {
    document.getElementById('metric-buffer-used').textContent = formatBytes(metrics.buffer_bytes_active);
  }
  if (metrics.buffer_pool_total !== undefined) {
    document.getElementById('metric-buffer-total').textContent = formatBytes(metrics.buffer_pool_total);
  }
  if (metrics.buffer_pool_available !== undefined) {
    document.getElementById('metric-buffer-free').textContent = formatBytes(metrics.buffer_pool_available);
  }
  if (metrics.synthdef_count !== undefined) {
    document.getElementById('metric-synthdefs').textContent = metrics.synthdef_count;
  }

  // Scheduler metrics
  const schedulerDepth = metrics.scheduler_queue_depth ?? metrics.schedulerQueueDepth;
  if (schedulerDepth !== undefined) {
    document.getElementById('metric-scheduler-depth').textContent = schedulerDepth;
  }
  const schedulerPeak = metrics.scheduler_queue_max ?? metrics.schedulerQueueMax;
  if (schedulerPeak !== undefined) {
    document.getElementById('metric-scheduler-peak').textContent = schedulerPeak;
  }
  const schedulerDropped = metrics.scheduler_queue_dropped ?? metrics.schedulerQueueDropped;
  if (schedulerDropped !== undefined) {
    document.getElementById('metric-scheduler-dropped').textContent = schedulerDropped;
  }
}

function addMessage(message) {
  messages.unshift(message);
  if (messages.length > 50) messages = messages.slice(0, 50);

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

function addSentMessage(oscData, comment = null) {
  sentMessages.unshift({ oscData, timestamp: Date.now(), comment });
  if (sentMessages.length > 50) sentMessages = sentMessages.slice(0, 50);

  renderSentMessages();
}

function renderSentMessages() {
  if (!sentMessageHistory) return;  // Element may not exist on all pages

  // Flash tab if there are messages (don't flash for empty state)
  const hasMessages = sentMessages.length > 0;

  if (sentMessages.length === 0) {
    sentMessageHistory.innerHTML = '<p class="message-empty">No messages sent yet</p>';
  } else {
    sentMessageHistory.innerHTML = sentMessages.map(msg => {
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

      const time = new Date(msg.timestamp).toLocaleTimeString();
      return `
        <div class="message-item">
          <span class="message-header">[${time}]</span><span class="message-content">${content}</span>
        </div>
      `;
    }).join('');
  }

  // Auto-scroll to bottom (sentMessageHistory has log-scroll-area class, so it's the scroll container)
  sentMessageHistory.scrollTop = sentMessageHistory.scrollHeight;

  // Flash tab to indicate update (only if there are messages)
  if (hasMessages) {
    flashTab('osc-out');
  }
}

// Initialize button
initButton.addEventListener('click', async () => {
  try {
    updateStatus('initializing');
    hideError();

    orchestrator = new SuperSonic({
      sampleBaseURL: 'dist/samples/',
      synthdefBaseURL: 'dist/synthdefs/'
    });

    // Set up callbacks
    orchestrator.onInitialized = (data) => {
      console.log('[App] System initialized', data);
      updateStatus('ready');

      // Expose to global scope for console access (development only)
      if (orchestrator.config.development) {
        window.orchestrator = orchestrator;
        console.log('[App] orchestrator exposed to window for dev console access');
      }

      // Enable synth controls
      const fillSynthBtns = document.querySelectorAll('.fill-synth-button');
      fillSynthBtns.forEach(btn => btn.disabled = false);
      const loadSynthdefsBtn = document.getElementById('load-synthdefs-button');
      if (loadSynthdefsBtn) loadSynthdefsBtn.disabled = false;
      const loadSamplesBtn = document.getElementById('load-samples-button');
      if (loadSamplesBtn) loadSamplesBtn.disabled = false;

      // Display WASM version
      if (orchestrator.workletNode && orchestrator.workletNode.port) {
        orchestrator.workletNode.port.postMessage({ type: 'getVersion' });
      }

      // Setup scope visualiser
      setupScope();
    };

    orchestrator.onOSC = (message) => {
      addMessage(message);
      // Also log to console in development mode
      if (orchestrator.config.development && message.oscData) {
        try {
          const decoded = SuperSonic.osc.decode(message.oscData);
          const args = decoded.args ? decoded.args.map(a => a.value !== undefined ? a.value : a).join(' ') : '';
          console.log(`[OSC ←] ${decoded.address} ${args}`);
        } catch (e) {
          console.log(`[OSC ←] <decode error: ${e.message}>`);
        }
      }
    };

    orchestrator.onMessageSent = (oscData) => {
      addSentMessage(oscData);
      // Also log to console in development mode
      if (orchestrator.config.development) {
        try {
          const decoded = SuperSonic.osc.decode(oscData);
          const args = decoded.args ? decoded.args.map(a => a.value !== undefined ? a.value : a).join(' ') : '';
          console.log(`[OSC →] ${decoded.address} ${args}`);
        } catch (e) {
          // Bundles can't be easily decoded, skip
          console.log('[OSC →] <bundle>');
        }
      }
    };

    orchestrator.onMetricsUpdate = (metrics) => {
      // Map to snake_case and update UI
      // Only include fields that are actually present (don't default to 0)
      const metricsWithUsage = {};

      if (metrics.processCount !== undefined || metrics.process_count !== undefined) {
        metricsWithUsage.process_count = metrics.processCount || metrics.process_count;
      }
      if (metrics.maxProcessTime !== undefined || metrics.max_process_time_us !== undefined) {
        metricsWithUsage.max_process_time_us = metrics.maxProcessTime || metrics.max_process_time_us;
      }
      if (metrics.bufferOverruns !== undefined || metrics.buffer_overruns !== undefined) {
        metricsWithUsage.buffer_overruns = metrics.bufferOverruns || metrics.buffer_overruns;
      }
      if (metrics.messagesDropped !== undefined || metrics.dropped_messages !== undefined) {
        metricsWithUsage.messages_dropped = metrics.messagesDropped || metrics.dropped_messages;
      }
      // Messages received is tracked by the orchestrator, not WASM
      metricsWithUsage.messages_received = orchestrator.stats.messagesReceived || 0;
      if (metrics.inBufferUsed?.percentage !== undefined) {
        metricsWithUsage.in_buffer_usage = metrics.inBufferUsed.percentage;
      }
      if (metrics.outBufferUsed?.percentage !== undefined) {
        metricsWithUsage.out_buffer_usage = metrics.outBufferUsed.percentage;
      }
      if (metrics.debugBufferUsed?.percentage !== undefined) {
        metricsWithUsage.debug_buffer_usage = metrics.debugBufferUsed.percentage;
      }
      if (metrics.pollInterval !== undefined) {
        metricsWithUsage.poll_interval = metrics.pollInterval;
      }
      if (metrics.schedulerQueueDepth !== undefined || metrics.scheduler_queue_depth !== undefined) {
        metricsWithUsage.scheduler_queue_depth = metrics.schedulerQueueDepth ?? metrics.scheduler_queue_depth;
      }
      if (metrics.schedulerQueueMax !== undefined || metrics.scheduler_queue_max !== undefined) {
        metricsWithUsage.scheduler_queue_max = metrics.schedulerQueueMax ?? metrics.scheduler_queue_max;
      }
      if (metrics.schedulerQueueDropped !== undefined || metrics.scheduler_queue_dropped !== undefined) {
        metricsWithUsage.scheduler_queue_dropped = metrics.schedulerQueueDropped ?? metrics.scheduler_queue_dropped;
      }

      metricsWithUsage.messages_sent = orchestrator.stats.messagesSent || 0;

      if (typeof orchestrator.getDiagnostics === 'function') {
        try {
          const diagnostics = orchestrator.getDiagnostics();
          if (diagnostics?.buffers) {
            metricsWithUsage.buffer_allocated_count = diagnostics.buffers.active;
            metricsWithUsage.buffer_pending = diagnostics.buffers.pending;
            metricsWithUsage.buffer_bytes_active = diagnostics.buffers.bytesActive;
            if (diagnostics.buffers.pool) {
              metricsWithUsage.buffer_pool_total = diagnostics.buffers.pool.total;
              metricsWithUsage.buffer_pool_available = diagnostics.buffers.pool.available;
            }
          }
          if (diagnostics?.synthdefs) {
            metricsWithUsage.synthdef_count = diagnostics.synthdefs.count;
          }
        } catch (diagError) {
          console.warn('[App] Diagnostics fetch failed', diagError);
        }
      }

      updateMetrics(metricsWithUsage);
    };

    orchestrator.onError = (error) => {
      console.error('[App] Error:', error);
      showError(error.message);
      updateStatus('error');
    };

    orchestrator.onDebugMessage = (msg) => {
      debugLog.textContent += msg.text + '\n';
      if (window.debugAutoScroll !== false) {
        const scrollContainer = debugLog.parentElement;
        if (scrollContainer) {
          scrollContainer.scrollTop = scrollContainer.scrollHeight;
        }
      }
      flashTab('debug');
    };

    orchestrator.onSendError = (error) => {
      if (error.backpressure) {
        showError('Buffer full - try again in a moment');
      } else if (error.maxSize) {
        showError(`Message too large (max ${error.maxSize} characters)`);
      } else {
        showError(error.error || 'Failed to send message');
      }
    };

    // Listen for console messages from worklet
    orchestrator.onConsoleMessage = (msg) => {
      console.log('[AudioWorklet]', msg);
    };

    orchestrator.onVersion = (version) => {
      console.log('[App]', version);
      // Version display removed from UI
    };

    // Check capabilities and initialize
    await orchestrator.checkCapabilities();
    await orchestrator.init({ development: true });

    // Expose for debugging
    window.testOrchestrator = orchestrator;

  } catch (error) {
    console.error('[App] Initialization failed:', error);
    showError(error.message);
    updateStatus('error');
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

    // Log comments immediately so the history shows them in order
    parsed.comments.forEach(comment => addSentMessage(null, comment));

    let sendsThisRound = 0;

    // Handle scheduled bundles if any timestamps were provided
    if (parsed.scheduled.size > 0) {
      // Ensure AudioContext ↔ NTP offset is ready before encoding bundles
      let wasmOffset;
      try {
        wasmOffset = await orchestrator.waitForTimeSync();
      } catch (err) {
        showError(`Failed to synchronise clocks: ${err.message || err}`);
        return;
      }

      const now = orchestrator.audioContext.currentTime;

      function createOSCBundle(audioTimeS, messages) {
        const ntpTimeS = audioTimeS + wasmOffset;
        const ntpSeconds = Math.floor(ntpTimeS);
        const ntpFraction = Math.floor((ntpTimeS % 1) * 0x100000000);
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

      for (const timestamp of sortedTimes) {
        const messagesAtTime = parsed.scheduled.get(timestamp);
        const targetTimeS = now + timestamp + 0.02; // small safety margin
        const bundle = createOSCBundle(targetTimeS, messagesAtTime);
        bundlePromises.push(orchestrator.sendOSC(bundle));
        sendsThisRound += messagesAtTime.length;
      }

      await Promise.all(bundlePromises);
    }

    // Immediate OSC commands
    for (const oscMessage of parsed.immediate) {
      const args = oscMessage.args.map(arg => arg.value);
      orchestrator.send(oscMessage.address, ...args);
      sendsThisRound++;
    }

    if (sendsThisRound > 0) {
      messagesSent += sendsThisRound;
      updateMetrics({ messages_sent: messagesSent });
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

  console.log('[Divider] Initialized', { divider, leftColumn, rightColumn });

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

// Tab functionality for mobile
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

// Load Synthdefs button - load binary synthdefs using new API
const loadSynthdefsButton = document.getElementById('load-synthdefs-button');
if (loadSynthdefsButton) {
  loadSynthdefsButton.addEventListener('click', async () => {
    try {
      const synthNames = ['sonic-pi-beep', 'sonic-pi-tb303', 'sonic-pi-chiplead', 'sonic-pi-dsaw', 'sonic-pi-dpulse', 'sonic-pi-bnoise', 'sonic-pi-prophet', 'sonic-pi-fm', 'sonic-pi-stereo_player', 'sonic-pi-basic_stereo_player'];

      console.log('[App] Loading', synthNames.length, 'synthdefs...');
      const results = await orchestrator.loadSynthDefs(synthNames);

      const successCount = Object.values(results).filter(r => r.success).length;
      console.log(`[App] Loaded ${successCount}/${synthNames.length} synthdefs`);
    } catch (error) {
      console.error('[App] Load synthdefs error:', error);
      showError('Failed to load synthdefs: ' + error.message);
    }
  });
}

// Load Samples button - load audio samples into buffers
const loadSamplesButton = document.getElementById('load-samples-button');
if (loadSamplesButton) {
  loadSamplesButton.addEventListener('click', async () => {
    try {
      const samples = [
        { bufnum: 0, filename: 'loop_amen.flac' },
        { bufnum: 1, filename: 'ambi_choir.flac' },
        { bufnum: 2, filename: 'bd_haus.flac' }
      ];

      console.log('[App] Loading', samples.length, 'samples...');

      for (const sample of samples) {
        console.log(`[App] Loading buffer ${sample.bufnum}: ${sample.filename}`);
        await orchestrator.send('/b_allocRead', sample.bufnum, sample.filename);
      }

      console.log(`[App] Loaded ${samples.length} samples into buffers 0-2`);
    } catch (error) {
      console.error('[App] Load samples error:', error);
      showError('Failed to load samples: ' + error.message);
    }
  });
}

// Load Example button
const loadExampleButton = document.getElementById('load-example-button');
if (loadExampleButton) {
  loadExampleButton.addEventListener('click', () => {
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
  });
}
