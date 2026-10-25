<script>
  import { live, displayUnit, setLimits } from './live.svelte.js';
  import { decimalsFor, fmt, fromG, toG, MAX_DECIMALS } from './format.js';
  import { MIN_CAL_POINTS, VISIBLE_MEASUREMENTS } from './constants.js';

  // --- Layout [px]
  /** Höhe des Diagramms */
  const H = 200;
  /** Ränder: links (y-Beschriftung), rechts, oben, unten (x-Beschriftung) */
  const M = { l: 36, r: 8, t: 10, b: 26 };
  /** Breite vor der ersten Messung des Containers */
  const DEFAULT_WIDTH = 600;
  /** kleinste Zeichenbreite */
  const MIN_PLOT_WIDTH = 10;
  /** Mindestabstand der x-Beschriftungen */
  const X_TICK_SPACING = 90;
  /** Abstand der y-Beschriftung von der Achse */
  const Y_LABEL_GAP = 6;
  /** Abstand der x-Beschriftung vom unteren Rand */
  const X_LABEL_BASELINE = 8;
  /** Randbereich, in dem x-Beschriftungen bündig statt zentriert stehen */
  const X_LABEL_EDGE = 24;
  /** halbe Balkenlücke bzw. Mindestbreite eines Balkens */
  const BAR_GAP = 0.5;
  const BAR_MIN_WIDTH = 1;
  /** Tooltip: Breite, Höhe, Eckenradius, Abstand oben */
  const TIP_W = 140;
  const TIP_H = 22;
  const TIP_R = 3;
  const TIP_TOP = 4;
  /** Grenzbalken: sichtbare Breite, Greifbreite, Höhe des Griffs */
  const LIMIT_BAR_W = 3;
  const LIMIT_HIT_W = 14;
  const LIMIT_GRIP_H = 26;
  /** Pfeiltasten am Grenzbalken: Schritte je Druck mit Umschalt */
  const KEY_FAST_STEPS = 10;

  // --- Klassenbildung und Achsen
  /** Mindestanzahl Werte für ein Histogramm (Streuung braucht zwei Werte) */
  const MIN_VALUES = MIN_CAL_POINTS;
  /** Quartile für den Interquartilsabstand */
  const Q1 = 0.25;
  const Q3 = 0.75;
  /** Ausreißergrenze nach Tukey ("far out"): Q1 − 3·IQR … Q3 + 3·IQR */
  const OUTLIER_IQR = 3;
  /** Freedman–Diaconis: Klassenbreite 2·IQR / ∛n */
  const FD_FACTOR = 2;
  /** zulässige Anzahl Klassen (MAX_BINS bei Faktor 1) */
  const MIN_BINS = 5;
  const MAX_BINS = 40;
  /** Faktor für die Klassenanzahl: Bereich und Schrittweite des Eingabefelds */
  const BIN_FACTOR_MIN = 0.25;
  const BIN_FACTOR_MAX = 4;
  const BIN_FACTOR_STEP = 0.25;
  /** Mantissen für „glatte“ Schrittweiten 1, 2, 5 × 10^k */
  const NICE_STEPS = [1, 2, 5, 10];
  /** Basis der Schrittweiten */
  const DECIMAL_BASE = 10;
  /** Rundungstoleranz bei Klassenzuordnung und Teilstrichen */
  const EPS = 1e-9;
  /** Luft über dem höchsten Balken bzw. der Kurve */
  const Y_HEADROOM = 1.08;
  /** Anzahl Teilstriche der y-Achse (ungefähr) */
  const Y_TICKS = 4;
  /** Stützstellen der Normalverteilungskurve */
  const CURVE_POINTS = 80;
  /** Nachkommastellen der Kurvenkoordinaten im SVG */
  const SVG_DECIMALS = 1;

  let width = $state(DEFAULT_WIDTH);
  let scope = $state('all'); // all | shown (in der Messwerttabelle angezeigte Werte)
  let hover = $state(null); // Index des Balkens unter dem Zeiger
  let binFactor = $state(1); // Faktor für die Klassenanzahl, nur bis zum Neuladen
  let dragging = $state(null); // 'lo' | 'hi' während ein Grenzbalken gezogen wird
  let svgEl = $state(); // für die Umrechnung Zeiger → Wert

  const unit = $derived(displayUnit());
  const dec = $derived(decimalsFor(live.config?.measurement?.tolerance, unit));

  // live.measurements: neueste zuerst, Werte in g
  const values = $derived(
    (scope === 'shown' ? live.measurements.slice(0, VISIBLE_MEASUREMENTS) : live.measurements).map((m) =>
      fromG(m.value, unit),
    ),
  );
  const latest = $derived(values[0]);

  // Grenzen in der Anzeigeeinheit (live.limits in g), null = offen
  const lo = $derived(fromG(live.limits.lo, unit));
  const hi = $derived(fromG(live.limits.hi, unit));
  const outside = $derived(values.filter((v) => (lo != null && v < lo) || (hi != null && v > hi)).length);
  let limitError = $state('');
  /** @param {number|null} v Grenze @returns {number|string} Inhalt des Eingabefelds ohne Umrechnungsreste */
  const fieldValue = (v) => (v == null ? '' : Number(v.toFixed(MAX_DECIMALS)));

  /**
   * Übernimmt eine Grenze aus dem Eingabefeld; leeres Feld = offen.
   * @param {'lo'|'hi'} which Grenze
   * @param {string} text Inhalt des Feldes (Anzeigeeinheit)
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
   * Rundet auf NICE_STEPS × 10^k (nach oben).
   * @param {number} x Wert > 0
   * @returns {number}
   */
  function niceStep(x) {
    const p = DECIMAL_BASE ** Math.floor(Math.log10(x));
    const f = x / p;
    return NICE_STEPS.find((m) => f <= m) * p;
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
    if (values.length < MIN_VALUES) return null;
    const sorted = [...values].sort((a, b) => a - b);
    // Ausreißer (außerhalb Q1 − OUTLIER_IQR·IQR … Q3 + OUTLIER_IQR·IQR) nicht in die Klassen aufnehmen
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
    // Klassenbreite nach Freedman–Diaconis (Rückfall √n-Regel), MIN_BINS..MAX_BINS Klassen,
    // gerundet auf NICE_STEPS · 10^k und nicht feiner als die Anzeigeauflösung
    const span = max - min;
    // binFactor skaliert die Höchstzahl und die Vorgabe nach Freedman–Diaconis
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
  const ph = H - M.t - M.b;
  /** @param {number} v Messwert @returns {number} x-Koordinate im SVG */
  const sx = (v) => M.l + ((v - hist.start) / (hist.end - hist.start)) * pw;
  /** @param {number} c Häufigkeit @returns {number} y-Koordinate im SVG */
  const sy = (c) => M.t + ph - (c / yMax) * ph;
  const yMax = $derived(hist ? Math.max(hist.top, normalPeak()) * Y_HEADROOM : 1);

  /**
   * Erwartete Häufigkeit je Klasse bei Normalverteilung N(mean, sd).
   * @param {number} v Messwert
   * @returns {number}
   */
  function expected(v) {
    const { n, w, mean, sd } = hist;
    const z = (v - mean) / sd;
    return ((n * w) / (sd * Math.sqrt(2 * Math.PI))) * Math.exp(-(z * z) / 2);  // Dichte von N(mean, sd)
  }
  /** @returns {number} Maximum der Normalverteilungskurve (0 bei sd = 0) */
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
    // über Ganzzahlen zählen, damit keine Rundungsreste (z. B. „-0“) entstehen
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

  /** Rasterschritt der Grenzen beim Ziehen [Anzeigeeinheit] */
  const limitStep = $derived(DECIMAL_BASE ** -dec);

  /**
   * Setzt eine Grenze aus dem Diagramm: auf limitStep gerastet, nicht über die andere Grenze;
   * am oder hinter dem Achsenrand wird sie aufgehoben.
   * @param {'lo'|'hi'} which Grenze
   * @param {number} v Wert [Anzeigeeinheit]
   */
  function moveLimit(which, v) {
    let x = Number((Math.round(v / limitStep) * limitStep).toFixed(dec));
    if (which === 'lo') x = x <= hist.start + EPS ? null : hi != null ? Math.min(x, hi - limitStep) : x;
    else x = x >= hist.end - EPS ? null : lo != null ? Math.max(x, lo + limitStep) : x;
    const g = x == null ? null : toG(x, unit);
    limitError = '';
    setLimits(which === 'lo' ? g : live.limits.lo, which === 'hi' ? g : live.limits.hi);
  }

  /** @param {PointerEvent} e @returns {number} Wert unter dem Zeiger [Anzeigeeinheit] */
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
   * Pfeiltasten verschieben den Grenzbalken um limitStep (mit Umschalt KEY_FAST_STEPS Schritte).
   * @param {'lo'|'hi'} which Grenze
   * @param {KeyboardEvent} e
   */
  function nudge(which, e) {
    const dir = { ArrowLeft: -1, ArrowDown: -1, ArrowRight: 1, ArrowUp: 1 }[e.key];
    if (!dir) return;
    e.preventDefault();
    const cur = (which === 'lo' ? lo : hi) ?? (which === 'lo' ? hist.start : hist.end);
    moveLimit(which, cur + dir * limitStep * (e.shiftKey ? KEY_FAST_STEPS : 1));
  }

  /** @param {number|null} v Grenze @param {number} edge Achsenrand @returns {number} x des Balkens, am Rand begrenzt */
  const barX = (v, edge) => Math.min(Math.max(sx(v ?? edge), M.l), M.l + pw);

  /** Klasse @p i liegt vollständig außerhalb der Grenzen */
  const binOutside = (i) => {
    const a = hist.start + i * hist.w;
    return (lo != null && a + hist.w <= lo + EPS) || (hi != null && a >= hi - EPS);
  };

  const tickDec = $derived(hist ? Math.max(0, Math.min(MAX_DECIMALS, Math.ceil(-Math.log10(hist.w) - EPS))) : dec);
  // Klasse der letzten Messung (-1, wenn sie als Ausreißer außerhalb liegt)
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
    <div class="plot" bind:clientWidth={width}>
      <svg bind:this={svgEl} {width} height={H} aria-label="Histogramm der Messwerte, {hist.n} Messungen">
        {#each yTicks as c (c)}
          <line class="grid" x1={M.l} x2={M.l + pw} y1={sy(c)} y2={sy(c)} />
          <text class="tick" x={M.l - Y_LABEL_GAP} y={sy(c)} text-anchor="end" dominant-baseline="middle">{c}</text>
        {/each}
        {#each xTicks as v (v)}
          {@const x = sx(v)}
          <!-- Randbeschriftungen bündig, damit sie nicht abgeschnitten werden -->
          <text
            class="tick"
            {x}
            y={H - X_LABEL_BASELINE}
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

        <!-- Bereiche außerhalb der Grenzen (Grenzen außerhalb der Achse am Rand abgeschnitten) -->
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

        <!-- Grenzbalken: ohne Grenze am Achsenrand; ziehen oder Pfeiltasten, an den Rand ziehen hebt auf -->
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
  .plot {
    width: 100%;
  }
  svg {
    display: block;
    font-family: var(--font);
    user-select: none; /* Ziehen der Grenzbalken markiert sonst die Beschriftung */
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
    touch-action: none; /* Ziehen statt Scrollen auf Touchgeräten */
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
