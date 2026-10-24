<script>
  import { live, displayUnit } from './live.svelte.js';
  import { decimalsFor, fmt, fromG } from './format.js';

  const H = 200;
  const M = { l: 36, r: 8, t: 10, b: 26 };

  let width = $state(600);
  let scope = $state('all'); // all | rolling
  let hover = $state(null); // Index des Balkens unter dem Zeiger

  const unit = $derived(displayUnit());
  const dec = $derived(decimalsFor(live.config?.measurement?.tolerance, unit));
  const rolling = $derived(live.config?.statistics?.rolling_window ?? 50);

  // live.measurements: neueste zuerst, Werte in g
  const values = $derived(
    (scope === 'rolling' ? live.measurements.slice(0, rolling) : live.measurements).map((m) => fromG(m.value, unit)),
  );
  const latest = $derived(values[0]);

  /**
   * Rundet auf 1, 2 oder 5 × 10^k (nach oben).
   * @param {number} x Wert > 0
   * @returns {number}
   */
  function niceStep(x) {
    const p = 10 ** Math.floor(Math.log10(x));
    const f = x / p;
    return (f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10) * p;
  }

  /**
   * Quantil mit linearer Interpolation.
   * @param {number[]} sorted Werte, aufsteigend sortiert
   * @param {number} q Anteil 0..1
   * @returns {number}
   */
  function quantile(sorted, q) {
    const i = (sorted.length - 1) * q;
    const lo = Math.floor(i);
    return sorted[lo] + (sorted[Math.min(lo + 1, sorted.length - 1)] - sorted[lo]) * (i - lo);
  }

  const hist = $derived.by(() => {
    if (values.length < 2) return null;
    const sorted = [...values].sort((a, b) => a - b);
    // Ausreißer (außerhalb Q1 − 3·IQR … Q3 + 3·IQR) nicht in die Klassen aufnehmen
    const q1 = quantile(sorted, 0.25);
    const q3 = quantile(sorted, 0.75);
    const iqr = q3 - q1;
    const inner = iqr > 0 ? sorted.filter((v) => v >= q1 - 3 * iqr && v <= q3 + 3 * iqr) : sorted;
    const outliers = sorted.length - inner.length;
    const n = inner.length;
    const min = inner[0];
    const max = inner[n - 1];
    const mean = inner.reduce((a, v) => a + v, 0) / n;
    const sd = n > 1 ? Math.sqrt(inner.reduce((a, v) => a + (v - mean) ** 2, 0) / (n - 1)) : 0;
    // Klassenbreite nach Freedman–Diaconis (Rückfall √n-Regel), 5..40 Klassen,
    // gerundet auf 1/2/5 · 10^k und nicht feiner als die Anzeigeauflösung
    const span = max - min;
    let w = iqr > 0 ? (2 * iqr) / Math.cbrt(n) : span / Math.ceil(Math.sqrt(n));
    if (span > 0) w = Math.min(Math.max(w, span / 40), span / 5);
    const res = 10 ** -dec;
    w = Math.max(res, niceStep(w || res));
    const start = Math.floor(min / w + 1e-9) * w || 0;
    const nb = Math.max(1, Math.floor((max - start) / w + 1e-9) + 1);
    const counts = new Array(nb).fill(0);
    for (const v of inner) counts[Math.min(nb - 1, Math.floor((v - start) / w + 1e-9))]++;
    return { n, outliers, mean, sd, w, start, end: start + nb * w, counts, top: Math.max(...counts) };
  });

  const pw = $derived(Math.max(10, width - M.l - M.r));
  const ph = H - M.t - M.b;
  /** @param {number} v Messwert @returns {number} x-Koordinate im SVG */
  const sx = (v) => M.l + ((v - hist.start) / (hist.end - hist.start)) * pw;
  /** @param {number} c Häufigkeit @returns {number} y-Koordinate im SVG */
  const sy = (c) => M.t + ph - (c / yMax) * ph;
  const yMax = $derived(hist ? Math.max(hist.top, normalPeak()) * 1.08 : 1);

  /**
   * Erwartete Häufigkeit je Klasse bei Normalverteilung N(mean, sd).
   * @param {number} v Messwert
   * @returns {number}
   */
  function expected(v) {
    const { n, w, mean, sd } = hist;
    return ((n * w) / (sd * Math.sqrt(2 * Math.PI))) * Math.exp(-0.5 * ((v - mean) / sd) ** 2);
  }
  /** @returns {number} Maximum der Normalverteilungskurve (0 bei sd = 0) */
  function normalPeak() {
    return hist.sd > 0 ? expected(hist.mean) : 0;
  }

  const curve = $derived.by(() => {
    if (!hist || !(hist.sd > 0)) return '';
    const pts = [];
    for (let i = 0; i <= 80; i++) {
      const v = hist.start + ((hist.end - hist.start) * i) / 80;
      pts.push(`${sx(v).toFixed(1)},${sy(expected(v)).toFixed(1)}`);
    }
    return 'M' + pts.join('L');
  });

  const xTicks = $derived.by(() => {
    if (!hist) return [];
    const step = Math.max(hist.w, niceStep((hist.end - hist.start) / Math.max(2, Math.floor(pw / 90))));
    const out = [];
    // über Ganzzahlen zählen, damit keine Rundungsreste (z. B. „-0“) entstehen
    for (let i = Math.ceil(hist.start / step - 1e-9); i * step <= hist.end + 1e-9; i++) out.push(i * step || 0);
    return out;
  });

  const yTicks = $derived.by(() => {
    if (!hist) return [];
    const step = Math.max(1, niceStep(yMax / 4));
    const out = [];
    for (let c = 0; c <= yMax; c += step) out.push(c);
    return out;
  });

  const tickDec = $derived(hist ? Math.max(0, Math.min(6, Math.ceil(-Math.log10(hist.w) - 1e-9))) : dec);
  // Klasse der letzten Messung (-1, wenn sie als Ausreißer außerhalb liegt)
  const latestBin = $derived.by(() => {
    if (!hist || latest == null || latest < hist.start || latest > hist.end) return -1;
    return Math.min(hist.counts.length - 1, Math.floor((latest - hist.start) / hist.w + 1e-9));
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
        aria-checked={scope === 'rolling'}
        class:on={scope === 'rolling'}
        onclick={() => (scope = 'rolling')}>Letzte {rolling}</button
      >
    </div>
  </div>

  {#if !hist}
    <p class="muted empty">Das Histogramm erscheint ab zwei erfassten Messungen.</p>
  {:else}
    <div class="plot" bind:clientWidth={width}>
      <svg {width} height={H} role="img" aria-label="Histogramm der Messwerte, {hist.n} Messungen">
        {#each yTicks as c (c)}
          <line class="grid" x1={M.l} x2={M.l + pw} y1={sy(c)} y2={sy(c)} />
          <text class="tick" x={M.l - 6} y={sy(c)} text-anchor="end" dominant-baseline="middle">{c}</text>
        {/each}
        {#each xTicks as v (v)}
          {@const x = sx(v)}
          <!-- Randbeschriftungen bündig, damit sie nicht abgeschnitten werden -->
          <text
            class="tick"
            {x}
            y={H - 8}
            text-anchor={x > M.l + pw - 24 ? 'end' : x < M.l + 24 ? 'start' : 'middle'}>{fmt(v, tickDec)}</text
          >
        {/each}

        {#each hist.counts as c, i (i)}
          {@const x0 = sx(hist.start + i * hist.w)}
          {@const x1 = sx(hist.start + (i + 1) * hist.w)}
          <rect
            class="bar"
            class:latest={i === latestBin}
            class:hover={i === hover}
            x={x0 + 0.5}
            y={sy(c)}
            width={Math.max(1, x1 - x0 - 1)}
            height={M.t + ph - sy(c)}
          />
          <!-- Trefferfläche über die volle Höhe, auch für leere Klassen -->
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

        {#if curve}<path class="normal" d={curve} />{/if}
        <line class="mean" x1={sx(hist.mean)} x2={sx(hist.mean)} y1={M.t} y2={M.t + ph} />
        <line class="axis" x1={M.l} x2={M.l + pw} y1={M.t + ph} y2={M.t + ph} />

        {#if hover !== null}
          {@const a = hist.start + hover * hist.w}
          {@const tx = Math.min(Math.max(sx(a + hist.w / 2), M.l + 70), M.l + pw - 70)}
          <g class="tip" transform="translate({tx},{M.t + 4})">
            <rect x="-70" y="0" width="140" height="22" rx="3" />
            <text x="0" y="11" text-anchor="middle" dominant-baseline="middle"
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
      <span>Klassenbreite {fmt(hist.w, tickDec)}</span>
      <span><span class="key normal-key"></span>Normalverteilung</span>
      <span><span class="key latest-key"></span>letzte Messung</span>
    </p>
  {/if}
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
  .plot {
    width: 100%;
  }
  svg {
    display: block;
    font-family: var(--font);
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
  .warn {
    color: var(--st-settling);
  }
  .empty {
    margin: 0.25rem 0;
  }
</style>
