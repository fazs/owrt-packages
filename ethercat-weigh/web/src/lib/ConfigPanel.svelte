<script>
  import { live, saveConfig, notify } from './live.svelte.js';
  import { settingEntries, entryOptions, entryUnit, entryFactor, descEntry, F32_DIGITS } from './coe.js';
  import { MEASUREMENT_COLUMNS } from './constants.js';

  /** @type {{open?: boolean}} Dialog geöffnet (bindbar) */
  let { open = $bindable(false) } = $props();

  let dialog;
  let draft = $state(null);
  // Eingabetext der Zahlenfelder (Komma oder Punkt, Zwischenstände wie "-" oder "0,")
  let texts = $state({});
  let errors = $state([]);
  let saving = $state(false);
  // aufgeklappte Gruppen (id -> true)
  let expanded = $state({});

  const unitOptions = [
    ['mg', 'mg'],
    ['g', 'g'],
    ['kg', 'kg'],
    ['gr', 'gr (Grain)'],
  ];

  // Bedingungen {p: Pfad der Option, h: Hinweis am Feld}; ohne h steht der Hinweis einmal
  // an der Option selbst (off), damit er sich nicht an jedem Feld wiederholt.
  /** Bedingung: Klemmenparameter werden nur bei „Beim Start schreiben“ beim Start übertragen. */
  const ON_START = { p: 'device.write_on_start' };
  /** Bedingung: Stillstandserkennung der Klemme nur relevant, wenn das Steady-state-Bit gefordert ist. */
  const STEADY_ON = {
    p: 'measurement.use_device_steady_state',
    h: 'Für die Messung unwirksam, solange das Steady-state-Bit nicht gefordert ist (Abschnitt Messung).',
  };

  /** Platzhalter für die aus der Gerätebeschreibung erzeugten Gruppen des Abschnitts Klemme. */
  const DEVICE_GROUPS_SLOT = { id: 'device-slot' };

  const staticGroups = [
    {
      key: 'measurement',
      title: 'Messung',
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
        { k: 'display_average', l: 'Glättung des Anzeigewerts [Zyklen]', t: 'int', h: 'Gleitender Mittelwert über die letzten Zyklen; 1 = aus. Wirkt nur auf die Anzeige, nicht auf die Messungserkennung.' },
        {
          k: 'hidden_columns',
          l: 'Spalten der Messwerttabelle',
          t: 'hide',
          o: MEASUREMENT_COLUMNS.map((c) => [c.key, c.label]),
          h: 'Pos.: lückenlose Nummer der angezeigten Zeilen; Nr.: fortlaufende Nummer des Backends.',
        },
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
    DEVICE_GROUPS_SLOT,
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

  /** ESI-Name der Prozessdaten-Variante (AlternativeSmMapping) je device.pdo_variant. */
  const VARIANT_NAMES = { real: 'Standard (REAL)', int32: 'Standard (INT32)' };
  /** Auswahl der Prozessdaten-Variante. */
  const VARIANT_OPTIONS = [
    ['real', 'Value (Real), 0x1A02'],
    ['int32', 'Value (INT32), 0x1A01 – experimentell'],
  ];
  /** Ausgänge 0x7000, die aus der Konfiguration zyklisch geschrieben werden. */
  const OUTPUT_KEYS = ['0x7000:04', '0x7000:02', '0x7000:11'];
  /** Zusätzliche Bedingungen je Klemmenparameter (Abhängigkeiten der Anwendung). */
  const EXTRA_DEPS = { '0x8000:29': [STEADY_ON], '0x8000:2A': [STEADY_ON] };
  /**
   * Gruppen des Abschnitts Klemme in der Reihenfolge des TwinCAT-Dialogs; nicht aufgeführte
   * Parameter landen in „Weitere“, Objekte in objs als eingeklappte Gruppe je Objekt.
   */
  const DEVICE_LAYOUT = [
    {
      id: 'device-filter',
      title: 'Klemme: Filter',
      keys: ['0x8000:01', '0x8000:03', '0x8000:11', '0x8000:02', '0x8000:04', '0x8000:12', '0x8000:13', '0x8000:14', '0x8000:05'],
      hint: 'Modus 0/1 = Sample mode 10,5 kHz bzw. 105 kHz (0x7000:04).',
    },
    {
      id: 'device-scale',
      title: 'Klemme: Kennwerte',
      keys: ['0x8000:23', '0x8000:24', '0x8000:25', '0x8000:26', '0x8000:27', '0x8000:28', '0x8000:21'],
      hint: 'Lastwerte in der Einheit der Klemme.',
    },
    { id: 'device-steady', title: 'Klemme: Steady state', keys: ['0x8000:29', '0x8000:2A'] },
    { id: 'device-cal', title: 'Klemme: Selbstkalibrierung', keys: ['0x8000:31', '0x8000:32'] },
    { id: 'device-ai1', title: 'Klemme: AI-Kanal 1 (nur Spannungsmessung)', obj: 0x8010, collapsed: true },
    { id: 'device-ai2', title: 'Klemme: AI-Kanal 2 (nur Spannungsmessung)', obj: 0x8020, collapsed: true },
  ];

  /**
   * Wert eines Klemmenparameters im Entwurf: typisiertes Feld, sonst device.coe, sonst ESI-Default.
   * @param {object} e Eintrag der Gerätebeschreibung
   * @returns {number|boolean|null}
   */
  function deviceValue(e) {
    if (e.config_key) return draft.device[e.config_key];
    return draft.device.coe?.[e.key] ?? e.default ?? null;
  }

  /**
   * Bedingung aus enable_if der Gerätebeschreibung (mindestens eine Teilbedingung erfüllt).
   * @param {object} e Eintrag
   * @returns {{ok: () => boolean, h: string}|null}
   */
  function enableDep(e) {
    if (!e.enable_if?.length) return null;
    const conds = e.enable_if;
    const texts = conds
      .filter((c) => c.obj)
      .map((c) => {
        const src = descEntry(live.description, c.obj);
        const opt = entryOptions(live.description, src)?.find(([v]) => v === Number(c.eq));
        const what = typeof c.eq === 'boolean' ? (c.eq ? 'aktiv' : 'aus') : (opt?.[1] ?? c.eq);
        return `${src?.label_de ?? src?.name ?? c.obj} (${c.obj}) = ${what}`;
      });
    return {
      ok: () =>
        conds.some((c) =>
          c.variant ? c.variant === VARIANT_NAMES[draft.device.pdo_variant] : Number(deviceValue(descEntry(live.description, c.obj))) === Number(c.eq),
        ),
      h: texts.length ? `Nur wirksam bei ${texts.join(' oder ')}.` : 'In der aktuellen Prozessdaten-Variante unwirksam.',
    };
  }

  /**
   * Felddefinition eines Klemmenparameters.
   * @param {object} e Eintrag der Gerätebeschreibung
   * @param {boolean} [onStart=true] nur bei „Beim Start schreiben“ beim Start übertragen (SDO)
   * @returns {object} Feld (k, l, t, o, h, tip, dep, entry)
   */
  function deviceField(e, onStart = true) {
    const unit = entryUnit(e);
    const opts = entryOptions(live.description, e);
    const t = opts ? 'select' : e.type === 'bool' ? 'bool' : e.type === 'f32' || entryFactor(e) !== 1 ? 'num' : 'int';
    const def = e.default === undefined ? '' : ` · Default ${typeof e.default === 'boolean' ? (e.default ? 'TRUE' : 'FALSE') : Number((e.default * entryFactor(e)).toPrecision(F32_DIGITS)).toLocaleString('de-DE', { maximumFractionDigits: F32_DIGITS })}`;
    const dep = [...(onStart ? [ON_START] : []), ...(EXTRA_DEPS[e.key] ?? [])];
    const en = enableDep(e);
    if (en) dep.push(en);
    return {
      k: e.config_key ?? e.key,
      l: `${e.label_de ?? e.name}${unit ? ` [${unit}]` : ''}`,
      t,
      o: opts,
      h: `${e.key} ${e.name}${def}`,
      tip: e.tooltip_de,
      dep,
      entry: e,
    };
  }

  /** Gruppen des Abschnitts Klemme aus der Gerätebeschreibung. */
  const deviceGroups = $derived.by(() => {
    const entries = settingEntries(live.description);
    const used = new Set();
    const head = {
      id: 'device',
      key: 'device',
      title: 'Klemme EL3356',
      hint: live.description
        ? 'Parameter laut ESI. Wird bei Änderung per SDO übertragen; nicht gesetzte Parameter erhalten den Default der ESI.'
        : 'Gerätebeschreibung nicht geladen.',
      fields: [
        {
          k: 'write_on_start',
          l: 'Beim Start schreiben',
          t: 'bool',
          off: 'Die markierten Werte werden beim Start nicht geschrieben; die Klemme behält ihre gespeicherten Werte. Änderungen werden beim Speichern trotzdem übertragen.',
        },
      ],
    };
    const outputs = OUTPUT_KEYS.map((k) => descEntry(live.description, k)).filter(Boolean);
    const pdo = {
      id: 'device-pdo',
      key: 'device',
      title: 'Klemme: Prozessdaten',
      hint: 'Eine andere Variante initialisiert die Klemme neu. Die Ausgänge 0x7000 werden zyklisch geschrieben.',
      fields: [
        { k: 'pdo_variant', l: 'Messwert', t: 'select', o: VARIANT_OPTIONS },
        ...outputs.map((e) => deviceField(e, false)),
      ],
    };
    const out = live.description ? [head, pdo] : [head];
    for (const g of DEVICE_LAYOUT) {
      const list = g.keys
        ? g.keys.map((k) => entries.find((e) => e.key === k)).filter(Boolean)
        : entries.filter((e) => e.index === g.obj);
      list.forEach((e) => used.add(e.key));
      if (list.length) out.push({ ...g, key: 'device', fields: list.map(deviceField) });
    }
    const rest = entries.filter((e) => !used.has(e.key));
    if (rest.length) out.push({ id: 'device-rest', key: 'device', title: 'Klemme: Weitere', fields: rest.map(deviceField) });
    return out;
  });

  const groups = $derived(staticGroups.flatMap((g) => (g === DEVICE_GROUPS_SLOT ? deviceGroups : [g])));

  $effect(() => {
    if (open && dialog && !dialog.open) {
      draft = structuredClone($state.snapshot(live.config));
      draft.ui.hidden_columns ??= [];
      draft.device.coe ??= {};
      texts = {};
      errors = [];
      dialog.showModal();
    } else if (!open && dialog?.open) {
      dialog.close();
    }
  });

  /**
   * Wert eines Felds im Entwurf (Klemmenparameter ohne typisiertes Feld: device.coe bzw. Default).
   * @param {string} g Gruppe (Abschnitt der Konfiguration)
   * @param {{k: string, entry?: object}} f Felddefinition
   * @returns {any}
   */
  function fieldValue(g, f) {
    return f.entry ? deviceValue(f.entry) : draft[g][f.k];
  }

  /**
   * Schreibt einen Wert in den Entwurf; Klemmenparameter ohne typisiertes Feld als Zahl in device.coe.
   * @param {string} g Gruppe
   * @param {{k: string, entry?: object}} f Felddefinition
   * @param {any} v Wert
   */
  function assign(g, f, v) {
    if (f.entry && !f.entry.config_key) draft.device.coe[f.entry.key] = typeof v === 'boolean' ? Number(v) : v;
    else draft[g][f.k] = v;
  }

  /**
   * Übernimmt eine Eingabe in den Entwurf; Zahlenfelder mit Komma oder Punkt,
   * ungültige Zahlen werden als null gespeichert (Validierung beim Speichern).
   * Klemmenparameter mit Umrechnung (ConvertValue) werden als Rohwert gespeichert.
   * @param {string} g Gruppe (Abschnitt der Konfiguration)
   * @param {{k: string, t: string, o?: Array, entry?: object}} f Felddefinition
   * @param {string|boolean|string[]} raw Eingabewert
   */
  function setField(g, f, raw) {
    if (f.t === 'bool' || f.t === 'text' || f.t === 'hide') assign(g, f, raw);
    else if (f.t === 'select')
      assign(g, f, f.entry?.type === 'bool' && f.entry.config_key ? raw === '1' : typeof f.o[0][0] === 'number' ? Number(raw) : raw);
    else {
      texts[`${g}.${f.k}`] = raw;
      const t = raw.trim().replace(',', '.');
      let v = t === '' ? NaN : Number(t);
      const factor = f.entry ? entryFactor(f.entry) : 1;
      const integral = f.t === 'int' || (f.entry && f.entry.type !== 'f32');
      if (Number.isFinite(v) && factor !== 1) {
        const r = v / factor;
        v = integral && Math.abs(r - Math.round(r)) < ROUND_EPS ? Math.round(r) : r;
      }
      assign(g, f, Number.isFinite(v) && (!integral || Number.isInteger(v)) ? v : null);
    }
  }

  /** Toleranz beim Runden umgerechneter Ganzzahlen (z. B. 0,1 s / 0,01 s). */
  const ROUND_EPS = 1e-9;
  const NUM_PATTERN = '\\s*-?(\\d+([.,]\\d*)?|[.,]\\d+)([eE][\\-+]?\\d+)?\\s*';
  const INT_PATTERN = '\\s*-?\\d+\\s*';
  /**
   * Anzeigetext eines Zahlenfelds: letzte Eingabe, sonst Wert mit Dezimalkomma.
   * Gleitkommafelder zeigen immer Nachkommastellen ("20,0"), damit Bruchteile erkennbar zulässig sind.
   * @param {string} g Gruppe
   * @param {{k: string, t: string, entry?: object}} f Felddefinition
   * @returns {string}
   */
  function numText(g, f) {
    const typed = texts[`${g}.${f.k}`];
    if (typed !== undefined) return typed;
    let v = fieldValue(g, f);
    if (f.entry && v !== null && v !== undefined) v = Number((v * entryFactor(f.entry)).toPrecision(F32_DIGITS));
    const s = String(v ?? '');
    return (f.t === 'num' && /^-?\d+$/.test(s) ? `${s}.0` : s).replace('.', ',');
  }

  /**
   * Bedingungen eines Felds, die im Entwurf nicht erfüllt sind.
   * @param {{dep?: Array<{p?: string, ok?: () => boolean, h?: string}>}} f Felddefinition
   * @returns {Array<{p?: string, h?: string}>}
   */
  function unmet(f) {
    return (f.dep ?? []).filter((d) => {
      if (d.ok) return !d.ok();
      const [g, k] = d.p.split('.');
      return !draft[g][k];
    });
  }

  /**
   * Blendet einen Eintrag einer Liste ausgeblendeter Elemente ein oder aus.
   * @param {string} g Gruppe
   * @param {{k: string, t: string}} f Felddefinition (Typ 'hide')
   * @param {string} key Element
   * @param {boolean} visible true = anzeigen
   */
  function setVisible(g, f, key, visible) {
    const hidden = draft[g][f.k].filter((k) => k !== key);
    setField(g, f, visible ? hidden : [...hidden, key]);
  }

  /**
   * Speichert den Entwurf; bei Fehlern bleibt der Dialog mit Fehlerliste offen.
   * @param {SubmitEvent} e Formularereignis
   */
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

{#snippet notes(f)}
  {#each unmet(f).filter((d) => d.h) as d (d.h)}<small class="dep">{d.h}</small>{/each}
{/snippet}

<dialog bind:this={dialog} onclose={() => (open = false)} aria-labelledby="cfg-title">
  {#if draft}
    <form onsubmit={save}>
      <header>
        <h2 id="cfg-title">Einstellungen</h2>
        <button type="button" onclick={() => (open = false)}>Schließen</button>
      </header>

      {#if errors.length}
        <ul class="errors" role="alert">
          {#each errors as e (e)}<li>{e}</li>{/each}
        </ul>
      {/if}

      <div class="body">
        {#each groups as g (g.id ?? g.key)}
          <fieldset class:collapsed={g.collapsed && !expanded[g.id]}>
            <legend>
              {#if g.collapsed}
                <button type="button" class="toggle" onclick={() => (expanded[g.id] = !expanded[g.id])} aria-expanded={!!expanded[g.id]}>
                  {expanded[g.id] ? '▾' : '▸'} {g.title}
                </button>
              {:else}{g.title}{/if}
            </legend>
            {#if g.hint}<p class="hint muted">{g.hint}</p>{/if}
            {#each g.collapsed && !expanded[g.id] ? [] : g.fields as f (f.k)}
              {#if f.t === 'bool'}
                <label class="check" class:inactive={unmet(f).length > 0} title={f.tip}>
                  <input
                    type="checkbox"
                    checked={!!fieldValue(g.key, f)}
                    onchange={(e) => setField(g.key, f, e.currentTarget.checked)}
                  />
                  <span>{f.l}</span>
                  {#if f.h}<small class="muted">{f.h}</small>{/if}
                  {@render notes(f)}
                  {#if f.off && !draft[g.key][f.k]}<small class="dep">{f.off}</small>{/if}
                </label>
              {:else if f.t === 'hide'}
                <fieldset class="checks">
                  <legend>{f.l}</legend>
                  {#each f.o as [key, label] (key)}
                    <label class="check">
                      <input
                        type="checkbox"
                        checked={!draft[g.key][f.k].includes(key)}
                        onchange={(e) => setVisible(g.key, f, key, e.currentTarget.checked)}
                      />
                      {label}
                    </label>
                  {/each}
                  {#if f.h}<small class="muted">{f.h}</small>{/if}
                </fieldset>
              {:else if f.t === 'select'}
                <label class="field" class:inactive={unmet(f).length > 0} title={f.tip}>
                  <span>{f.l}</span>
                  <select
                    value={typeof fieldValue(g.key, f) === 'boolean' ? Number(fieldValue(g.key, f)) : fieldValue(g.key, f)}
                    onchange={(e) => setField(g.key, f, e.currentTarget.value)}
                  >
                    {#each f.o as [v, label] (v)}
                      <option value={v}>{label}</option>
                    {/each}
                  </select>
                  {#if f.h}<small class="muted">{f.h}</small>{/if}
                  {@render notes(f)}
                </label>
              {:else}
                <label class="field" class:inactive={unmet(f).length > 0} title={f.tip}>
                  <span>{f.l}</span>
                  <input
                    type="text"
                    inputmode={f.t === 'int' ? 'numeric' : 'text'}
                    pattern={f.t === 'text' ? undefined : f.t === 'int' ? INT_PATTERN : NUM_PATTERN}
                    value={f.t === 'text' ? draft[g.key][f.k] : numText(g.key, f)}
                    oninput={(e) => setField(g.key, f, e.currentTarget.value)}
                    required
                  />
                  {#if f.h}<small class="muted">{f.h}</small>{/if}
                  {@render notes(f)}
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
    margin-left: 0;
    margin-right: 0;
    margin-top: 0.5rem;
    padding: 0.9rem 0 1rem;
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(260px, 1fr));
    gap: 0.75rem 1.25rem;
  }
  fieldset:last-child {
    border-bottom: 0;
  }
  fieldset.collapsed {
    padding-bottom: 0.4rem;
  }
  .toggle {
    font: inherit;
    font-weight: inherit;
    padding: 0;
    border: 0;
    background: none;
    color: inherit;
    cursor: pointer;
  }
  /* zusammengehörige Checkboxen als Kasten */
  fieldset.checks {
    grid-column: 1 / -1;
    display: flex;
    flex-wrap: wrap;
    gap: 0.45rem 1.2rem;
    padding: 0.6rem 0.8rem 0.7rem;
    border: 1px solid var(--line);
    border-radius: var(--r-sm);
    background: color-mix(in srgb, var(--accent) 6%, transparent);
  }
  fieldset.checks legend {
    float: none;
    width: auto;
    padding: 0 0.3rem;
    font-weight: normal;
    font-size: 0.9rem;
  }
  fieldset.checks .check {
    grid-column: auto;
  }
  fieldset.checks small {
    flex-basis: 100%;
    font-size: 0.78rem;
  }
  /* Feld durch eine ausgeschaltete Option unwirksam */
  .inactive {
    padding-left: 0.6rem;
    border-left: 2px dashed var(--line);
  }
  .inactive > span,
  .inactive > input,
  .inactive > select {
    opacity: 0.5;
  }
  .dep {
    flex-basis: 100%;
    font-size: 0.78rem;
    line-height: 1.3;
    color: var(--muted);
    font-style: italic;
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
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem;
    font-size: 0.9rem;
    grid-column: 1 / -1;
  }
  input[type='text'],
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
