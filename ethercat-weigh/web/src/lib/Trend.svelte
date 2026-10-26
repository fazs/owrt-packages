<script>
  import uPlot from 'uplot';
  import { onMount } from 'svelte';
  import { live, trend, trendTick, TREND_SECONDS, displayUnit } from './live.svelte.js';
  import { fromG } from './format.js';

  /** Spacing of the vertical grid lines [s] */
  const GRID_STEP_S = 5;
  /** Dash length of the threshold lines [px, logical] */
  const DASH_PX = 5;
  /** Radius of the marker of captured measurements [px, logical] */
  const MARKER_RADIUS_PX = 5;
  /** Margin above and below the curve (fraction of the value range) or at least [display unit] */
  const Y_PAD_REL = 0.08;
  const Y_PAD_MIN = 0.1;
  /** Width of the y axis [px] */
  const Y_AXIS_SIZE = 56;
  /** Line width of the curve [px] */
  const LINE_WIDTH = 1.5;

  let el;
  let plot = null;
  let tNow = 0; // timestamp of the newest value; x axis relative to it (-TREND_SECONDS .. 0)

  /**
   * Fixed grid: ticks every GRID_STEP_S, independent of the time progress.
   * @returns {number[]} -TREND_SECONDS .. 0
   */
  const xSplits = () =>
    Array.from({ length: TREND_SECONDS / GRID_STEP_S + 1 }, (_, i) => -TREND_SECONDS + i * GRID_STEP_S);

  const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

  /**
   * uPlot hook: draws thresholds and captured measurements directly onto the canvas.
   * @param {uPlot} u Plot
   */
  function drawOverlay(u) {
    const ctx = u.ctx;
    const m = live.config?.measurement;
    const { left, top, width, height } = u.bbox;
    ctx.save();
    ctx.beginPath();
    ctx.rect(left, top, width, height);
    ctx.clip();
    if (m) {
      ctx.setLineDash([DASH_PX * devicePixelRatio, DASH_PX * devicePixelRatio]);
      ctx.lineWidth = devicePixelRatio;
      for (const [val, color] of [
        [m.min_load, css('--st-settling')],
        [m.zero_threshold, css('--st-idle')],
      ]) {
        const y = u.valToPos(fromG(val, displayUnit()), 'y', true);
        ctx.strokeStyle = color;
        ctx.beginPath();
        ctx.moveTo(left, y);
        ctx.lineTo(left + width, y);
        ctx.stroke();
      }
      ctx.setLineDash([]);
    }
    ctx.fillStyle = css('--st-captured');
    for (const meas of live.measurements) {
      if (meas.t < tNow - TREND_SECONDS) break;
      const x = u.valToPos(meas.t - tNow, 'x', true);
      const y = u.valToPos(fromG(meas.value, displayUnit()), 'y', true);
      ctx.beginPath();
      ctx.arc(x, y, MARKER_RADIUS_PX * devicePixelRatio, 0, 2 * Math.PI);
      ctx.fill();
    }
    ctx.restore();
  }

  /** Recreates the plot (start and change of the color scheme). */
  function build() {
    const ink = css('--muted');
    const grid = css('--line');
    const font = `12px ${css('--font')}`;
    const opts = {
      width: el.clientWidth,
      height: el.clientHeight,
      legend: { show: false },
      cursor: { show: true, drag: { x: false, y: false }, points: { show: false } },
      scales: {
        x: { time: false, auto: false, range: () => [-TREND_SECONDS, 0] },
        y: {
          range: (u, min, max) => {
            const lo = Math.min(min ?? 0, 0);
            const hi = Math.max(max ?? 1, fromG(live.config?.measurement?.min_load ?? 0, displayUnit()));
            const pad = (hi - lo) * Y_PAD_REL || Y_PAD_MIN;
            return [lo - pad, hi + pad];
          },
        },
      },
      axes: [
        {
          stroke: ink,
          font,
          grid: { stroke: grid, width: 1 },
          ticks: { show: false },
          splits: xSplits,
          values: (u, vals) => vals.map((v) => `${v} s`),
        },
        {
          stroke: ink,
          font,
          grid: { stroke: grid, width: 1 },
          ticks: { show: false },
          size: Y_AXIS_SIZE,
          values: (u, vals) => vals.map((v) => v.toLocaleString('de-DE')),
        },
      ],
      series: [{}, { stroke: css('--ink'), width: LINE_WIDTH, spanGaps: false, points: { show: false } }],
      hooks: { draw: [drawOverlay] },
    };
    plot?.destroy();
    plot = new uPlot(opts, data(), el);
  }

  onMount(() => {
    build();
    const ro = new ResizeObserver(() => plot?.setSize({ width: el.clientWidth, height: el.clientHeight }));
    ro.observe(el);
    const mq = matchMedia('(prefers-color-scheme: dark)');
    mq.addEventListener('change', build);
    return () => {
      ro.disconnect();
      mq.removeEventListener('change', build);
      plot?.destroy();
    };
  });

  /**
   * Plot data: trend stored in grams, displayed in the display unit,
   * time relative to the newest value.
   * @returns {[number[], (number|null)[]]} [time [s], value]
   */
  function data() {
    const u = displayUnit();
    tNow = trend.t.length ? trend.t[trend.t.length - 1] : 0;
    return [trend.t.map((t) => t - tNow), trend.v.map((v) => (v == null ? null : fromG(v, u)))];
  }

  $effect(() => {
    trendTick.version; // dependency
    displayUnit(); // dependency
    plot?.setData(data());
  });
</script>

<section class="panel">
  <div class="panel-head">
    <h2>Verlauf <span class="muted unit">[{displayUnit()}]</span></h2>
    <span class="legend muted">
      <span class="key dot"></span>Erfasst
      <span class="key dash settle"></span>Mindestlast
      <span class="key dash idle"></span>Nullschwelle
    </span>
  </div>
  <div class="plot" bind:this={el}></div>
</section>

<style>
  h2 .unit {
    font-weight: 450;
  }
  /* grows with the available height (App.svelte), at least min-height */
  section {
    flex: 1 1 auto;
    display: flex;
    flex-direction: column;
  }
  .plot {
    flex: 1 1 0;
    min-height: 240px;
    overflow: hidden; /* uPlot follows the size via the ResizeObserver, but must not determine it */
    width: 100%;
  }
  .legend {
    font-size: 0.8rem;
    display: flex;
    align-items: center;
    gap: 0.35rem;
    flex-wrap: wrap;
  }
  .key {
    display: inline-block;
    margin-left: 0.5rem;
  }
  .dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    background: var(--st-captured);
  }
  .dash {
    width: 16px;
    border-top: 2px dashed;
  }
  .dash.settle {
    color: var(--st-settling);
  }
  .dash.idle {
    color: var(--st-idle);
  }
  :global(.u-axis) {
    font-family: var(--font);
  }
</style>
