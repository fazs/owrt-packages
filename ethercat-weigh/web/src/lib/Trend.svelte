<script>
  import uPlot from 'uplot';
  import { onMount } from 'svelte';
  import { live, trend, trendTick, TREND_SECONDS, displayUnit } from './live.svelte.js';
  import { fromG } from './format.js';

  let el;
  let plot = null;

  const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

  // Schwellen und erfasste Messungen direkt auf die Zeichenfläche
  function drawOverlay(u) {
    const ctx = u.ctx;
    const m = live.config?.measurement;
    const { left, top, width, height } = u.bbox;
    ctx.save();
    ctx.beginPath();
    ctx.rect(left, top, width, height);
    ctx.clip();
    if (m) {
      ctx.setLineDash([5 * devicePixelRatio, 5 * devicePixelRatio]);
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
    const tMin = u.scales.x.min;
    ctx.fillStyle = css('--st-captured');
    for (const meas of live.measurements) {
      if (meas.t < tMin) break;
      const x = u.valToPos(meas.t, 'x', true);
      const y = u.valToPos(fromG(meas.value, displayUnit()), 'y', true);
      ctx.beginPath();
      ctx.arc(x, y, 5 * devicePixelRatio, 0, 2 * Math.PI);
      ctx.fill();
    }
    ctx.restore();
  }

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
        x: {
          time: false,
          range: (u, min, max) => [Math.max(max - TREND_SECONDS, 0), Math.max(max, TREND_SECONDS)],
        },
        y: {
          range: (u, min, max) => {
            const lo = Math.min(min ?? 0, 0);
            const hi = Math.max(max ?? 1, fromG(live.config?.measurement?.min_load ?? 0, displayUnit()));
            const pad = (hi - lo) * 0.08 || 0.1;
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
          values: (u, vals) => vals.map((v) => `${Math.round(v - u.scales.x.max)} s`),
        },
        {
          stroke: ink,
          font,
          grid: { stroke: grid, width: 1 },
          ticks: { show: false },
          size: 56,
          values: (u, vals) => vals.map((v) => v.toLocaleString('de-DE')),
        },
      ],
      series: [{}, { stroke: css('--ink'), width: 1.5, spanGaps: false, points: { show: false } }],
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

  // Trend in Gramm gespeichert, Darstellung in der Anzeigeeinheit
  function data() {
    const u = displayUnit();
    return [trend.t, trend.v.map((v) => (v == null ? null : fromG(v, u)))];
  }

  $effect(() => {
    trendTick.version; // Abhängigkeit
    displayUnit(); // Abhängigkeit
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
  .plot {
    height: 240px;
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
