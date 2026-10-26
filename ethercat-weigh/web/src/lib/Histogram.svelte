<script>
  import { live, displayUnit, setLimits } from './live.svelte.js';
  import { decimalsFor, fmt, fromG, toG, MAX_DECIMALS } from './format.js';
  import { MIN_CAL_POINTS, VISIBLE_MEASUREMENTS } from './constants.js';

  // --- Layout [px]
  /** Minimum height of the chart; more if the page has room */
  const MIN_HEIGHT = 200;
  /** Margins: left (y labels), right, top, bottom (x labels) */
  const M = { l: 36, r: 8, t: 10, b: 26 };
  /** Width before the first measurement of the container */
  const DEFAULT_WIDTH = 600;
  /** Smallest drawing width */
  const MIN_PLOT_WIDTH = 10;
  /** Minimum spacing of the x labels */
  const X_TICK_SPACING = 90;
  /** Distance of the y labels from the axis */
  const Y_LABEL_GAP = 6;
  /** Distance of the x labels from the bottom edge */
  const X_LABEL_BASELINE = 8;
  /** Edge area in which x labels are aligned flush instead of centered */
  const X_LABEL_EDGE = 24;
  /** Half bar gap or minimum width of a bar */
  const BAR_GAP = 0.5;
  const BAR_MIN_WIDTH = 1;
  /** Tooltip: width, height, corner radius, distance from the top */
  const TIP_W = 140;
  const TIP_H = 22;
  const TIP_R = 3;
  const TIP_TOP = 4;
  /** Limit bars: visible width, grab width, height of the handle */
  const LIMIT_BAR_W = 3;
  const LIMIT_HIT_W = 14;
  const LIMIT_GRIP_H = 26;
  /** Arrow keys on the limit bar: steps per press with Shift */
  const KEY_FAST_STEPS = 10;

  // --- Binning and axes
  /** Minimum number of values for a histogram (spread needs two values) */
  const MIN_VALUES = MIN_CAL_POINTS;
  /** Quartiles for the interquartile range */
  const Q1 = 0.25;
  const Q3 = 0.75;
  /** Outlier limit according to Tukey ("far out"): Q1 − 3·IQR … Q3 + 3·IQR */
  const OUTLIER_IQR = 3;
  /** Freedman–Diaconis: bin width 2·IQR / ∛n */
  const FD_FACTOR = 2;
  /** Permitted number of bins (MAX_BINS at factor 1) */
  const MIN_BINS = 5;
  const MAX_BINS = 40;
  /** Factor for the number of bins: range and step of the input field */
  const BIN_FACTOR_MIN = 0.25;
  const BIN_FACTOR_MAX = 4;
  const BIN_FACTOR_STEP = 0.25;
  /** Mantissas for „nice“ step sizes 1, 2, 5 × 10^k */
  const NICE_STEPS = [1, 2, 5, 10];
  /** Base of the step sizes */
  const DECIMAL_BASE = 10;
  /** Rounding tolerance for bin assignment and ticks */
  const EPS = 1e-9;
  /** Margin above the highest bar or the curve */
  const Y_HEADROOM = 1.08;
  /** Number of ticks of the y axis (approximately) */
  const Y_TICKS = 4;
  /** Sample points of the normal distribution curve */
  const CURVE_POINTS = 80;
  /** Decimal places of the curve coordinates in the SVG */
  const SVG_DECIMALS = 1;

  let width = $state(DEFAULT_WIDTH);
  let height = $state(MIN_HEIGHT);
  let scope = $state('all'); // all | shown (values displayed in the measurement table)
  let hover = $state(null); // index of the bar under the pointer
  let binFactor = $state(1); // factor for the number of bins, only until reload
  let dragging = $state(null); // 'lo' | 'hi' while a limit bar is being dragged
  let svgEl = $state(); // for converting pointer → value

  const unit = $derived(displayUnit());
  const dec = $derived(decimalsFor(live.config?.measurement?.tolerance, unit));

  // live.measurements: newest first, values in g
  const values = $derived(
    (scope === 'shown' ? live.measurements.slice(0, VISIBLE_MEASUREMENTS) : live.measurements).map((m) =>
      fromG(m.value, unit),
    ),
  );
  const latest = $derived(values[0]);

  // Limits in the display unit (live.limits in g), null = open
  const lo = $derived(fromG(live.limits.lo, unit));
  const hi = $derived(fromG(live.limits.hi, unit));
  const outside = $derived(values.filter((v) => (lo != null && v < lo) || (hi != null && v > hi)).length);
  let limitError = $state('');
  /** @param {number|null} v limit @returns {number|string} content of the input field without conversion remainders */
  const fieldValue = (v) => (v == null ? '' : Number(v.toFixed(MAX_DECIMALS)));

  /**
   * Takes over a limit from the input field; empty field = open.
   * @param {'lo'|'hi'} which limit
   * @param {string} text content of the field (display unit)
   */
  function setLimit(which, text) {
    const v = text.trim() === '' ? null : toG(Number(text), unit);
    const next = { ...live.limits, [which]: v };
    if (next.lo != null && next.hi != null && next.lo >= next.hi) {
      limitError = 'Die linke Grenze muss kleiner als die rechte sein.';
      return;
    }
    limitError = '';
    setLimits(next.lo, next.hi);
  }

  /**
   * Rounds to NICE_STEPS × 10^k (upwards).
   * @param {number} x value > 0
   * @returns {number}
   */
  function niceStep(x) {
    const p = DECIMAL_BASE ** Math.floor(Math.log10(x));
    const f = x / p;
    return NICE_STEPS.find((m) => f <= m) * p;
  }

  /**
   * Quantile with linear interpolation.
   * @param {number[]} sorted values, sorted ascending
   * @param {number} q fraction 0..1
   * @returns {number}
   */
  function quantile(sorted, q) {
    const i = (sorted.length - 1) * q;
    const lo = Math.floor(i);
    return sorted[lo] + (sorted[Math.min(lo + 1, sorted.length - 1)] - sorted[lo]) * (i - lo);
  }

  const hist = $derived.by(() => {
    if (values.length < MIN_VALUES) return null;
    const sorted = [...values].sort((a, b) => a - b);
    // Do not include outliers (outside Q1 − OUTLIER_IQR·IQR … Q3 + OUTLIER_IQR·IQR) in the bins
    const q1 = quantile(sorted, Q1);
    const q3 = quantile(sorted, Q3);
    const iqr = q3 - q1;
    const inner =
      iqr > 0 ? sorted.filter((v) => v >= q1 - OUTLIER_IQR * iqr && v <= q3 + OUTLIER_IQR * iqr) : sorted;
    const outliers = sorted.length - inner.length;
    const n = inner.length;
    const min = inner[0];
    const max = inner[n - 1];
    const mean = inner.reduce((a, v) => a + v, 0) / n;
    const sd = n > 1 ? Math.sqrt(inner.reduce((a, v) => a + (v - mean) ** 2, 0) / (n - 1)) : 0;
    // Bin width according to Freedman–Diaconis (fallback √n rule), MIN_BINS..MAX_BINS bins,
    // rounded to NICE_STEPS · 10^k and not finer than the display resolution
    const span = max - min;
    // binFactor scales the maximum count and the Freedman–Diaconis default
    let w = (iqr > 0 ? (FD_FACTOR * iqr) / Math.cbrt(n) : span / Math.ceil(Math.sqrt(n))) / binFactor;
    const maxBins = Math.max(MIN_BINS, Math.round(MAX_BINS * binFactor));
    if (span > 0) w = Math.min(Math.max(w, span / maxBins), span / MIN_BINS);
    const res = DECIMAL_BASE ** -dec;
    w = Math.max(res, niceStep(w || res));
    const start = Math.floor(min / w + EPS) * w || 0;
    const nb = Math.max(1, Math.floor((max - start) / w + EPS) + 1);
    const counts = new Array(nb).fill(0);
    for (const v of inner) counts[Math.min(nb - 1, Math.floor((v - start) / w + EPS))]++;
    return { n, outliers, mean, sd, w, start, end: start + nb * w, counts, top: Math.max(...counts) };
  });

  const pw = $derived(Math.max(MIN_PLOT_WIDTH, width - M.l - M.r));
  const ph = $derived(height - M.t - M.b);
  /** @param {number} v measured value @returns {number} x coordinate in the SVG */
  const sx = (v) => M.l + ((v - hist.start) / (hist.end - hist.start)) * pw;
  /** @param {number} c frequency @returns {number} y coordinate in the SVG */
  const sy = (c) => M.t + ph - (c / yMax) * ph;
  const yMax = $derived(hist ? Math.max(hist.top, normalPeak()) * Y_HEADROOM : 1);

  /**
   * Expected frequency per bin for the normal distribution N(mean, sd).
   * @param {number} v measured value
   * @returns {number}
   */
  function expected(v) {
    const { n, w, mean, sd } = hist;
    const z = (v - mean) / sd;
    return ((n * w) / (sd * Math.sqrt(2 * Math.PI))) * Math.exp(-(z * z) / 2);  // density of N(mean, sd)
  }
  /** @returns {number} maximum of the normal distribution curve (0 for sd = 0) */
  function normalPeak() {
    return hist.sd > 0 ? expected(hist.mean) : 0;
  }

  const curve = $derived.by(() => {
    if (!hist || !(hist.sd > 0)) return '';
    const pts = [];
    for (let i = 0; i <= CURVE_POINTS; i++) {
      const v = hist.start + ((hist.end - hist.start) * i) / CURVE_POINTS;
      pts.push(`${sx(v).toFixed(SVG_DECIMALS)},${sy(expected(v)).toFixed(SVG_DECIMALS)}`);
    }
    return 'M' + pts.join('L');
  });

  const xTicks = $derived.by(() => {
    if (!hist) return [];
    const step = Math.max(hist.w, niceStep((hist.end - hist.start) / Math.max(MIN_VALUES, Math.floor(pw / X_TICK_SPACING))));
    const out = [];
    // count over integers so that no rounding remainders (e.g. „-0“) arise
    for (let i = Math.ceil(hist.start / step - EPS); i * step <= hist.end + EPS; i++) out.push(i * step || 0);
    return out;
  });

  const yTicks = $derived.by(() => {
    if (!hist) return [];
    const step = Math.max(1, niceStep(yMax / Y_TICKS));
    const out = [];
    for (let c = 0; c <= yMax; c += step) out.push(c);
    return out;
  });

  /** Grid step of the limits while dragging [display unit] */
  const limitStep = $derived(DECIMAL_BASE ** -dec);

  /**
   * Sets a limit from the chart: snapped to limitStep, not beyond the other limit;
   * at or beyond the axis edge it is removed.
   * @param {'lo'|'hi'} which limit
   * @param {number} v value [display unit]
   */
  function moveLimit(which, v) {
    let x = Number((Math.round(v / limitStep) * limitStep).toFixed(dec));
    if (which === 'lo') x = x <= hist.start + EPS ? null : hi != null ? Math.min(x, hi - limitStep) : x;
    else x = x >= hist.end - EPS ? null : lo != null ? Math.max(x, lo + limitStep) : x;
    const g = x == null ? null : toG(x, unit);
    limitError = '';
    setLimits(which === 'lo' ? g : live.limits.lo, which === 'hi' ? g : live.limits.hi);
  }

  /** @param {PointerEvent} e @returns {number} value under the pointer [display unit] */
  const valueAt = (e) =>
    hist.start + ((e.clientX - svgEl.getBoundingClientRect().left - M.l) / pw) * (hist.end - hist.start);

  /** @param {'lo'|'hi'} which @param {PointerEvent} e */
  function dragStart(which, e) {
    e.currentTarget.setPointerCapture(e.pointerId);
    dragging = which;
  }
  /** @param {'lo'|'hi'} which @param {PointerEvent} e */
  function dragMove(which, e) {
    if (dragging === which) moveLimit(which, valueAt(e));
  }

  /**
   * Arrow keys move the limit bar by limitStep (with Shift KEY_FAST_STEPS steps).
   * @param {'lo'|'hi'} which limit
   * @param {KeyboardEvent} e
   */
  function nudge(which, e) {
    const dir = { ArrowLeft: -1, ArrowDown: -1, ArrowRight: 1, ArrowUp: 1 }[e.key];
    if (!dir) return;
    e.preventDefault();
    const cur = (which === 'lo' ? lo : hi) ?? (which === 'lo' ? hist.start : hist.end);
    moveLimit(which, cur + dir * limitStep * (e.shiftKey ? KEY_FAST_STEPS : 1));
  }

  /** @param {number|null} v limit @param {number} edge axis edge @returns {number} x of the bar, clamped at the edge */
  const barX = (v, edge) => Math.min(Math.max(sx(v ?? edge), M.l), M.l + pw);

  /** Bin @p i lies completely outside the limits */
  const binOutside = (i) => {
    const a = hist.start + i * hist.w;
    return (lo != null && a + hist.w <= lo + EPS) || (hi != null && a >= hi - EPS);
  };

  const tickDec = $derived(hist ? Math.max(0, Math.min(MAX_DECIMALS, Math.ceil(-Math.log10(hist.w) - EPS))) : dec);
  // Bin of the last measurement (-1 if it lies outside as an outlier)
  const latestBin = $derived.by(() => {
    if (!hist || latest == null || latest < hist.start || latest > hist.end) return -1;
    return Math.min(hist.counts.length - 1, Math.floor((latest - hist.start) / hist.w + EPS));
  });
</script>

<section class="panel">
  <div class="panel-head">
    <h2>Verteilung <span class="muted unit">[{unit}]</span></h2>
    <div class="scope" role="radiogroup" aria-label="Datenbasis">
      <button role="radio" aria-checked={scope === 'all'} class:on={scope === 'all'} onclick={() => (scope = 'all')}
        >Alle</button
      >
      <button
        role="radio"
        aria-checked={scope === 'shown'}
        class:on={scope === 'shown'}
        title="Die unter „Messungen“ angezeigten Messwerte"
        onclick={() => (scope = 'shown')}>Messungen</button
      >
    </div>
  </div>

  {#if !hist}
    <p class="muted empty">Das Histogramm erscheint ab zwei erfassten Messungen.</p>
  {:else}
    <div class="plot" style:min-height="{MIN_HEIGHT}px" bind:clientWidth={width} bind:clientHeight={height}>
      <svg bind:this={svgEl} {width} {height} aria-label="Histogramm der Messwerte, {hist.n} Messungen">
        {#each yTicks as c (c)}
          <line class="grid" x1={M.l} x2={M.l + pw} y1={sy(c)} y2={sy(c)} />
          <text class="tick" x={M.l - Y_LABEL_GAP} y={sy(c)} text-anchor="end" dominant-baseline="middle">{c}</text>
        {/each}
        {#each xTicks as v (v)}
          {@const x = sx(v)}
          <!-- edge labels aligned flush so that they are not clipped -->
          <text
            class="tick"
            {x}
            y={height - X_LABEL_BASELINE}
            text-anchor={x > M.l + pw - X_LABEL_EDGE ? 'end' : x < M.l + X_LABEL_EDGE ? 'start' : 'middle'}>{fmt(v, tickDec)}</text
          >
        {/each}

        {#each hist.counts as c, i (i)}
          {@const x0 = sx(hist.start + i * hist.w)}
          {@const x1 = sx(hist.start + (i + 1) * hist.w)}
          <rect
            class="bar"
            class:latest={i === latestBin}
            class:out={binOutside(i)}
            class:hover={i === hover}
            x={x0 + BAR_GAP}
            y={sy(c)}
            width={Math.max(BAR_MIN_WIDTH, x1 - x0 - 2 * BAR_GAP)}
            height={M.t + ph - sy(c)}
          />
          <!-- hit area over the full height, also for empty bins -->
          <rect
            class="hit"
            x={x0}
            y={M.t}
            width={x1 - x0}
            height={ph}
            role="presentation"
            onpointerenter={() => (hover = i)}
            onpointerleave={() => (hover = null)}
          />
        {/each}

        <!-- areas outside the limits (limits outside the axis clipped at the edge) -->
        {#if lo != null && lo > hist.start}
          <rect class="excluded" x={M.l} y={M.t} width={barX(lo, hist.start) - M.l} height={ph} />
        {/if}
        {#if hi != null && hi < hist.end}
          {@const x = barX(hi, hist.end)}
          <rect class="excluded" {x} y={M.t} width={M.l + pw - x} height={ph} />
        {/if}

        {#if curve}<path class="normal" d={curve} />{/if}
        <line class="mean" x1={sx(hist.mean)} x2={sx(hist.mean)} y1={M.t} y2={M.t + ph} />
        <line class="axis" x1={M.l} x2={M.l + pw} y1={M.t + ph} y2={M.t + ph} />

        <!-- limit bars: without a limit at the axis edge; drag or arrow keys, dragging to the edge removes the limit -->
        {#each [['lo', lo, hist.start, 'Linke Grenze'], ['hi', hi, hist.end, 'Rechte Grenze']] as [which, v, edge, label] (which)}
          {@const x = barX(v, edge)}
          <g
            class="limit"
            class:set={v != null}
            class:dragging={dragging === which}
            role="slider"
            tabindex="0"
            aria-label={label}
            aria-valuemin={hist.start}
            aria-valuemax={hist.end}
            aria-valuenow={v ?? edge}
            aria-valuetext={v == null ? 'keine' : `${fmt(v, dec)} ${unit}`}
            onpointerdown={(e) => dragStart(which, e)}
            onpointermove={(e) => dragMove(which, e)}
            onpointerup={() => (dragging = null)}
            onpointercancel={() => (dragging = null)}
            onkeydown={(e) => nudge(which, e)}
          >
            <rect class="limit-hit" x={x - LIMIT_HIT_W / 2} y={M.t} width={LIMIT_HIT_W} height={ph} />
            <rect class="limit-bar" x={x - LIMIT_BAR_W / 2} y={M.t} width={LIMIT_BAR_W} height={ph} />
            <rect
              class="limit-grip"
              x={x - LIMIT_HIT_W / 4}
              y={M.t + (ph - LIMIT_GRIP_H) / 2}
              width={LIMIT_HIT_W / 2}
              height={LIMIT_GRIP_H}
              rx={TIP_R}
            />
          </g>
        {/each}

        {#if hover !== null}
          {@const a = hist.start + hover * hist.w}
          {@const tx = Math.min(Math.max(sx(a + hist.w / 2), M.l + TIP_W / 2), M.l + pw - TIP_W / 2)}
          <g class="tip" transform="translate({tx},{M.t + TIP_TOP})">
            <rect x={-TIP_W / 2} y="0" width={TIP_W} height={TIP_H} rx={TIP_R} />
            <text x="0" y={TIP_H / 2} text-anchor="middle" dominant-baseline="middle"
              >{fmt(a, tickDec)} … {fmt(a + hist.w, tickDec)}: {hist.counts[hover]}</text
            >
          </g>
        {/if}
      </svg>
    </div>
    <p class="legend muted">
      <span>n = {hist.n}</span>
      {#if hist.outliers}<span class="warn">{hist.outliers} Ausreißer nicht dargestellt</span>{/if}
      <span><span class="key mean-key"></span>x̄ = {fmt(hist.mean, dec)}</span>
      <span>s = {fmt(hist.sd, dec + 1)}</span>
      <span>{hist.counts.length} Klassen, Breite {fmt(hist.w, tickDec)}</span>
      <span><span class="key normal-key"></span>Normalverteilung</span>
      <span><span class="key latest-key"></span>letzte Messung</span>
      {#if outside}<span class="bad"><span class="key limit-key"></span>{outside} außerhalb der Grenzen</span>{/if}
    </p>
  {/if}
  <div class="limits">
    <span class="muted">Grenzen [{unit}]</span>
    <label
      >links <input
        type="number"
        step="any"
        value={fieldValue(lo)}
        aria-invalid={!!limitError}
        onchange={(e) => setLimit('lo', e.currentTarget.value)}
      /></label
    >
    <label
      >rechts <input
        type="number"
        step="any"
        value={fieldValue(hi)}
        aria-invalid={!!limitError}
        onchange={(e) => setLimit('hi', e.currentTarget.value)}
      /></label
    >
    {#if lo != null || hi != null}
      <button onclick={() => ((limitError = ''), setLimits(null, null))}>Aufheben</button>
    {/if}
    {#if limitError}<span class="bad" role="alert">{limitError}</span>{/if}
    <label class="factor" title="Skaliert die Höchstzahl der Klassen ({MAX_BINS} bei 1) und die Klassenbreite"
      >Klassen × <input
        type="number"
        min={BIN_FACTOR_MIN}
        max={BIN_FACTOR_MAX}
        step={BIN_FACTOR_STEP}
        value={binFactor}
        onchange={(e) => {
          const v = Number(e.currentTarget.value);
          binFactor = Number.isFinite(v) && v > 0 ? Math.min(Math.max(v, BIN_FACTOR_MIN), BIN_FACTOR_MAX) : 1;
          e.currentTarget.value = String(binFactor);
        }}
      /></label
    >
  </div>
</section>

<style>
  h2 .unit {
    font-weight: 450;
  }
  .scope {
    display: inline-flex;
    border: 1px solid var(--line);
    border-radius: var(--r-sm);
    overflow: hidden;
  }
  .scope button {
    border: 0;
    border-radius: 0;
    padding: 0.25rem 0.6rem;
    font-size: 0.85rem;
    color: var(--muted);
  }
  .scope button + button {
    border-left: 1px solid var(--line);
  }
  .scope button.on {
    background: var(--ink);
    color: var(--panel);
  }
  /* grows with the available height (App.svelte); the SVG follows the measured size */
  section {
    flex: 1 1 auto;
    display: flex;
    flex-direction: column;
  }
  .plot {
    flex: 1 1 0;
    position: relative;
    width: 100%;
  }
  svg {
    position: absolute; /* does not contribute to the height of .plot */
    inset: 0;
    display: block;
    font-family: var(--font);
    user-select: none; /* dragging the limit bars would otherwise select the labels */
  }
  .grid {
    stroke: var(--line);
    stroke-width: 1;
  }
  .axis {
    stroke: var(--muted);
    stroke-width: 1;
  }
  .tick {
    fill: var(--muted);
    font-size: 12px;
  }
  .bar {
    fill: color-mix(in srgb, var(--ink) 55%, transparent);
  }
  .bar.latest {
    fill: var(--st-captured);
  }
  .bar.hover {
    fill: var(--ink);
  }
  .bar.out {
    fill: color-mix(in srgb, var(--st-fault) 55%, transparent);
  }
  .excluded {
    fill: color-mix(in srgb, var(--st-fault) 8%, transparent);
  }
  .limit {
    cursor: ew-resize;
    outline: none;
    touch-action: none; /* dragging instead of scrolling on touch devices */
  }
  .limit-hit {
    fill: transparent;
  }
  .limit-bar,
  .limit-grip {
    fill: var(--muted);
    opacity: 0.45;
  }
  .limit.set .limit-bar,
  .limit.set .limit-grip {
    fill: var(--st-fault);
    opacity: 1;
  }
  .limit:hover .limit-grip,
  .limit.dragging .limit-grip,
  .limit:focus-visible .limit-grip {
    opacity: 1;
    stroke: var(--ink);
    stroke-width: 1;
  }
  .hit {
    fill: transparent;
  }
  .normal {
    fill: none;
    stroke: var(--st-settling);
    stroke-width: 1.5;
  }
  .mean {
    stroke: var(--st-settling);
    stroke-width: 1;
    stroke-dasharray: 4 3;
  }
  .tip rect {
    fill: var(--ink);
  }
  .tip text {
    fill: var(--panel);
    font-size: 12px;
  }
  .legend {
    display: flex;
    flex-wrap: wrap;
    gap: 0.25rem 1rem;
    margin: 0.4rem 0 0;
    font-size: 0.8rem;
  }
  .key {
    display: inline-block;
    margin-right: 0.3rem;
    vertical-align: middle;
  }
  .mean-key {
    width: 14px;
    border-top: 2px dashed var(--st-settling);
  }
  .normal-key {
    width: 14px;
    border-top: 2px solid var(--st-settling);
  }
  .latest-key {
    width: 10px;
    height: 10px;
    background: var(--st-captured);
  }
  .limit-key {
    width: 10px;
    height: 10px;
    background: color-mix(in srgb, var(--st-fault) 55%, transparent);
  }
  .warn {
    color: var(--st-settling);
  }
  .bad {
    color: var(--st-fault);
  }
  .limits {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.4rem 0.9rem;
    margin-top: 0.5rem;
    font-size: 0.85rem;
  }
  .limits input {
    width: 7rem;
    margin-left: 0.3rem;
  }
  .limits .factor {
    margin-left: auto;
  }
  .limits .factor input {
    width: 4.5rem;
  }
  .limits button {
    padding: 0.2rem 0.6rem;
  }
  .empty {
    margin: 0.25rem 0;
  }
</style>
