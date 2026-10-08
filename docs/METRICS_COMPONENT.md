# Metrics Component

`<clockwork-metrics>` is a web component that renders real-time performance metrics from a SuperSonic instance. It builds its entire UI from the metrics schema — no hand-coded HTML required.

## Quick Start

```html
<link rel="stylesheet" href="https://unpkg.com/supersonic-scsynth@latest/dist/metrics-dark.css" />
<script type="module" src="https://unpkg.com/supersonic-scsynth@latest/dist/metrics_component.js"></script>

<button id="boot-btn">boot</button>
<clockwork-metrics id="metrics"></clockwork-metrics>

<script type="module">
  import { SuperSonic } from "https://unpkg.com/supersonic-scsynth@latest/dist/supersonic.js";

  // Build placeholder panels from schema (before boot)
  document.getElementById("metrics").buildFromSchema(SuperSonic);

  const sonic = new SuperSonic({
    baseURL: "https://unpkg.com/supersonic-scsynth@latest/dist/",
    coreBaseURL: "https://unpkg.com/supersonic-scsynth-core@latest/",
  });

  document.getElementById("boot-btn").onclick = async () => {
    await sonic.init();

    // Connect and start live updates at 10Hz
    document.getElementById("metrics").connect(sonic, { refreshRate: 10 });
  };
</script>
```

This gives you a full metrics dashboard with zero manual DOM work.

## How It Works

The component is schema-driven:

1. `SuperSonic.getMetricsSchema()` returns a `metrics` map (offsets + types) and a `layout` (panel definitions), along with `nativeStats` and `composites`, which the component does not use
2. `buildFromSchema()` creates the DOM from the layout — one panel per group, rows for each metric
3. `connect()` starts a timer that calls `getMetricsArray()` and writes values into the DOM
4. Only changed values trigger DOM updates (delta-diffing), and `getMetricsArray()` returns the same array every time

## API

### `buildFromSchema(SuperSonicClass)`

Build the DOM panels from the schema. Call with the `SuperSonic` class (not an instance) to show placeholder panels before booting.

```javascript
metricsEl.buildFromSchema(SuperSonic);
```

This is called automatically on first `connect()` if you haven't called it already.

### `connect(sonic, options?)`

Start live rendering from a SuperSonic instance.

```javascript
metricsEl.connect(sonic, { refreshRate: 10 });
```

| Option | Default | Description |
|--------|---------|-------------|
| `refreshRate` | `10` | Updates per second (Hz) |

Calling `connect()` again will disconnect the previous instance first.

### `disconnect()`

Stop the update loop. Panels remain visible showing their last values.

```javascript
metricsEl.disconnect();
```

Also called automatically when the element is removed from the DOM.


## Theming

The component renders into light DOM with `ssm-` prefixed CSS classes. It has no built-in styles — you load a theme CSS file.

### Built-in Themes

| Theme | File |
|-------|------|
| Dark  | `metrics-dark.css` |
| Light | `metrics-light.css` |

```html
<!-- Dark theme -->
<link rel="stylesheet" href="dist/metrics-dark.css" />

<!-- Light theme -->
<link rel="stylesheet" href="dist/metrics-light.css" />
```

### Custom Styling

Since everything is light DOM, you can override any class directly:

```css
/* Custom panel background */
.ssm-panel { background: #1a1a2e; }

/* Custom title colour */
.ssm-title { color: #e94560; }

/* Custom value colour */
.ssm-value { color: #0f3460; }
```

### CSS Classes

| Class | Element |
|-------|---------|
| `ssm-panel` | Panel container |
| `ssm-panel--wide` | Wide panel variant (e.g. ring buffer bars) |
| `ssm-title` | Panel title |
| `ssm-row` | Value row (label + value) |
| `ssm-label` | Row label text |
| `ssm-value` | Metric value span |
| `ssm-sep` | Separator between compound values |
| `ssm-bar` | Bar chart row |
| `ssm-bar-label` | Bar label |
| `ssm-bar-track` | Bar background track |
| `ssm-bar-fill` | Bar fill (animated) |
| `ssm-bar-peak` | Peak marker |
| `ssm-bar-value` | Percentage label |
| `ssm-bar-fill--blue`, `--green`, `--purple` | Bar fill colour (and the same for `ssm-bar-peak`) |

### Value Kinds

Values have a `data-kind` attribute for semantic colouring:

| `data-kind` | Meaning |
|-------------|---------|
| `green` | Healthy / positive |
| `error` | Error / dropped / late |
| `dim` | Low importance |
| `muted` | Secondary info (bytes, units) |
| `purple` | Debug channel |

```css
/* Make errors blink */
.ssm-value[data-kind="error"] { animation: blink 1s infinite; }
```

## Layout Control

The component itself is a grid container. The themes default to `auto-fit` responsive columns, but you control the grid from CSS:

```css
/* Fixed 5 columns */
clockwork-metrics { grid-template-columns: repeat(5, 1fr); }

/* 2 columns on mobile */
@media (max-width: 768px) {
  clockwork-metrics { grid-template-columns: repeat(2, 1fr); }
}
```


## npm Imports

When using a bundler:

```javascript
import "supersonic-scsynth/metrics";
```

```css
@import "supersonic-scsynth/metrics-dark.css";
```


## Schema Structure

The `getMetricsSchema()` return value drives everything:

```javascript
const schema = SuperSonic.getMetricsSchema();
// {
//   metrics: {
//     engineProcessCount: { offset: 0, type: 'counter', unit: 'count', description: '...' },
//     ...
//   },
//   nativeStats: { ... },   // native builds' DSP load and control-thread readings
//   composites: { ... },    // descriptions for rows that combine several metrics
//   layout: {
//     panels: [
//       { title: 'OSC Out', rows: [
//         { label: 'sent', cells: [{ key: 'oscOutMessagesSent' }] },
//         ...
//       ]},
//       ...
//     ]
//   }
// }
```

### Metric Definition Fields

| Field | Description |
|-------|-------------|
| `offset` | Index into the merged `Uint32Array` |
| `type` | `counter`, `gauge`, `constant` or `enum`; SuperSonic's own buffer and synthdef counters are `u32` |
| `unit` | `count`, `bytes`, `ms`, `us`, `Hz`, `ppm`, `%`, `bool`, `milliBpm`, `centi` or `commit`; absent on `enum` metrics and some plain counts |
| `description` | Human-readable description (used as tooltip) |
| `signed` | `true` if the value is signed int32 (e.g. drift) |
| `values` | Array of string values for `enum` types |
| `nativeOnly` | `true` for metrics nothing writes on the web (always 0 there) |

### Cell Formats

Layout cells can specify a `format` to control rendering:

| Format | Behaviour |
|--------|-----------|
| `bytes` | Format as `0 B`, `1.2 KB`, `3.4 MB` etc. |
| `signed` | Interpret uint32 as signed int32 |
| `enum` | Map integer to string from the metric's `values` array |
| `percent` | The raw value |
| `latencyUs` | Microseconds shown as milliseconds, to one decimal place |
| `milliBpm` | Thousandths shown as whole units, to one decimal place (`120000` → `120.0`) |
| `centi` | Hundredths shown to two decimal places (`150` → `1.50`) |
| `commit` | A commit as eight hex digits, or `unknown` for 0 |
| `headroom` | Show `-` for the unset value (`0xFFFFFFFF`), otherwise the raw value |
| `chromeOnly` | Show `-` when the browser has no `playbackStats` (Chrome only), otherwise the raw value |
| `chromeLatencyUs` | As `latencyUs`, but `-` without `playbackStats` |

A cell without a `format` shows the raw value.


## See Also

- [Metrics](METRICS.md) — `getMetrics()` object API and metric descriptions
- [API Reference](API.md) — Full SuperSonic API
