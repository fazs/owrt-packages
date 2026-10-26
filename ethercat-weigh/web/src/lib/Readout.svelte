<script>
  import { live, displayUnit } from './live.svelte.js';
  import { decimalsFor, fmt, fromG, STATE_TEXT, FAULT_TEXT } from './format.js';

  const s = $derived(live.sample);
  const m = $derived(live.config?.measurement);
  const unit = $derived(displayUnit());
  const dec = $derived(decimalsFor(m?.tolerance, unit));
  const tare = $derived(s?.tare);
  const state = $derived(s?.state ?? 'idle');
  const stateText = $derived(
    state === 'fault' ? FAULT_TEXT[s?.fault] ?? STATE_TEXT.fault : STATE_TEXT[state] ?? state,
  );
  const last = $derived(live.measurements[0]);

  // Progress of the stability window
  const timeFill = $derived(
    state === 'settling' && m ? Math.min(1, (s?.window?.span_ms ?? 0) / m.stable_time_ms) : 0,
  );
  const rangeRatio = $derived(
    state === 'settling' && m && s?.window?.n > 1 ? s.window.range / m.tolerance : 0,
  );

  // One-time pulse on each new capture
  let pulse = $state(false);
  $effect(() => {
    if (!live.lastCapture) return;
    pulse = false;
    requestAnimationFrame(() => (pulse = true));
  });
</script>

<section class="readout state-{state}" class:pulse aria-live="polite">
  <div class="value-row">
    <output class="value" aria-label="Aktueller Messwert">
      {s && s.valid ? fmt(fromG(s.display ?? s.value, unit), dec) : '– – –'}
    </output>
    <span class="unit">{unit}</span>
  </div>
  <!-- always present (empty without tare) so that nothing shifts when taring -->
  <div class="net">
    {#if tare != null}Netto, Tara {fmt(fromG(tare, unit), dec)} {unit}{:else}&nbsp;{/if}
  </div>

  <div class="band">
    <span class="state">{stateText}</span>
    {#if last}
      <span class="last">
        Letzte Messung Nr. {last.id}: <strong>{fmt(fromG(last.value, unit), dec)} {unit}</strong>
      </span>
    {/if}
  </div>

  <div class="meters" aria-label="Stabilitätsfenster">
    <div class="meter">
      <div class="meter-label">
        <span>Fensterzeit</span>
        <span>{Math.round(s?.window?.span_ms ?? 0)} / {m?.stable_time_ms ?? '–'} ms</span>
      </div>
      <div class="track"><div class="fill time" style:width="{timeFill * 100}%"></div></div>
    </div>
    <div class="meter">
      <div class="meter-label">
        <span>Spannweite</span>
        <span>{fmt(fromG(s?.window?.range ?? 0, unit), dec + 1)} / {fmt(fromG(m?.tolerance, unit), dec + 1)} {unit}</span>
      </div>
      <div class="track">
        <div
          class="fill range"
          class:over={rangeRatio > 1}
          style:width="{Math.min(1, rangeRatio) * 100}%"
        ></div>
      </div>
    </div>
  </div>
</section>

<style>
  .readout {
    --st: var(--st-idle);
    background: var(--panel);
    border: 1px solid var(--line);
    border-left: 6px solid var(--st);
    border-radius: 0 var(--r-md) var(--r-md) 0;
    padding: 1rem 1.25rem 1.1rem;
  }
  .state-settling {
    --st: var(--st-settling);
  }
  .state-wait_unload {
    --st: var(--st-captured);
  }
  .state-fault {
    --st: var(--st-fault);
  }

  .value-row {
    display: flex;
    align-items: baseline;
    justify-content: flex-end;
    gap: 0.6rem;
  }
  .value {
    font-size: clamp(3.6rem, 13vw, 7.5rem);
    font-stretch: 72%;
    font-weight: 640;
    line-height: 1;
    letter-spacing: -0.01em;
  }
  .unit {
    font-size: clamp(1.2rem, 3.5vw, 1.9rem);
    font-stretch: 85%;
    font-weight: 500;
    color: var(--muted);
    min-width: 2ch;
  }
  .net {
    text-align: right;
    color: var(--muted);
    font-size: 0.9rem;
    margin-top: 0.15rem;
  }
  .pulse .value {
    animation: capture 700ms ease-out;
  }
  @keyframes capture {
    0% {
      color: var(--st-captured);
    }
    100% {
      color: var(--ink);
    }
  }

  .band {
    display: flex;
    flex-wrap: wrap;
    justify-content: space-between;
    align-items: baseline;
    gap: 0.25rem 1rem;
    margin-top: 0.6rem;
    padding-top: 0.6rem;
    border-top: 1px solid var(--line);
  }
  .state {
    color: var(--st);
    font-weight: 650;
    font-size: 1.15rem;
  }
  .last {
    color: var(--muted);
  }
  .last strong {
    color: var(--ink);
    font-weight: 600;
  }

  .meters {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 1rem;
    margin-top: 0.9rem;
  }
  .meter-label {
    display: flex;
    justify-content: space-between;
    gap: 0.5rem;
    font-size: 0.85rem;
    color: var(--muted);
    margin-bottom: 0.25rem;
  }
  .track {
    height: 6px;
    background: var(--bg);
    border-radius: 3px;
    overflow: hidden;
  }
  .fill {
    height: 100%;
    background: var(--st-settling);
    transition: width 120ms linear;
  }
  .fill.range {
    background: var(--st-captured);
  }
  .fill.range.over {
    background: var(--st-fault);
  }

  @media (max-width: 520px) {
    .meters {
      grid-template-columns: 1fr;
      gap: 0.6rem;
    }
  }
</style>
