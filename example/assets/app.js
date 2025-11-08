
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
      if (metrics.pollInterval !== undefined) {
        metricsWithUsage.poll_interval = metrics.pollInterval;
      }

      metricsWithUsage.messages_sent = orchestrator.stats.messagesSent || 0;

      updateMetrics(metricsWithUsage);
    };

    orchestrator.onError = (error) => {
      console.error('[App] Error:', error);
      showError(error.message);
      updateStatus('error');
    };

    orchestrator.onDebugMessage = (msg) => {
      // Display debug messages from debug worker
      debugLog.textContent += msg.text + '\n';
      if (window.debugAutoScroll !== false) {
        const scrollContainer = debugLog.parentElement;
        if (scrollContainer) {
          scrollContainer.scrollTop = scrollContainer.scrollHeight;
        }
      }
      flashTab('debug');
      // Also log to console in development mode
      if (orchestrator.config.development) {
        console.log(`[WASM] ${msg.text}`);
      }
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

messageForm.addEventListener('submit', (e) => {
  e.preventDefault();
  const message = messageInput.value.trim();

  if (!message || !orchestrator) return;

  // Clear any previous errors
  hideError();

  try {
    // Check if this is a multi-line scheduled message format
    const allLines = message.split('\n').map(l => l.trim()).filter(l => l);
    const lines = allLines.filter(l => !l.startsWith('#'));

    // Check if first line starts with a number (timestamp)
    const firstTokenMatch = lines[0].match(/^([\d.]+)\s+(.+)$/);
    const hasTimestamps = firstTokenMatch && firstTokenMatch[2].startsWith('/');

    if (hasTimestamps && lines.length > 0) {
      // First, log any comments
      allLines.filter(l => l.startsWith('#')).forEach(comment => {
        addSentMessage(null, comment);
      });

      // Parse scheduled messages and group by timestamp
      const now = orchestrator.audioContext.currentTime; // AudioContext time in seconds
      const messagesByTime = new Map();

      for (const line of lines) {
        const match = line.match(/^([\d.]+)\s+(.+)$/);
        if (!match) {
          showError(`Invalid format: ${line}`);
          return;
        }

        const timestamp = parseFloat(match[1]);
        const oscCommand = match[2];

        if (isNaN(timestamp)) {
          showError(`Invalid timestamp: ${match[1]}`);
          return;
        }

        if (!oscCommand.startsWith('/')) {
          showError(`Invalid OSC command: ${oscCommand}`);
          return;
        }

        const oscMessage = parseTextToOSC(oscCommand);

        if (!messagesByTime.has(timestamp)) {
          messagesByTime.set(timestamp, []);
        }
        messagesByTime.get(timestamp).push(oscMessage);
      }

      // Send bundles in timestamp order (messages with same timestamp go in same bundle)
      const sortedTimes = Array.from(messagesByTime.keys()).sort((a, b) => a - b);

      // Helper to manually create OSC bundle with correct NTP timestamp
      // Takes AudioContext time and converts to NTP using WASM's offset
      function createOSCBundle(audioTimeS, messages) {
        const NTP_EPOCH_OFFSET = 2208988800;
        // Get WASM's time offset (AudioContext → NTP conversion)
        const wasmOffset = orchestrator.wasmTimeOffset || 0;
        // Debug logs (commented out - enable for timing debugging)
        // console.log('[Bundle] Creating bundle - audioTime:', audioTimeS, 'wasmOffset:', wasmOffset);
        const ntpTimeS = audioTimeS + wasmOffset;
        const ntpSeconds = Math.floor(ntpTimeS);
        const ntpFraction = Math.floor((ntpTimeS % 1) * 0x100000000);
        // console.log('[Bundle] NTP time:', ntpTimeS, 'seconds:', ntpSeconds, 'fraction:', ntpFraction);
        // console.log('[Bundle] Creating - NTP seconds:', ntpSeconds, 'fraction:', ntpFraction);

        // Encode each message
        const encodedMessages = messages.map(msg => SuperSonic.osc.encode(msg));

        // Calculate total bundle size
        let bundleSize = 8 + 8; // "#bundle\0" + NTP timestamp
        encodedMessages.forEach(msg => {
          bundleSize += 4 + msg.byteLength; // size prefix + message
        });

        // Create bundle buffer
        const bundle = new Uint8Array(bundleSize);
        const view = new DataView(bundle.buffer);
        let offset = 0;

        // Write "#bundle\0"
        bundle.set([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00], offset);
        offset += 8;

        // Write NTP timestamp (big-endian)
        view.setUint32(offset, ntpSeconds, false);
        offset += 4;
        view.setUint32(offset, ntpFraction, false);
        offset += 4;

        // Write messages
        encodedMessages.forEach(msg => {
          view.setInt32(offset, msg.byteLength, false);
          offset += 4;
          bundle.set(msg, offset);
          offset += msg.byteLength;
        });

        return bundle;
      }

      // Debug logs (commented out - enable for scheduling debugging)
      // console.log('[Schedule] Sending', sortedTimes.length, 'bundles');
      for (const timestamp of sortedTimes) {
        const messages = messagesByTime.get(timestamp);
        const targetTimeS = now + timestamp + 0.02; // Add 20ms safety margin for execution time
        // console.log('[Schedule] Bundle at', timestamp, 's with', messages.length, 'messages');
        // console.log('[Schedule] Target AudioContext time (s):', targetTimeS);
        const oscData = createOSCBundle(targetTimeS, messages);
        // console.log('[Schedule] Bundle encoded, size:', oscData.byteLength, 'bytes');
        orchestrator.sendOSC(oscData);
        messagesSent++;
      }

      updateMetrics({ messages_sent: messagesSent });

    } else {
      // Original single message handling
      // First, log any comments
      allLines.filter(l => l.startsWith('#')).forEach(comment => {
        addSentMessage(null, comment);
      });

      if (message.startsWith('/')) {
        // Parse as OSC command: /address arg1 arg2...
        const oscMessage = parseTextToOSC(message);
        const args = oscMessage.args.map(arg => arg.value);
        orchestrator.send(oscMessage.address, ...args);
      } else if (!message.startsWith('#')) {
        // Wrap in /echo (but not if it's a comment)
        orchestrator.send('/echo', message);
      }

      messagesSent++;
      updateMetrics({ messages_sent: messagesSent });
    }
  } catch (error) {
    showError(error.message);
  }
});

// Auto-scroll enabled by default
window.debugAutoScroll = true;

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

