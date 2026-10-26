<script>
  import { live, calibrate, simLoad, simDefaultGrams, displayUnit, notify } from './live.svelte.js';
  import { UNITS, fmt, fromG, toG } from './format.js';
  import { DEFAULT_STABLE_TIME_S, MIN_CAL_POINTS, MIN_REFERENCE_RATIO, PERCENT } from './constants.js';

  /** Significant digits of the standard deviation of the raw value */
  const SIG_DIGITS = 3;
  /** Decimal places of raw values and calibration point masses */
  const RAW_DECIMALS = 4;
  /** Decimal places of rated output and zero balance [mV/V] */
  const MVV_DECIMALS = 6;
  /** Decimal places of the minimum reference in the hint */
  const REF_DECIMALS = 3;

  /** @type {{open?: boolean}} dialog open (bindable) */
  let { open = $bindable(false) } = $props();

  let dialog;
  const cal = $derived(live.calibration);
  const active = $derived(!!cal?.active);
  const isSim = $derived(live.info?.backend === 'simulation');
  const cfgCal = $derived(live.config?.calibration);
  const cfgDev = $derived(live.config?.device);

  // Inputs
  let devNominal = $state(null);
  let devUnit = $state('g');
  let pointMass = $state(0);
  let pointUnit = $state(displayUnit());
  let refMass = $state(null);
  let refUnit = $state(displayUnit());
  let simGrams = $state(null); // preset: simDefaultGrams()
  let busy = $state(false);

  $effect(() => {
    if (open && dialog && !dialog.open) {
      if (cfgDev && devNominal === null) devNominal = cfgDev.nominal_load;
      if (simGrams === null) simGrams = simDefaultGrams();
      if (cfgCal) devUnit = cfgCal.device_unit;
      dialog.showModal();
    } else if (!open && dialog?.open) {
      dialog.close();
    }
  });

  /**
   * Sends a calibration command and locks the controls meanwhile.
   * @param {{action: string}} body command
   * @param {string} [okText] notice on success
   * @returns {Promise<boolean>} true on success
   */
  async function act(body, okText) {
    busy = true;
    const ok = await calibrate(body);
    busy = false;
    if (ok && okText) notify('ok', okText);
    return ok;
  }

  /**
   * Formats with SIG_DIGITS significant digits (standard deviation of the raw value).
   * @param {number|null} v value
   * @returns {string}
   */
  const sig = (v) => (v == null ? '–' : Number(v.toPrecision(SIG_DIGITS)).toLocaleString('de-DE'));
  /**
   * Display name of a unit.
   * @param {string} id unit
   * @returns {string}
   */
  const unitName = (id) => UNITS.find((u) => u.id === id)?.label ?? id;

  // --- Application
  const points = $derived(cal?.kind === 'application' ? cal.points ?? [] : []);
  const canCommitApp = $derived(points.length >= MIN_CAL_POINTS && !cal?.capturing);

  // --- Terminal
  const devPhase = $derived(cal?.kind === 'device' ? cal.phase : null);
  const stableTime = $derived(cal?.stable_time_s || DEFAULT_STABLE_TIME_S);
  const stableOk = $derived((cal?.stable_s ?? 0) >= stableTime);
  const stableFrac = $derived(Math.min(1, (cal?.stable_s ?? 0) / stableTime));
  const cmdBusy = $derived(devPhase === 'zero_busy' || devPhase === 'ref_busy');
  const steps = $derived([
    { id: 'prepared', label: 'Vorbereitet', done: !!devPhase },
    { id: 'zero', label: 'Nullabgleich', done: ['zero_done', 'ref_busy', 'done'].includes(devPhase) },
    { id: 'ref', label: 'Referenzlast', done: devPhase === 'done' },
  ]);
  const minRefG = $derived(
    cal?.kind === 'device' ? toG(MIN_REFERENCE_RATIO * cal.nominal_load, cal.device_unit) : 0,
  );

  /** Takes over the calibration result and closes the dialog on success. */
  async function commit() {
    if (await act({ action: 'commit' }, 'Kalibrierung übernommen')) open = false;
  }
</script>

<dialog bind:this={dialog} onclose={() => (open = false)} aria-labelledby="cal-title">
  <header>
    <h2 id="cal-title">Kalibrierung</h2>
    <button type="button" onclick={() => (open = false)}>Schließen</button>
  </header>

  <div class="body">
    {#if !active}
      <p class="current">
        Aktive Kennlinie:
        <strong>
          {cfgCal?.mode === 'application'
            ? `in der Anwendung, ${cfgCal.points.length} Punkte`
            : `in der Klemme (Einheit ${unitName(cfgCal?.device_unit)})`}
        </strong>
        {#if live.info && !live.info.calibrated}<span class="bad">, ungültig</span>{/if}
      </p>

      <div class="methods">
        <section class="method">
          <h3>In der Anwendung</h3>
          <p class="muted">
            Kennlinie aus Nullpunkt und einem oder mehreren Referenzgewichten, stückweise linear.
            Die Parameter der Klemme bleiben unverändert.
          </p>
          <button class="primary" disabled={busy} onclick={() => act({ action: 'start_application' })}>
            Starten
          </button>
        </section>

        <section class="method">
          <h3>In der Klemme</h3>
          <p class="muted">
            Beckhoff-Abgleich: Nullabgleich und eine Referenzlast. Ergebnis in Rated output und
            Zero balance der EL3356.
          </p>
          <div class="row">
            <label class="field grow">
              <span>Nennlast</span>
              <input type="number" step="any" min="0" bind:value={devNominal} />
            </label>
            <label class="field">
              <span>Einheit</span>
              <select bind:value={devUnit}>
                {#each UNITS as u (u.id)}<option value={u.id}>{u.label}</option>{/each}
              </select>
            </label>
          </div>
          <button
            class="primary"
            disabled={busy || !(devNominal > 0)}
            onclick={() => act({ action: 'start_device', nominal_load: devNominal, unit: devUnit })}
          >
            Starten
          </button>
        </section>
      </div>
    {:else}
      <div class="live">
        <span class="muted">Rohwert</span>
        <output>
          {fmt(cal.raw, RAW_DECIMALS)}{#if cal.kind === 'device'}&nbsp;{unitName(cal.device_unit)}{/if}
        </output>
      </div>
      {#if cal.message}<p class="message">{cal.message}</p>{/if}
      {#if cal.error}<p class="bad" role="alert">{cal.error}</p>{/if}

      {#if cal.kind === 'application'}
        <div class="scroll">
        <table>
          <thead>
            <tr>
              <th scope="col">Masse [{displayUnit()}]</th>
              <th scope="col">Rohwert</th>
              <th scope="col">s</th>
              <th scope="col">n</th>
              <th scope="col"><span class="sr">Aktion</span></th>
            </tr>
          </thead>
          <tbody>
            {#each points as p, i (p.mass_g)}
              <tr>
                <td>{fmt(fromG(p.mass_g, displayUnit()), RAW_DECIMALS)}</td>
                <td>{fmt(p.raw, RAW_DECIMALS)}</td>
                <td>{sig(p.stddev)}</td>
                <td>{p.n}</td>
                <td>
                  <button class="link" disabled={busy || cal.capturing}
                    onclick={() => act({ action: 'remove_point', index: i })}>Entfernen</button>
                </td>
              </tr>
            {:else}
              <tr><td colspan="5" class="muted">Noch keine Punkte. Mit dem Nullpunkt beginnen.</td></tr>
            {/each}
          </tbody>
        </table>
        </div>

        {#if cal.capturing}
          <div class="progress" aria-label="Erfassung">
            <div style:width="{(cal.capture_progress ?? 0) * 100}%"></div>
          </div>
        {/if}

        <div class="row end">
          <label class="field grow">
            <span>Masse des aufgelegten Gewichts (0 = Nullpunkt)</span>
            <input type="number" step="any" min="0" bind:value={pointMass} />
          </label>
          <label class="field">
            <span>Einheit</span>
            <select bind:value={pointUnit}>
              {#each UNITS as u (u.id)}<option value={u.id}>{u.label}</option>{/each}
            </select>
          </label>
          <button
            disabled={busy || cal.capturing || !(pointMass >= 0)}
            onclick={() => act({ action: 'capture', mass: Number(pointMass), unit: pointUnit })}
          >
            {Number(pointMass) === 0 ? 'Nullpunkt erfassen' : 'Punkt erfassen'}
          </button>
        </div>
      {:else}
        <ol class="steps">
          {#each steps as s (s.id)}
            <li class:done={s.done}>{s.label}</li>
          {/each}
        </ol>

        <div class="stable">
          <div class="meter-label">
            <span>Stillstand</span>
            <span>{fmt(cal.stable_s ?? 0, 1)} / {fmt(cal.stable_time_s, 0)} s</span>
          </div>
          <div class="progress"><div class:ok={stableOk} style:width="{stableFrac * 100}%"></div></div>
        </div>

        {#if cmdBusy}
          <p class="message">Klemme führt das Kommando aus …</p>
        {:else if devPhase === 'prepared'}
          <p class="muted">Waage entlasten. Der Nullabgleich ist freigegeben, sobald der Wert ausreichend lange stabil ist.</p>
          <button class="primary" disabled={busy || !stableOk} onclick={() => act({ action: 'zero' })}>
            Nullabgleich ausführen
          </button>
        {:else if devPhase === 'zero_done'}
          <p class="muted">
            Referenzlast auflegen, mindestens {fmt(fromG(minRefG, refUnit), REF_DECIMALS)} {unitName(refUnit)} ({Math.round(MIN_REFERENCE_RATIO * PERCENT)} % der Nennlast).
          </p>
          <div class="row end">
            <label class="field grow">
              <span>Referenzlast</span>
              <input type="number" step="any" min="0" bind:value={refMass} />
            </label>
            <label class="field">
              <span>Einheit</span>
              <select bind:value={refUnit}>
                {#each UNITS as u (u.id)}<option value={u.id}>{u.label}</option>{/each}
              </select>
            </label>
            <button
              class="primary"
              disabled={busy || !stableOk || !(refMass > 0)}
              onclick={() => act({ action: 'reference', mass: Number(refMass), unit: refUnit })}
            >
              Kalibrierung ausführen
            </button>
          </div>
        {:else if devPhase === 'done'}
          <dl class="result">
            <dt>Rated output</dt><dd>{fmt(cal.rated_output_mv_v, MVV_DECIMALS)} mV/V</dd>
            <dt>Zero balance</dt><dd>{fmt(cal.zero_balance, MVV_DECIMALS)} mV/V</dd>
          </dl>
        {/if}
      {/if}

      {#if isSim}
        <!-- form so that Enter in the input field triggers „Auflegen“ -->
        <form
          class="sim"
          onsubmit={(e) => {
            e.preventDefault();
            simLoad(Number(simGrams));
          }}
        >
          <span class="muted">Simulation: aufgelegte Masse [g]</span>
          <input type="number" step="any" min="0" bind:value={simGrams} />
          <button type="submit">Auflegen</button>
          <button type="button" onclick={() => simLoad(0)}>Entlasten</button>
          <button type="button" onclick={() => simLoad(null)}>Automatik</button>
        </form>
      {/if}
    {/if}
  </div>

  {#if active}
    <footer>
      <button disabled={busy} onclick={() => act({ action: 'cancel' }, 'Kalibrierung abgebrochen')}>
        Abbrechen
      </button>
      <button
        class="primary"
        disabled={busy || (cal.kind === 'application' ? !canCommitApp : devPhase !== 'done')}
        onclick={commit}
      >
        Übernehmen
      </button>
    </footer>
  {/if}
</dialog>

<style>
  dialog {
    width: min(720px, 100vw - 1rem);
    max-height: calc(100dvh - 1rem);
    padding: 0;
    border: 1px solid var(--line);
    border-radius: var(--r-md);
    background: var(--panel);
    color: var(--ink);
  }
  dialog::backdrop {
    background: rgb(10 15 20 / 0.45);
  }
  header,
  footer {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.5rem;
    padding: 0.85rem 1.1rem;
    border-bottom: 1px solid var(--line);
  }
  header h2 {
    font-size: 1.15rem;
  }
  footer {
    border-bottom: 0;
    border-top: 1px solid var(--line);
    justify-content: flex-end;
  }
  .body {
    padding: 1rem 1.1rem 1.2rem;
    display: flex;
    flex-direction: column;
    gap: 0.9rem;
  }
  h3 {
    margin: 0;
    font-size: 1rem;
    font-weight: 650;
  }
  p {
    margin: 0;
  }
  .current strong {
    font-weight: 600;
  }
  .methods {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 1rem;
  }
  .method {
    border: 1px solid var(--line);
    border-radius: var(--r-md);
    padding: 0.9rem;
    display: flex;
    flex-direction: column;
    gap: 0.7rem;
  }
  .method > button {
    align-self: flex-start;
  }
  @media (max-width: 600px) {
    .methods {
      grid-template-columns: 1fr;
    }
  }
  .row {
    display: flex;
    gap: 0.6rem;
    flex-wrap: wrap;
  }
  .row.end {
    align-items: flex-end;
  }
  .field {
    display: flex;
    flex-direction: column;
    gap: 0.25rem;
    font-size: 0.9rem;
  }
  .grow {
    flex: 1 1 10rem;
  }
  input,
  select {
    font: inherit;
    padding: 0.4rem 0.55rem;
    border: 1px solid var(--line);
    border-radius: var(--r-sm);
    background: var(--bg);
    color: var(--ink);
  }
  .live {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    border-bottom: 1px solid var(--line);
    padding-bottom: 0.6rem;
  }
  .live output {
    font-size: 2rem;
    font-stretch: 75%;
    font-weight: 620;
  }
  .message {
    font-weight: 520;
  }
  .bad {
    color: var(--st-fault);
  }
  .scroll {
    overflow-x: auto;
  }
  table {
    width: 100%;
    border-collapse: collapse;
    white-space: nowrap;
  }
  th,
  td {
    text-align: right;
    padding: 0.3rem 0 0.3rem 0.6rem;
    border-bottom: 1px solid var(--line);
  }
  th {
    font-weight: 560;
    font-size: 0.85rem;
    color: var(--muted);
  }
  th:first-child,
  td:first-child {
    text-align: left;
    padding-left: 0;
  }
  td[colspan] {
    text-align: left;
  }
  button.link {
    border: 0;
    background: none;
    color: var(--accent);
    padding: 0;
    font-weight: 500;
  }
  .progress {
    height: 6px;
    background: var(--bg);
    border-radius: 3px;
    overflow: hidden;
  }
  .progress > div {
    height: 100%;
    background: var(--st-settling);
    transition: width 120ms linear;
  }
  .progress > div.ok {
    background: var(--st-captured);
  }
  .meter-label {
    display: flex;
    justify-content: space-between;
    font-size: 0.85rem;
    color: var(--muted);
    margin-bottom: 0.25rem;
  }
  .steps {
    display: flex;
    gap: 0.5rem;
    margin: 0;
    padding: 0;
    list-style: none;
    counter-reset: step;
  }
  .steps li {
    counter-increment: step;
    flex: 1;
    padding: 0.4rem 0.6rem;
    border: 1px solid var(--line);
    border-radius: var(--r-sm);
    font-size: 0.88rem;
    color: var(--muted);
  }
  .steps li::before {
    content: counter(step) '  ';
    font-weight: 650;
  }
  .steps li.done {
    border-color: var(--st-captured);
    color: var(--st-captured);
  }
  .result {
    display: grid;
    grid-template-columns: auto 1fr;
    gap: 0.3rem 1rem;
    margin: 0;
  }
  .result dt {
    color: var(--muted);
  }
  .result dd {
    margin: 0;
    font-weight: 600;
  }
  .sim {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem;
    padding-top: 0.8rem;
    border-top: 1px dashed var(--line);
  }
  .sim input {
    width: 7rem;
  }
  .sr {
    position: absolute;
    width: 1px;
    height: 1px;
    overflow: hidden;
    clip: rect(0 0 0 0);
  }
</style>
