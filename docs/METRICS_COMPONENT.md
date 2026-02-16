# Metrics Component

`<supersonic-metrics>` is a web component that renders real-time performance metrics from a SuperSonic instance. It builds its entire UI from the metrics schema — no hand-coded HTML required.

## Quick Start

```html
<link rel="stylesheet" href="https://unpkg.com/supersonic-scsynth@latest/dist/metrics-dark.css" />
<script type="module" src="https://unpkg.com/supersonic-scsynth@latest/dist/metrics_component.js"></script>

<supersonic-metrics id="metrics"></supersonic-metrics>

<script type="module">
  import { SuperSonic } from "https://unpkg.com/supersonic-scsynth@latest";

  // Build placeholder panels from schema (before boot)
  document.getElementById("metrics").buildFromSchema(SuperSonic);

  const sonic = new SuperSonic({
    baseURL: "https://unpkg.com/supersonic-scsynth@latest/dist/",
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

1. `SuperSonic.getMetricsSchema()` returns a `metrics` map (offsets + types), a `layout` (panel definitions), and `sentinels` (magic values)
2. `buildFromSchema()` creates the DOM from the layout — one panel per group, rows for each metric
3. `connect()` starts a timer that calls `getMetricsArray()` and writes values into the DOM
4. Only changed values trigger DOM updates (delta-diffing), so the hot path is zero-allocation

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
supersonic-metrics { grid-template-columns: repeat(5, 1fr); }

/* 2 columns on mobile */
@media (max-width: 768px) {
  supersonic-metrics { grid-template-columns: repeat(2, 1fr); }
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
//     scsynthProcessCount: { offset: 0, type: 'counter', unit: 'count', description: '...' },
//     ...
//   },
//   layout: {
//     panels: [
//       { title: 'OSC Out', rows: [
//         { label: 'sent', cells: [{ key: 'oscOutMessagesSent' }] },
//         ...
//       ]},
//       ...
//     ]
//   },
//   sentinels: {
//     HEADROOM_UNSET: 0xFFFFFFFF
//   }
// }
```

### Metric Definition Fields

| Field | Description |
|-------|-------------|
| `offset` | Index into the merged `Uint32Array` |
| `type` | `counter`, `gauge`, `constant`, or `enum` |
| `unit` | `count`, `bytes`, `ms`, or `percentage` |
| `description` | Human-readable description (used as tooltip) |
| `signed` | `true` if the value is signed int32 (e.g. drift) |
| `values` | Array of string values for `enum` types |

### Cell Formats

Layout cells can specify a `format` to control rendering:

| Format | Behaviour |
|--------|-----------|
| `bytes` | Format as `0 B`, `1.2 KB`, `3.4 MB` etc. |
| `headroom` | Show `-` for unset sentinel, otherwise the raw value |
| `signed` | Interpret uint32 as signed int32 |
| `enum` | Map integer to string from the metric's `values` array |


## See Also

- [Metrics](METRICS.md) — `getMetrics()` object API and metric descriptions
- [API Reference](API.md) — Full SuperSonic API
