// ci-metrics.js — /ci, the CI suite-timings dashboard.
//
// Named ci-metrics rather than ci because a root-level `ci.js` shadows the
// /ci route: Vite resolves the extensionless request to the module, not to
// ci/index.html.
//
// Reads NDJSON from /api/ci-timings (worker.js proxies the `ci-metrics` orphan
// branch) and renders it as hand-built SVG. No charting library: the site
// bundles nothing from a CDN, and the shapes here are simple enough that a
// dependency would cost more than it saves.
//
// THE ONE RULE THIS FILE ENFORCES: suite timings are only comparable within a
// fixed (build_type, os, cc, nproc, jit) tuple. Rather than document that and
// hope, the environment <select> is a hard lock — every view derives from
// `state.env`, and nothing on the page ever aggregates across two of them.

import './icons.js'; // <t-icon>

const API = '/api/ci-timings';

// Fixed categorical order from vars.css. Assigned by suite name, never by
// rank, so filtering the selection does not repaint the survivors. Five is the
// cap: a sixth series would have to be an invented hue, so the picker refuses
// instead and the sparkline grid covers the long tail.
const SERIES_VARS = ['--chart-1', '--chart-2', '--chart-3', '--chart-4', '--chart-5'];
const MAX_SERIES = SERIES_VARS.length;

const STATUSES = ['pass', 'skip', 'fail'];
const STATUS_ICON = { pass: 'check', skip: 'minus', fail: 'x' };

const RANGES = [
  ['all', 'All time'],
  ['90d', 'Last 90 days'],
  ['30d', 'Last 30 days'],
  ['7d',  'Last 7 days'],
];

// ── STATE ───────────────────────────────────────────────────────────────────

const state = {
  rows: [],
  envs: [],           // [{ key, label, ts }]
  env: null,          // env key
  // Fixed-length slot array, one per palette color. A deselected suite leaves
  // a null HOLE rather than compacting, because the slot index IS the color:
  // compacting would repaint every survivor when you remove a series.
  suites: new Array(MAX_SERIES).fill(null),
  statuses: new Set(STATUSES),
  range: 'all',
  scale: 'linear',
  sort: { col: 'delta', dir: 'desc' },
  sparkFilter: '',
  year: null,
};

// ── FORMATTING ──────────────────────────────────────────────────────────────

function fmtDuration(ms) {
  if (ms == null || Number.isNaN(ms)) return '--';
  if (ms < 1000) return `${Math.round(ms)} ms`;
  const s = ms / 1000;
  if (s < 60) return `${s < 10 ? s.toFixed(1) : Math.round(s)} s`;
  const m = Math.floor(s / 60);
  const rem = Math.round(s - m * 60);
  return `${m}m ${String(rem).padStart(2, '0')}s`;
}

// Compact form for axis ticks, where horizontal room is scarce.
function fmtTick(ms) {
  if (ms === 0) return '0';
  if (ms < 1000) return `${Math.round(ms)}ms`;
  const s = ms / 1000;
  if (s < 60) return `${s < 10 ? s.toFixed(1) : Math.round(s)}s`;
  return `${Math.round(s / 60)}m`;
}

function fmtDate(ts) {
  return new Date(ts * 1000).toLocaleDateString(undefined, {
    month: 'short', day: 'numeric',
  });
}

function fmtDateTime(ts) {
  return new Date(ts * 1000).toLocaleString(undefined, {
    month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit',
  });
}

function fmtPct(x) {
  const sign = x > 0 ? '+' : '';
  return `${sign}${Math.abs(x) < 10 ? x.toFixed(1) : Math.round(x)}%`;
}

// `AppleClang-21.0.0` -> `AppleClang 21`, `GNU-13.3.0` -> `GNU 13.3`.
function prettyCC(cc) {
  const dash = cc.lastIndexOf('-');
  if (dash < 0) return cc;
  const name = cc.slice(0, dash);
  const parts = cc.slice(dash + 1).split('.');
  const ver = parts[1] && parts[1] !== '0'
    ? `${parts[0]}.${parts[1]}`
    : parts[0];
  return `${name} ${ver}`;
}

const esc = (s) => String(s).replace(/[&<>"']/g, (c) => (
  { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]
));

// ── DATA ────────────────────────────────────────────────────────────────────

function parseNDJSON(text) {
  const out = [];
  for (const line of text.split('\n')) {
    const t = line.trim();
    if (!t) continue;
    try {
      out.push(JSON.parse(t));
    } catch {
      // A torn final line is expected while CI is mid-append. Drop it.
    }
  }
  return out;
}

// build_type is single-valued today but need not stay so, so it is part of the
// key. It is only shown in the label when the data actually varies.
function envKey(r) {
  return [r.build_type, r.os, r.cc, r.nproc, r.jit ? 'jit' : 'nojit'].join('|');
}

function buildEnvs(rows) {
  const seen = new Map();
  for (const r of rows) {
    const key = envKey(r);
    const cur = seen.get(key);
    if (!cur) seen.set(key, { key, row: r, ts: r.ts });
    else if (r.ts > cur.ts) cur.ts = r.ts;
  }

  const buildTypes = new Set(rows.map((r) => r.build_type));
  const envs = [...seen.values()].map(({ key, row, ts }) => {
    const bits = [row.os, prettyCC(row.cc), `${row.nproc} cores`];
    if (row.jit) bits.push('JIT');
    if (buildTypes.size > 1) bits.unshift(row.build_type);
    return { key, label: bits.join(' · '), ts };
  });

  // Most recently active first, so the default is the freshest environment.
  envs.sort((a, b) => b.ts - a.ts || a.label.localeCompare(b.label));
  return envs;
}

function rangeCutoff() {
  if (state.range === 'all') return -Infinity;
  const days = parseInt(state.range, 10);
  const latest = state.rows.reduce((m, r) => Math.max(m, r.ts), 0);
  return latest - days * 86400;
}

// Every view starts here. Status filtering is deliberately NOT applied to the
// env/run axis — a run where a suite failed is still a run.
function envRows() {
  const cutoff = rangeCutoff();
  return state.rows.filter((r) => envKey(r) === state.env && r.ts >= cutoff);
}

// suite -> [{ ts, ms, status, sha, run_id }], ascending by ts.
function bySuite(rows) {
  const m = new Map();
  for (const r of rows) {
    if (!state.statuses.has(r.status)) continue;
    let a = m.get(r.suite);
    if (!a) m.set(r.suite, (a = []));
    a.push({
      ts: r.ts,
      ms: r.duration_ms,
      status: r.status,
      sha: r.sha,
      run_id: r.run_id,
    });
  }
  for (const a of m.values()) a.sort((x, y) => x.ts - y.ts);
  return m;
}

function mean(xs) { return xs.reduce((a, b) => a + b, 0) / xs.length; }

function quantile(sorted, q) {
  if (!sorted.length) return 0;
  const pos = (sorted.length - 1) * q;
  const lo = Math.floor(pos);
  const hi = Math.ceil(pos);
  return lo === hi ? sorted[lo] : sorted[lo] + (sorted[hi] - sorted[lo]) * (pos - lo);
}

function suiteStats(series) {
  const stats = [];
  for (const [suite, pts] of series) {
    const ms = pts.map((p) => p.ms);
    const sorted = [...ms].sort((a, b) => a - b);
    const first = ms[0];
    const last = ms[ms.length - 1];
    stats.push({
      suite,
      runs: pts.length,
      mean: mean(ms),
      p90: quantile(sorted, 0.9),
      last,
      delta: last - first,
      deltaPct: first > 0 ? ((last - first) / first) * 100 : 0,
      status: pts[pts.length - 1].status,
      pts,
    });
  }
  return stats;
}

// Color is bound to the suite's SLOT, which it holds until it is deselected.
// Removing another series never recolors it.
function colorOf(suite) {
  const i = state.suites.indexOf(suite);
  return i < 0 ? null : `var(${SERIES_VARS[i]})`;
}

// Selected suites in slot order, holes dropped.
function selected() {
  return state.suites.filter(Boolean);
}

function selectSuite(suite) {
  if (state.suites.includes(suite)) return;
  const free = state.suites.indexOf(null);
  if (free < 0) return;
  state.suites[free] = suite;
}

function deselectSuite(suite) {
  const i = state.suites.indexOf(suite);
  if (i >= 0) state.suites[i] = null;
}

// ── URL STATE ───────────────────────────────────────────────────────────────

function readURL() {
  const q = new URLSearchParams(location.search);
  return {
    env: q.get('env'),
    // Keep empty entries: they are slot holes, not junk.
    suites: q.get('suites') ? q.get('suites').split(',') : null,
    statuses: q.get('status') ? q.get('status').split(',').filter(Boolean) : null,
    range: q.get('range'),
    scale: q.get('scale'),
  };
}

function writeURL() {
  const q = new URLSearchParams();
  if (state.env) q.set('env', state.env);
  // Holes are serialized as empty entries ("a,,c") so slots — and therefore
  // colors — survive a reload or a shared link.
  if (selected().length) {
    q.set('suites', state.suites.map((s) => s ?? '').join(',').replace(/,+$/, ''));
  }
  if (state.statuses.size !== STATUSES.length) {
    q.set('status', [...state.statuses].join(','));
  }
  if (state.range !== 'all') q.set('range', state.range);
  if (state.scale !== 'linear') q.set('scale', state.scale);
  history.replaceState(null, '', q.toString() ? `?${q}` : location.pathname);
}

// ── SVG HELPERS ─────────────────────────────────────────────────────────────

// CSS custom properties do not resolve in SVG presentation attributes, only in
// the style property — hence style="stroke:var(--chart-1)" throughout.
const svgEl = (tag, attrs, style) => {
  const a = Object.entries(attrs)
    .map(([k, v]) => `${k}="${esc(v)}"`)
    .join(' ');
  return `<${tag} ${a}${style ? ` style="${style}"` : ''} />`;
};

// Durations are not decimal: a plain 1/2/5 ladder puts ticks on 3.3-minute
// boundaries, which render as 3m / 7m / 10m / 13m and read as an error. Step
// on values that are round in time units instead.
const TICK_STEPS_MS = [
  1, 2, 5, 10, 25, 50, 100, 250, 500,
  1_000, 2_000, 5_000, 10_000, 15_000, 30_000,
  60_000, 120_000, 300_000, 600_000, 900_000, 1_800_000,
  3_600_000, 7_200_000, 21_600_000,
];

function niceTicks(lo, hi, count) {
  if (hi <= lo) return [lo];
  const span = hi - lo;
  const step = TICK_STEPS_MS.find((s) => span / s <= count)
    ?? TICK_STEPS_MS[TICK_STEPS_MS.length - 1];
  const out = [];
  for (let v = Math.ceil(lo / step) * step; v <= hi + 1e-9; v += step) out.push(v);
  return out;
}

function logTicks(lo, hi) {
  const out = [];
  for (let e = Math.floor(Math.log10(lo)); e <= Math.ceil(Math.log10(hi)); e++) {
    for (const m of [1, 3]) {
      const v = m * Math.pow(10, e);
      if (v >= lo && v <= hi) out.push(v);
    }
  }
  return out.length >= 2 ? out : [lo, hi];
}

// ── RENDER: PROVENANCE + TILES ──────────────────────────────────────────────

function renderProvenance() {
  const rows = state.rows.filter((r) => envKey(r) === state.env);
  const latestTs = rows.reduce((m, r) => Math.max(m, r.ts), 0);
  const latest = rows.find((r) => r.ts === latestTs);
  if (!latest) return;

  const short = latest.sha.slice(0, 7);
  document.getElementById('ci-provenance').innerHTML = `
    <span>Latest run</span>
    <a class="mono ci-link"
       href="https://github.com/rjungemann/turmeric/commit/${esc(latest.sha)}">${esc(short)}</a>
    <span class="dot-sep">/</span>
    <span>${esc(fmtDateTime(latestTs))}</span>
    <span class="dot-sep">/</span>
    <span>${state.rows.length.toLocaleString()} rows on
      <span class="mono">suite-timings-${esc(state.year ?? '')}.jsonl</span></span>`;
}

function renderTiles() {
  const rows = state.rows.filter((r) => envKey(r) === state.env);
  const latestTs = rows.reduce((m, r) => Math.max(m, r.ts), 0);
  const latest = rows.filter((r) => r.ts === latestTs);

  const total = latest.reduce((a, r) => a + r.duration_ms, 0);
  const failed = latest.filter((r) => r.status === 'fail');
  const skipped = latest.filter(
    (r) => r.status === 'skip' || r.partial_skip_reason != null,
  );

  const tile = (cls, icon, label, value, note) => `
    <div class="ci-tile ${cls}">
      <div class="ci-tile-label">
        ${icon ? `<t-icon name="${icon}"></t-icon>` : ''}${esc(label)}
      </div>
      <div class="ci-tile-value">${value}</div>
      <div class="ci-tile-note">${esc(note)}</div>
    </div>`;

  document.getElementById('ci-tiles').innerHTML = [
    tile('', 'clock', 'Total wall time', esc(fmtDuration(total)),
      'Sum of every suite in the latest run'),
    tile('', 'layers', 'Suites', String(latest.length),
      `${new Set(rows.map((r) => r.suite)).size} seen in range`),
    tile(
      failed.length ? 'is-fail' : 'is-clean',
      failed.length ? 'circle-x' : 'circle-check',
      failed.length ? 'Failing' : 'All passing',
      String(failed.length),
      failed.length ? failed.map((r) => r.suite).join(', ') : 'No failures in the latest run',
    ),
    tile(skipped.length ? 'is-skip' : '', 'circle-minus', 'Skipped',
      String(skipped.length), 'Fully or partially skipped'),
  ].join('');
}

// ── RENDER: FILTERS ─────────────────────────────────────────────────────────

function renderFilters() {
  const allSuites = [...new Set(
    state.rows.filter((r) => envKey(r) === state.env).map((r) => r.suite),
  )].sort();

  const chosen = selected();
  const atCap = chosen.length >= MAX_SERIES;

  document.getElementById('ci-filters').innerHTML = `
    <div class="ci-field ci-field--env">
      <label class="ci-field-label" for="ci-env">
        Environment <span class="hint">— timings compare only within one</span>
      </label>
      <select class="ci-select" id="ci-env">
        ${state.envs.map((e) => `
          <option value="${esc(e.key)}"${e.key === state.env ? ' selected' : ''}>
            ${esc(e.label)}
          </option>`).join('')}
      </select>
    </div>

    <div class="ci-field">
      <label class="ci-field-label" for="ci-add-suite">
        Suites
        <span class="hint">— ${chosen.length} of ${MAX_SERIES}${
          atCap ? ', deselect one to add another' : ''
        }</span>
      </label>
      <select class="ci-select" id="ci-add-suite"${atCap ? ' disabled' : ''}>
        <option value="">${atCap ? 'Maximum reached' : 'Add a suite…'}</option>
        ${allSuites
          .filter((s) => !state.suites.includes(s))
          .map((s) => `<option value="${esc(s)}">${esc(s)}</option>`)
          .join('')}
      </select>
    </div>

    <div class="ci-field">
      <span class="ci-field-label">Status</span>
      <div class="ci-chips">
        ${STATUSES.map((s) => `
          <button type="button" class="ci-chip" data-status="${s}"
                  aria-pressed="${state.statuses.has(s)}">
            <span class="dot" style="background:var(--status-${s})"></span>${s}
          </button>`).join('')}
      </div>
    </div>

    <div class="ci-field">
      <label class="ci-field-label" for="ci-range">Range</label>
      <select class="ci-select" id="ci-range">
        ${RANGES.map(([v, l]) => `
          <option value="${v}"${v === state.range ? ' selected' : ''}>${l}</option>
        `).join('')}
      </select>
    </div>`;

  document.getElementById('ci-env').onchange = (e) => {
    state.env = e.target.value;
    state.suites = defaultSuites();
    render();
  };

  document.getElementById('ci-add-suite').onchange = (e) => {
    if (!e.target.value) return;
    selectSuite(e.target.value);
    render();
  };

  document.getElementById('ci-range').onchange = (e) => {
    state.range = e.target.value;
    render();
  };

  for (const btn of document.querySelectorAll('.ci-chip[data-status]')) {
    btn.onclick = () => {
      const s = btn.dataset.status;
      if (state.statuses.has(s)) {
        // Never let the last one go — an empty status set is an empty page.
        if (state.statuses.size > 1) state.statuses.delete(s);
      } else {
        state.statuses.add(s);
      }
      render();
    };
  }
}

// ── RENDER: MAIN CHART ──────────────────────────────────────────────────────

function renderChart() {
  const host = document.getElementById('ci-chart');
  const sub = document.getElementById('ci-chart-sub');
  const legend = document.getElementById('ci-legend');
  const rows = envRows();
  const series = bySuite(rows);

  const drawn = selected()
    .map((s) => ({ suite: s, pts: series.get(s) ?? [] }))
    .filter((s) => s.pts.length);

  sub.textContent = state.envs.find((e) => e.key === state.env)?.label ?? '';

  if (!drawn.length) {
    host.innerHTML = `<div class="ci-empty">
      No data for the selected suites in this environment and range.
    </div>`;
    legend.innerHTML = '';
    return;
  }

  // The x domain comes from every run in the environment, not just the
  // selected suites, so the axis holds still while you swap series in and out.
  const allTs = [...new Set(rows.map((r) => r.ts))].sort((a, b) => a - b);
  const x0 = allTs[0];
  const x1 = allTs[allTs.length - 1];

  const W = Math.max(360, host.clientWidth || 860);
  const H = 320;
  const showLabels = drawn.length <= 4 && W >= 640;
  const pad = { t: 18, r: showLabels ? 128 : 24, b: 40, l: 62 };
  const iw = W - pad.l - pad.r;
  const ih = H - pad.t - pad.b;

  const values = drawn.flatMap((s) => s.pts.map((p) => p.ms));
  const vmax = Math.max(...values);
  const vmin = Math.min(...values);
  const log = state.scale === 'log';

  // Log needs a positive floor; durations bottom out at 1 ms in practice.
  const yLo = log ? Math.max(1, vmin * 0.7) : 0;
  const yHi = log ? vmax * 1.3 : vmax * 1.08 || 1;

  const sx = (ts) => (x1 === x0 ? pad.l + iw / 2 : pad.l + ((ts - x0) / (x1 - x0)) * iw);
  const sy = (v) => {
    if (!log) return pad.t + ih - ((v - yLo) / (yHi - yLo)) * ih;
    const lv = Math.log10(Math.max(v, yLo));
    return pad.t + ih - ((lv - Math.log10(yLo)) / (Math.log10(yHi) - Math.log10(yLo))) * ih;
  };

  const parts = [];

  // Grid + y axis. Recessive: hairline rules, dim monospace labels.
  const yTicks = log ? logTicks(yLo, yHi) : niceTicks(yLo, yHi, 5);
  for (const t of yTicks) {
    const y = sy(t);
    parts.push(svgEl('line', {
      class: 'ci-grid-line', x1: pad.l, x2: pad.l + iw, y1: y, y2: y,
    }));
    parts.push(`<text class="ci-axis-text" x="${pad.l - 10}" y="${y + 3}"
      text-anchor="end">${esc(fmtTick(t))}</text>`);
  }

  // X axis: one tick per run, thinned by PIXEL distance. Thinning by index
  // instead lets labels collide, because runs cluster in time — several
  // pushes in an afternoon land almost on top of each other.
  const X_GAP = 62;
  const keep = [];
  for (const ts of allTs) {
    if (!keep.length || sx(ts) - sx(keep[keep.length - 1]) >= X_GAP) keep.push(ts);
  }
  // The most recent run is the one worth labeling, so make room for it.
  const lastTs = allTs[allTs.length - 1];
  if (keep[keep.length - 1] !== lastTs) {
    while (keep.length && sx(lastTs) - sx(keep[keep.length - 1]) < X_GAP) keep.pop();
    keep.push(lastTs);
  }
  for (const ts of keep) {
    parts.push(`<text class="ci-axis-text" x="${sx(ts)}" y="${pad.t + ih + 18}"
      text-anchor="middle">${esc(fmtDate(ts))}</text>`);
  }
  parts.push(`<text class="ci-axis-title" x="${pad.l}" y="${H - 4}">
    ${allTs.length} run${allTs.length === 1 ? '' : 's'}${log ? ' · log scale' : ''}</text>`);

  // Series, in the fixed palette order.
  const labelSlots = [];
  for (const { suite, pts } of drawn) {
    const color = colorOf(suite);
    const d = pts.map((p, i) => `${i ? 'L' : 'M'}${sx(p.ts).toFixed(1)},${sy(p.ms).toFixed(1)}`).join(' ');
    parts.push(`<path class="ci-series-line" d="${d}" style="stroke:${color}" />`);

    for (const p of pts) {
      if (p.status === 'fail') {
        // A red run is a fact about the trend line, so it gets its own mark.
        parts.push(svgEl('circle', {
          class: 'ci-point-fail', cx: sx(p.ts), cy: sy(p.ms), r: 5,
        }));
      } else {
        parts.push(svgEl('circle', {
          class: 'ci-point', cx: sx(p.ts), cy: sy(p.ms), r: 3.5,
        }, `fill:${color}`));
      }
    }

    if (showLabels) {
      const last = pts[pts.length - 1];
      labelSlots.push({ suite, color, y: sy(last.ms), x: sx(last.ts) + 10 });
    }
  }

  // Direct labels, pushed apart so they never collide.
  labelSlots.sort((a, b) => a.y - b.y);
  const GAP = 15;
  for (let i = 1; i < labelSlots.length; i++) {
    if (labelSlots[i].y - labelSlots[i - 1].y < GAP) {
      labelSlots[i].y = labelSlots[i - 1].y + GAP;
    }
  }
  const overflow = labelSlots.length && labelSlots[labelSlots.length - 1].y - (pad.t + ih);
  if (overflow > 0) for (const l of labelSlots) l.y -= overflow;
  for (const l of labelSlots) {
    const name = l.suite.length > 17 ? `${l.suite.slice(0, 16)}…` : l.suite;
    parts.push(`<text class="ci-direct-label" x="${l.x}" y="${l.y}"
      style="fill:${l.color}">${esc(name)}</text>`);
  }

  // Hover layer: one crosshair + one shared tooltip per run.
  parts.push(svgEl('line', { class: 'ci-crosshair', id: 'ci-cross', x1: 0, x2: 0, y1: pad.t, y2: pad.t + ih, opacity: 0 }));
  parts.push(svgEl('rect', {
    class: 'ci-hit', id: 'ci-hit', x: pad.l, y: pad.t, width: iw, height: ih,
  }));

  host.innerHTML = `<svg viewBox="0 0 ${W} ${H}" width="${W}" height="${H}"
    role="img" aria-label="Suite duration over time">${parts.join('')}</svg>`;

  wireHover({ host, allTs, sx, series, drawn, pad, ih });
  renderLegend(drawn);
}

function renderLegend(drawn) {
  // Always present for >= 2 series; for one, the panel title already names it.
  document.getElementById('ci-legend').innerHTML = drawn.map(({ suite }) => `
    <button type="button" class="ci-legend-item" data-suite="${esc(suite)}"
            title="Remove ${esc(suite)} from the chart">
      <span class="swatch" style="background:${colorOf(suite)}"></span>
      ${esc(suite)}
      <span class="x">×</span>
    </button>`).join('');

  for (const b of document.querySelectorAll('.ci-legend-item')) {
    b.onclick = () => {
      deselectSuite(b.dataset.suite);
      render();
    };
  }
}

function wireHover({ host, allTs, sx, series, drawn, pad, ih }) {
  const svg = host.querySelector('svg');
  const hit = host.querySelector('#ci-hit');
  const cross = host.querySelector('#ci-cross');
  const tip = document.getElementById('ci-tooltip');
  if (!svg || !hit) return;

  const hide = () => {
    tip.hidden = true;
    cross.setAttribute('opacity', 0);
  };

  hit.addEventListener('mouseleave', hide);
  hit.addEventListener('mousemove', (ev) => {
    const box = svg.getBoundingClientRect();
    // The SVG is width:100% with a fixed viewBox, so map client px back into
    // user units before comparing against the scale.
    const ux = ((ev.clientX - box.left) / box.width) * svg.viewBox.baseVal.width;

    let best = allTs[0];
    let bestD = Infinity;
    for (const ts of allTs) {
      const d = Math.abs(sx(ts) - ux);
      if (d < bestD) { bestD = d; best = ts; }
    }

    const cx = sx(best);
    cross.setAttribute('x1', cx);
    cross.setAttribute('x2', cx);
    cross.setAttribute('opacity', 1);

    const rows = [];
    let sha = '';
    for (const { suite } of drawn) {
      const p = (series.get(suite) ?? []).find((q) => q.ts === best);
      if (!p) continue;
      sha = p.sha;
      rows.push(`
        <div class="ci-tooltip-row">
          <span class="dot" style="background:${colorOf(suite)}"></span>
          <span class="name">${esc(suite)}</span>
          <span class="val">${esc(fmtDuration(p.ms))}</span>
        </div>`);
    }
    if (!rows.length) { hide(); return; }

    tip.innerHTML = `
      <div class="ci-tooltip-head">
        <span>${esc(fmtDateTime(best))}</span>
        <span class="mono">${esc(sha.slice(0, 7))}</span>
      </div>${rows.join('')}`;
    tip.hidden = false;

    // Keep the tooltip inside the panel; flip it left near the right edge.
    const scale = box.width / svg.viewBox.baseVal.width;
    const px = cx * scale;
    const flip = px + tip.offsetWidth + 20 > box.width;
    tip.style.left = `${flip ? px - tip.offsetWidth - 14 : px + 14}px`;
    tip.style.top = `${Math.max(0, (pad.t + ih / 2) * scale - tip.offsetHeight / 2)}px`;
  });
}

// ── RENDER: SPARKLINE GRID ──────────────────────────────────────────────────

function renderSparks(stats) {
  const host = document.getElementById('ci-sparks');
  const q = state.sparkFilter.trim().toLowerCase();
  const shown = (q ? stats.filter((s) => s.suite.toLowerCase().includes(q)) : stats)
    .slice()
    .sort((a, b) => b.mean - a.mean);

  if (!shown.length) {
    host.innerHTML = '<div class="ci-empty">No suites match that filter.</div>';
    return;
  }

  const W = 96;
  const H = 24;
  const P = 3;

  host.innerHTML = shown.map((s) => {
    // Per-suite y scale: a shared one would flatten all but the largest few.
    const ms = s.pts.map((p) => p.ms);
    const lo = Math.min(...ms);
    const hi = Math.max(...ms);
    const span = hi - lo || 1;
    const n = ms.length;
    const px = (i) => (n === 1 ? W / 2 : P + (i / (n - 1)) * (W - P * 2));
    const py = (v) => H - P - ((v - lo) / span) * (H - P * 2);
    const d = ms.map((v, i) => `${i ? 'L' : 'M'}${px(i).toFixed(1)},${py(v).toFixed(1)}`).join(' ');
    const on = state.suites.includes(s.suite);
    const color = on ? colorOf(s.suite) : 'var(--text-dim)';

    return `
      <button type="button" class="ci-spark${on ? ' is-selected' : ''}"
              data-suite="${esc(s.suite)}"
              title="${esc(s.suite)} — mean ${esc(fmtDuration(s.mean))}, ${s.runs} runs">
        <span class="ci-spark-name">${esc(s.suite)}</span>
        <svg viewBox="0 0 ${W} ${H}" aria-hidden="true">
          <path class="ci-spark-line" d="${d}" style="stroke:${color}" />
          ${svgEl('circle', { cx: px(n - 1), cy: py(ms[n - 1]), r: 2 }, `fill:${color}`)}
        </svg>
        <span class="ci-spark-val">${esc(fmtDuration(s.last))}</span>
      </button>`;
  }).join('');

  for (const b of host.querySelectorAll('.ci-spark')) {
    b.onclick = () => toggleSuite(b.dataset.suite);
  }
}

function toggleSuite(suite) {
  if (state.suites.includes(suite)) {
    deselectSuite(suite);
  } else if (state.suites.includes(null)) {
    selectSuite(suite);
  } else {
    // At the cap, evict slot 0 rather than silently doing nothing. The other
    // four keep their slots, and so their colors.
    state.suites[0] = suite;
  }
  render();
}

// ── RENDER: TABLE ───────────────────────────────────────────────────────────

const COLS = [
  ['suite', 'Suite'],
  ['runs',  'Runs'],
  ['mean',  'Mean'],
  ['p90',   'p90'],
  ['last',  'Latest'],
  ['delta', 'Change'],
  ['status', 'Last status'],
];

function renderTable(stats) {
  const { col, dir } = state.sort;
  const sign = dir === 'asc' ? 1 : -1;
  const sorted = stats.slice().sort((a, b) => {
    if (col === 'suite' || col === 'status') {
      return sign * String(a[col]).localeCompare(String(b[col]));
    }
    // Change sorts by magnitude — the biggest movers, up or down, come first.
    if (col === 'delta') return sign * (Math.abs(a.delta) - Math.abs(b.delta));
    return sign * (a[col] - b[col]);
  });

  const head = COLS.map(([key, label]) => `
    <th scope="col">
      <button type="button" data-col="${key}">${label}${
        col === key ? `<span class="arrow">${dir === 'asc' ? '↑' : '↓'}</span>` : ''
      }</button>
    </th>`).join('');

  const body = sorted.map((s) => {
    const cls = s.delta > 0 ? 'ci-delta-up' : s.delta < 0 ? 'ci-delta-down' : 'ci-delta-flat';
    const change = s.runs < 2
      ? '<span class="ci-delta-flat">--</span>'
      : `<span class="${cls}">${esc(fmtPct(s.deltaPct))}</span>`;
    return `
      <tr class="${state.suites.includes(s.suite) ? 'is-selected' : ''}"
          data-suite="${esc(s.suite)}">
        <td title="${esc(s.suite)}">${esc(s.suite)}</td>
        <td>${s.runs}</td>
        <td>${esc(fmtDuration(s.mean))}</td>
        <td>${esc(fmtDuration(s.p90))}</td>
        <td>${esc(fmtDuration(s.last))}</td>
        <td>${change}</td>
        <td>${statusPill(s.status)}</td>
      </tr>`;
  }).join('');

  const table = document.getElementById('ci-table');
  table.innerHTML = `<thead><tr>${head}</tr></thead><tbody>${body}</tbody>`;

  for (const b of table.querySelectorAll('th button')) {
    b.onclick = () => {
      const key = b.dataset.col;
      state.sort = key === state.sort.col
        ? { col: key, dir: state.sort.dir === 'asc' ? 'desc' : 'asc' }
        : { col: key, dir: key === 'suite' || key === 'status' ? 'asc' : 'desc' };
      render();
    };
  }
  for (const tr of table.querySelectorAll('tbody tr')) {
    tr.onclick = () => toggleSuite(tr.dataset.suite);
  }
}

function statusPill(status) {
  const icon = STATUS_ICON[status] ?? 'circle';
  return `<span class="ci-pill ${esc(status)}">
    <t-icon name="${icon}"></t-icon>${esc(status)}</span>`;
}

// ── RENDER: SKIP LEDGER ─────────────────────────────────────────────────────

function renderSkips() {
  const rows = state.rows.filter((r) => envKey(r) === state.env);
  const latestTs = rows.reduce((m, r) => Math.max(m, r.ts), 0);
  const latest = rows.filter((r) => r.ts === latestTs);

  // The two reason fields are nullable by different conventions: skip_reason
  // is an explicit null on most rows, partial_skip_reason is absent entirely.
  const groups = new Map();
  const add = (kind, reason, suite) => {
    const key = `${kind} ${reason}`;
    if (!groups.has(key)) groups.set(key, { kind, reason, suites: [] });
    groups.get(key).suites.push(suite);
  };

  for (const r of latest) {
    if (r.status === 'skip') {
      add('skip', r.skip_reason || 'no reason recorded', r.suite);
    } else if (r.partial_skip_reason != null) {
      add('partial', r.partial_skip_reason, r.suite);
    }
  }

  const host = document.getElementById('ci-skips');
  if (!groups.size) {
    host.innerHTML = `<div class="ci-empty">
      Every suite ran in full in the latest build. Nothing skipped.
    </div>`;
    return;
  }

  host.innerHTML = [...groups.values()]
    .sort((a, b) => b.suites.length - a.suites.length)
    .map((g) => `
      <div class="ci-skip-group">
        <div class="ci-skip-reason">
          <!-- A partial skip is a coverage gap, not a failure: warn, not fail. -->
          <span class="ci-pill ${g.kind === 'skip' ? 'skip' : 'warn'}">
            <t-icon name="${g.kind === 'skip' ? 'minus' : 'triangle-alert'}"></t-icon>
            ${g.kind === 'skip' ? 'skipped' : 'partial'}
          </span>
          <span class="text">${esc(g.reason)}</span>
        </div>
        <div class="ci-skip-suites">
          ${g.suites.sort().map((s) => `<span>${esc(s)}</span>`).join('')}
        </div>
      </div>`).join('');
}

// ── ORCHESTRATION ───────────────────────────────────────────────────────────

// Top N by mean duration: the five biggest suites sit in the same magnitude
// band, which is what lets the chart default to a linear axis and still read.
function defaultSuites() {
  const stats = suiteStats(bySuite(envRows()));
  const top = stats
    .sort((a, b) => b.mean - a.mean)
    .slice(0, MAX_SERIES)
    .map((s) => s.suite);
  // Always MAX_SERIES long: slot index is the color, so the array is fixed
  // length and short selections are padded with holes.
  return Array.from({ length: MAX_SERIES }, (_, i) => top[i] ?? null);
}

function render() {
  const stats = suiteStats(bySuite(envRows()));
  renderProvenance();
  renderTiles();
  renderFilters();
  renderChart();
  renderSparks(stats);
  renderTable(stats);
  renderSkips();
  writeURL();

  const search = document.getElementById('ci-spark-search');
  if (search && search.value !== state.sparkFilter) search.value = state.sparkFilter;
}

function fail(message) {
  const el = document.getElementById('ci-state');
  el.className = 'ci-state is-error';
  el.innerHTML = `<t-icon name="circle-x"></t-icon><span>${esc(message)}</span>`;
}

async function boot() {
  let text;
  try {
    const res = await fetch(API);
    if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
    state.year = res.headers.get('X-Timings-Year');
    text = await res.text();
  } catch (e) {
    fail(`Could not load CI timings (${e.message}). The data is published from
          pushes to main; try again in a few minutes.`);
    return;
  }

  state.rows = parseNDJSON(text);
  if (!state.rows.length) {
    fail('No timing rows have been published yet.');
    return;
  }

  state.envs = buildEnvs(state.rows);

  const url = readURL();
  state.env = state.envs.some((e) => e.key === url.env) ? url.env : state.envs[0].key;
  if (url.range && RANGES.some(([v]) => v === url.range)) state.range = url.range;
  if (url.scale === 'log') state.scale = 'log';
  if (url.statuses) {
    const valid = url.statuses.filter((s) => STATUSES.includes(s));
    if (valid.length) state.statuses = new Set(valid);
  }

  const known = new Set(state.rows.map((r) => r.suite));
  state.suites = url.suites
    ? Array.from({ length: MAX_SERIES }, (_, i) =>
        (known.has(url.suites[i]) ? url.suites[i] : null))
    : new Array(MAX_SERIES).fill(null);
  if (!selected().length) state.suites = defaultSuites();

  document.getElementById('ci-state').hidden = true;
  document.getElementById('ci-body').hidden = false;

  for (const b of document.querySelectorAll('.ci-seg-btn[data-scale]')) {
    b.onclick = () => {
      state.scale = b.dataset.scale;
      for (const o of document.querySelectorAll('.ci-seg-btn[data-scale]')) {
        o.classList.toggle('is-active', o === b);
      }
      renderChart();
      writeURL();
    };
  }

  const search = document.getElementById('ci-spark-search');
  search.addEventListener('input', () => {
    state.sparkFilter = search.value;
    renderSparks(suiteStats(bySuite(envRows())));
  });

  render();

  // Re-render (rather than scale) on resize so text never distorts.
  let raf = 0;
  new ResizeObserver(() => {
    cancelAnimationFrame(raf);
    raf = requestAnimationFrame(renderChart);
  }).observe(document.getElementById('ci-chart'));
}

boot();
