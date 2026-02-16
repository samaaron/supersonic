// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * <supersonic-metrics> web component.
 *
 * Renders metrics panels from SuperSonic.getMetricsSchema().
 * Light DOM with ssm- prefixed CSS classes — no Shadow DOM, no layout opinion.
 * The embedding page controls arrangement (grid, flex, etc.) and loads an
 * optional CSS theme (metrics-dark.css or metrics-light.css).
 *
 * Usage:
 *   <supersonic-metrics id="metrics"></supersonic-metrics>
 *   <script>
 *     document.getElementById('metrics').connect(sonic, { refreshRate: 10 });
 *   </script>
 */

const HEADROOM_UNSET = 0xFFFFFFFF;

function formatBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes <= 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB'];
  let i = 0;
  while (bytes >= 1024 && i < units.length - 1) { bytes /= 1024; i++; }
  return `${bytes.toFixed(i > 0 ? 1 : 0)} ${units[i]}`;
}

function formatHeadroom(val) {
  return (val === HEADROOM_UNSET) ? '-' : String(val);
}

function formatEnum(val, values) {
  return values[val] ?? String(val);
}

function formatSigned(val) {
  // Read as int32 from uint32
  return String(val | 0);
}

function makeFormatter(cell, metricDef) {
  if (cell.format === 'bytes') return (v) => formatBytes(v);
  if (cell.format === 'headroom') return (v) => formatHeadroom(v);
  if (cell.format === 'signed') return (v) => formatSigned(v);
  if (cell.format === 'enum' && metricDef?.values) {
    const values = metricDef.values;
    return (v) => formatEnum(v, values);
  }
  return (v) => String(v);
}

class SupersonicMetrics extends HTMLElement {
  #sonic = null;
  #intervalId = null;
  #cells = [];     // { offset, format, el, prev } — flat binding array
  #bars = [];      // { usedOffset, peakOffset, capacityOffset, fillEl, peakEl, labelEl, prevUsed, prevPeak, prevCap }
  #built = false;

  /**
   * Build DOM from schema. Called automatically on first connect,
   * or can be called with a SuperSonic class to show placeholder panels before boot.
   * @param {Function} SuperSonicClass - The SuperSonic class (for static getMetricsSchema)
   */
  buildFromSchema(SuperSonicClass) {
    const schema = SuperSonicClass.getMetricsSchema();
    const metrics = schema.metrics;
    const layout = schema.layout;

    this.innerHTML = '';
    this.#cells = [];
    this.#bars = [];

    for (const panel of layout.panels) {
      const panelEl = document.createElement('div');
      panelEl.className = 'ssm-panel' + (panel.class ? ` ssm-panel--${panel.class}` : '');

      const titleEl = document.createElement('div');
      titleEl.className = 'ssm-title';
      titleEl.textContent = panel.title;
      panelEl.appendChild(titleEl);

      for (const row of panel.rows) {
        if (row.type === 'bar') {
          this.#buildBarRow(panelEl, row, metrics);
        } else {
          this.#buildValueRow(panelEl, row, metrics);
        }
      }

      this.appendChild(panelEl);
    }

    this.#built = true;
  }

  /**
   * Connect to a SuperSonic instance and start rendering.
   * @param {Object} sonic - SuperSonic instance
   * @param {Object} [options]
   * @param {number} [options.refreshRate=10] - Updates per second (Hz)
   */
  connect(sonic, options = {}) {
    this.disconnect();
    this.#sonic = sonic;

    if (!this.#built) {
      this.buildFromSchema(sonic.constructor);
    }

    // Start update loop
    const hz = options.refreshRate ?? 10;
    this.#intervalId = setInterval(() => this.#update(), 1000 / hz);
  }

  /**
   * Stop the update loop. Panels remain visible with their last values.
   */
  disconnect() {
    if (this.#intervalId !== null) {
      clearInterval(this.#intervalId);
      this.#intervalId = null;
    }
    this.#sonic = null;
  }

  disconnectedCallback() {
    this.disconnect();
  }

  #buildValueRow(panelEl, row, metrics) {
    const rowEl = document.createElement('div');
    rowEl.className = 'ssm-row';

    // Add tooltip from first metric key in the row
    const firstMetricCell = row.cells?.find(c => c.key);
    if (firstMetricCell) {
      const def = metrics[firstMetricCell.key];
      if (def?.description) rowEl.title = def.description;
    }

    const labelEl = document.createElement('span');
    labelEl.className = 'ssm-label';
    labelEl.textContent = row.label;
    rowEl.appendChild(labelEl);

    const valueContainer = document.createElement('span');

    for (const cell of (row.cells || [])) {
      if (cell.sep !== undefined) {
        // Static separator
        const sepEl = document.createElement('span');
        sepEl.className = 'ssm-sep';
        sepEl.textContent = cell.sep;
        valueContainer.appendChild(sepEl);
      } else if (cell.text !== undefined) {
        // Static text
        const textEl = document.createElement('span');
        textEl.className = 'ssm-value';
        if (cell.kind) textEl.setAttribute('data-kind', cell.kind);
        textEl.textContent = cell.text;
        valueContainer.appendChild(textEl);
      } else if (cell.key) {
        // Dynamic metric value
        const def = metrics[cell.key];
        if (!def) continue;

        const el = document.createElement('span');
        el.className = 'ssm-value';
        if (cell.kind) el.setAttribute('data-kind', cell.kind);
        el.textContent = '-';
        valueContainer.appendChild(el);

        this.#cells.push({
          offset: def.offset,
          format: makeFormatter(cell, def),
          el,
          prev: -1, // force first update
        });
      }
    }

    rowEl.appendChild(valueContainer);
    panelEl.appendChild(rowEl);
  }

  #buildBarRow(panelEl, row, metrics) {
    const rowEl = document.createElement('div');
    rowEl.className = 'ssm-bar';

    const usedDef = metrics[row.usedKey];
    const peakDef = metrics[row.peakKey];
    const capDef = metrics[row.capacityKey];
    if (usedDef?.description) rowEl.title = usedDef.description;

    const labelEl = document.createElement('span');
    labelEl.className = 'ssm-bar-label';
    labelEl.textContent = row.label;
    rowEl.appendChild(labelEl);

    const trackEl = document.createElement('div');
    trackEl.className = 'ssm-bar-track';

    const fillEl = document.createElement('div');
    fillEl.className = `ssm-bar-fill ssm-bar-fill--${row.color}`;
    trackEl.appendChild(fillEl);

    const peakEl = document.createElement('div');
    peakEl.className = `ssm-bar-peak ssm-bar-peak--${row.color}`;
    trackEl.appendChild(peakEl);

    rowEl.appendChild(trackEl);

    const valEl = document.createElement('span');
    valEl.className = 'ssm-bar-value ssm-value';
    if (row.color === 'green') valEl.setAttribute('data-kind', 'green');
    else if (row.color === 'purple') valEl.setAttribute('data-kind', 'purple');
    valEl.textContent = '-';
    rowEl.appendChild(valEl);

    panelEl.appendChild(rowEl);

    this.#bars.push({
      usedOffset: usedDef?.offset ?? 0,
      peakOffset: peakDef?.offset ?? 0,
      capacityOffset: capDef?.offset ?? 0,
      fillEl,
      peakEl,
      labelEl: valEl,
      prevUsed: -1,
      prevPeak: -1,
      prevCap: -1,
    });
  }

  #update() {
    if (!this.#sonic) return;
    const arr = this.#sonic.getMetricsArray();

    // Update value cells (delta-diff — only touch DOM on change)
    for (let i = 0; i < this.#cells.length; i++) {
      const c = this.#cells[i];
      const v = arr[c.offset];
      if (v !== c.prev) {
        c.el.textContent = c.format(v);
        c.prev = v;
      }
    }

    // Update bar charts
    for (let i = 0; i < this.#bars.length; i++) {
      const b = this.#bars[i];
      const used = arr[b.usedOffset];
      const peak = arr[b.peakOffset];
      const cap = arr[b.capacityOffset];

      if (used !== b.prevUsed || cap !== b.prevCap) {
        if (cap > 0) {
          const pct = (used / cap) * 100;
          b.fillEl.style.width = pct + '%';
          b.labelEl.textContent = pct.toFixed(1) + '%';
        } else {
          b.fillEl.style.width = '0%';
          b.labelEl.textContent = 'N/A';
        }
        b.prevUsed = used;
        b.prevCap = cap;
      }

      if (peak !== b.prevPeak && cap > 0) {
        const peakPct = (peak / cap) * 100;
        b.peakEl.style.left = `calc(${peakPct}% - 1px)`;
        b.prevPeak = peak;
      }
    }
  }
}

if (typeof customElements !== 'undefined' && !customElements.get('supersonic-metrics')) {
  customElements.define('supersonic-metrics', SupersonicMetrics);
}

export { SupersonicMetrics };
