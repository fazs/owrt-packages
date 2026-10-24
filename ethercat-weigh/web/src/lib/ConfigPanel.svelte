<script>
  import { live, saveConfig, notify } from './live.svelte.js';
  import { FILTER_OPTIONS } from './coe.js';

  let { open = $bindable(false) } = $props();

  let dialog;
  let draft = $state(null);
  // Eingabetext der Zahlenfelder (Komma oder Punkt, Zwischenstände wie "-" oder "0,")
  let texts = $state({});
  let errors = $state([]);
  let saving = $state(false);

  const unitOptions = [
    ['mg', 'mg'],
    ['g', 'g'],
    ['kg', 'kg'],
    ['gr', 'gr (Grain)'],
  ];

  const groups = [
    {
      key: 'measurement',
      title: 'Messung',
      hint: 'Massen in Gramm. Wirkt sofort.',
      fields: [
        { k: 'min_load', l: 'Mindestlast [g]', t: 'num', h: 'Ab diesem Wert gilt die Waage als belastet.' },
        { k: 'zero_threshold', l: 'Nullschwelle [g]', t: 'num', h: 'Darunter gilt die Waage als entlastet; neue Messung freigegeben.' },
        { k: 'stable_time_ms', l: 'Fensterzeit [ms]', t: 'int', h: 'Dauer, über die der Wert stabil sein muss.' },
        { k: 'tolerance', l: 'Max. Spannweite [g]', t: 'num', h: 'max − min im Fenster.' },
        { k: 'sigma_max', l: 'Max. Standardabweichung [g]', t: 'num' },
        { k: 'use_device_steady_state', l: 'Steady-state-Bit der Klemme zusätzlich fordern', t: 'bool' },
      ],
    },
    {
      key: 'statistics',
      title: 'Statistik',
      hint: 'Wirkt sofort.',
      fields: [
        { k: 'rolling_window', l: 'Gleitendes Fenster [Messungen]', t: 'int' },
        { k: 'history_size', l: 'Gehaltene Messungen', t: 'int' },
      ],
    },
    {
      key: 'ui',
      title: 'Anzeige',
      fields: [
        { k: 'display_unit', l: 'Vorgabe-Einheit', t: 'select', o: unitOptions, h: 'Gilt, solange im Browser keine Einheit gewählt wurde.' },
        { k: 'rate_hz', l: 'Aktualisierungsrate [Hz]', t: 'int' },
      ],
    },
    {
      key: 'calibration',
      title: 'Kalibrierung',
      hint: 'Kennlinie und Stützpunkte werden über den Dialog Kalibrieren erstellt.',
      fields: [
        { k: 'mode', l: 'Verfahren', t: 'select', o: [['device', 'In der Klemme'], ['application', 'In der Anwendung']] },
        { k: 'device_unit', l: 'Einheit der Klemme', t: 'select', o: unitOptions, h: 'Einheit von Nominal load und Reference load.' },
        { k: 'capture_time_ms', l: 'Erfassungszeit je Punkt [ms]', t: 'int', h: 'Kalibrierung in der Anwendung.' },
        { k: 'stable_time_s', l: 'Geforderter Stillstand [s]', t: 'num', h: 'Kalibrierung in der Klemme; Beckhoff: mindestens 10 s.' },
        { k: 'stable_band_rel', l: 'Stillstandsband (Anteil Nennlast)', t: 'num', h: 'Kalibrierung in der Klemme; 0,0005 = 0,05 %.' },
      ],
    },
    {
      key: 'device',
      title: 'Klemme EL3356 (CoE 0x8000)',
      hint: 'Lastwerte in der Einheit der Klemme. Wird bei Änderung per SDO übertragen.',
      fields: [
        { k: 'write_on_start', l: 'Beim Start schreiben', t: 'bool' },
        { k: 'rated_output_mv_v', l: 'Rated output [mV/V] (0x8000:23)', t: 'num' },
        { k: 'nominal_load', l: 'Nominal load (0x8000:24)', t: 'num' },
        { k: 'zero_balance', l: 'Zero balance [mV/V] (0x8000:25)', t: 'num' },
        { k: 'gravity_of_earth', l: 'Gravity of earth [m/s²] (0x8000:26)', t: 'num' },
        { k: 'scale_factor', l: 'Scale factor (0x8000:27)', t: 'num' },
        { k: 'reference_load', l: 'Reference load (0x8000:28)', t: 'num' },
        { k: 'mode0_enable_filter', l: 'Mode0 enable filter (0x8000:01)', t: 'bool' },
        { k: 'mode0_filter_settings', l: 'Mode0 filter settings (0x8000:11)', t: 'select', o: FILTER_OPTIONS },
        { k: 'mode0_enable_averager', l: 'Mode0 enable averager (0x8000:03)', t: 'bool' },
        { k: 'steady_state_window', l: 'Steady state window [ms] (0x8000:29)', t: 'int' },
        { k: 'steady_state_tolerance', l: 'Steady state tolerance (0x8000:2A)', t: 'int' },
        { k: 'calibration_interval', l: 'Calibration interval (0x8000:31)', t: 'int' },
      ],
    },
    {
      key: 'ethercat',
      title: 'EtherCAT',
      hint: 'Wirkt nach Neustart des Dienstes.',
      fields: [
        { k: 'interface', l: 'Interface', t: 'text' },
        { k: 'slave_name', l: 'Slave-Name', t: 'text' },
        { k: 'cycle_ms', l: 'Zykluszeit [ms]', t: 'int' },
        { k: 'wkc_fault_cycles', l: 'WKC-Fehler bis Wiederanlauf', t: 'int' },
        { k: 'rt_priority', l: 'SCHED_FIFO-Priorität (0 = aus)', t: 'int' },
      ],
    },
    {
      key: 'http',
      title: 'Webserver',
      hint: 'Wirkt nach Neustart des Dienstes.',
      fields: [
        { k: 'bind', l: 'Adresse', t: 'text' },
        { k: 'port', l: 'Port', t: 'int' },
        { k: 'web_root', l: 'Frontend-Verzeichnis', t: 'text' },
        { k: 'threads', l: 'Threads', t: 'int' },
      ],
    },
  ];

  $effect(() => {
    if (open && dialog && !dialog.open) {
      draft = structuredClone($state.snapshot(live.config));
      texts = {};
      errors = [];
      dialog.showModal();
    } else if (!open && dialog?.open) {
      dialog.close();
    }
  });

  function setField(g, f, raw) {
    if (f.t === 'bool' || f.t === 'text') draft[g][f.k] = raw;
    else if (f.t === 'select') draft[g][f.k] = typeof f.o[0][0] === 'number' ? Number(raw) : raw;
    else {
      texts[`${g}.${f.k}`] = raw;
      const t = raw.trim().replace(',', '.');
      const v = t === '' ? NaN : Number(t);
      draft[g][f.k] = Number.isFinite(v) && (f.t !== 'int' || Number.isInteger(v)) ? v : null;
    }
  }

  const NUM_PATTERN = '\\s*-?(\\d+([.,]\\d*)?|[.,]\\d+)([eE][\\-+]?\\d+)?\\s*';
  const INT_PATTERN = '\\s*-?\\d+\\s*';
  const numText = (g, k) => texts[`${g}.${k}`] ?? String(draft[g][k] ?? '').replace('.', ',');

  async function save(e) {
    e.preventDefault();
    saving = true;
    const r = await saveConfig($state.snapshot(draft));
    saving = false;
    if (r.ok) {
      notify('ok', r.restart_required ? 'Gespeichert. Neustart des Dienstes erforderlich.' : 'Gespeichert');
      open = false;
    } else {
      errors = r.errors ?? ['Speichern fehlgeschlagen'];
      dialog.querySelector('.errors')?.scrollIntoView({ block: 'nearest' });
    }
  }
</script>

<dialog bind:this={dialog} onclose={() => (open = false)} aria-labelledby="cfg-title">
  {#if draft}
    <form onsubmit={save}>
      <header>
        <h2 id="cfg-title">Parameter</h2>
        <button type="button" onclick={() => (open = false)}>Schließen</button>
      </header>

      {#if errors.length}
        <ul class="errors" role="alert">
          {#each errors as e (e)}<li>{e}</li>{/each}
        </ul>
      {/if}

      <div class="body">
        {#each groups as g (g.key)}
          <fieldset>
            <legend>{g.title}</legend>
            {#if g.hint}<p class="hint muted">{g.hint}</p>{/if}
            {#each g.fields as f (f.k)}
              {#if f.t === 'bool'}
                <label class="check">
                  <input
                    type="checkbox"
                    checked={draft[g.key][f.k]}
                    onchange={(e) => setField(g.key, f, e.currentTarget.checked)}
                  />
                  {f.l}
                </label>
              {:else if f.t === 'select'}
                <label class="field">
                  <span>{f.l}</span>
                  <select
                    value={draft[g.key][f.k]}
                    onchange={(e) => setField(g.key, f, e.currentTarget.value)}
                  >
                    {#each f.o as [v, label] (v)}
                      <option value={v}>{label}</option>
                    {/each}
                  </select>
                  {#if f.h}<small class="muted">{f.h}</small>{/if}
                </label>
              {:else}
                <label class="field">
                  <span>{f.l}</span>
                  <input
                    type="text"
                    inputmode={f.t === 'int' ? 'numeric' : 'text'}
                    pattern={f.t === 'text' ? undefined : f.t === 'int' ? INT_PATTERN : NUM_PATTERN}
                    value={f.t === 'text' ? draft[g.key][f.k] : numText(g.key, f.k)}
                    oninput={(e) => setField(g.key, f, e.currentTarget.value)}
                    required
                  />
                  {#if f.h}<small class="muted">{f.h}</small>{/if}
                </label>
              {/if}
            {/each}
          </fieldset>
        {/each}
      </div>

      <footer>
        <button type="button" onclick={() => (open = false)}>Verwerfen</button>
        <button type="submit" class="primary" disabled={saving}>
          {saving ? 'Speichert …' : 'Speichern'}
        </button>
      </footer>
    </form>
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
  form {
    display: flex;
    flex-direction: column;
    max-height: calc(100dvh - 1rem);
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
    overflow: auto;
    padding: 0.5rem 1.1rem 1rem;
  }
  fieldset {
    border: 0;
    border-bottom: 1px solid var(--line);
    margin: 0;
    padding: 0.9rem 0 1rem;
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(260px, 1fr));
    gap: 0.75rem 1.25rem;
  }
  fieldset:last-child {
    border-bottom: 0;
  }
  legend {
    font-weight: 650;
    padding: 0;
    float: left;
    width: 100%;
  }
  .hint {
    grid-column: 1 / -1;
    margin: -0.4rem 0 0;
    font-size: 0.85rem;
  }
  .field {
    display: flex;
    flex-direction: column;
    gap: 0.25rem;
    font-size: 0.9rem;
  }
  .field small {
    font-size: 0.78rem;
    line-height: 1.3;
  }
  .check {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    font-size: 0.9rem;
    grid-column: 1 / -1;
  }
  input[type='text'],
  input[type='number'],
  select {
    font: inherit;
    padding: 0.4rem 0.55rem;
    border: 1px solid var(--line);
    border-radius: var(--r-sm);
    background: var(--bg);
    color: var(--ink);
  }
  input[type='checkbox'] {
    width: 1.05rem;
    height: 1.05rem;
    accent-color: var(--accent);
  }
  .errors {
    margin: 0;
    padding: 0.75rem 1.1rem 0.75rem 2.2rem;
    background: color-mix(in srgb, var(--st-fault) 12%, var(--panel));
    color: var(--st-fault);
    border-bottom: 1px solid var(--line);
  }
</style>
