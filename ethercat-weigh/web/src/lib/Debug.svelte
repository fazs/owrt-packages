<script>
  import { live, debugApi, notify } from './live.svelte.js';
  import { hex, objKey, enumFor, asNum, formatValue, parseValue } from './coe.js';

  let { open = $bindable(false) } = $props();

  let dialog;
  let tab = $state('pdo');

  // --- Prozessdaten
  let pdo = $state(null); // {pdos, inputs, outputs}
  let pdoError = $state('');
  let sdoVals = $state({}); // objKey -> {value, error}
  let edits = $state({}); // objKey -> Eingabe (Zahl bei Auswahlliste, sonst Text)
  let busy = $state({}); // objKey bzw. 'init:<i>' -> true

  // --- Initialisierungssequenz
  let steps = $state([]); // {index, sub, name, type, value, pre_op_only, target, actual, error}
  let seqBusy = $state(false);

  const isSim = $derived(live.info?.backend === 'simulation');
  const hasOverride = $derived(
    !!pdo?.pdos.some((p) => p.entries.some((e) => e.override !== null && e.override !== undefined)),
  );

  $effect(() => {
    if (open && dialog && !dialog.open) {
      sdoVals = {};
      edits = {};
      loadPdos().then(loadInit); // Auswahllisten der PDO-Zuordnung kommen aus dem Katalog
      dialog.showModal();
    } else if (!open && dialog?.open) {
      dialog.close();
    }
  });

  // Live-Werte aus dem Prozessabbild, solange die PDO-Ansicht offen ist
  $effect(() => {
    if (!open || tab !== 'pdo') return;
    const id = setInterval(loadPdos, 300);
    return () => clearInterval(id);
  });

  async function loadPdos() {
    try {
      pdo = await debugApi.pdos();
      pdoError = '';
    } catch (e) {
      pdoError = e.message;
    }
  }

  async function loadInit() {
    try {
      const j = await debugApi.init();
      steps = j.steps.map((s) => ({ ...s, target: initialTarget(s), actual: null, error: '' }));
    } catch (e) {
      notify('error', `Initialisierungssequenz: ${e.message}`);
    }
  }

  function initialTarget(s) {
    if (enumFor(s.index, s.sub, s.type, pdo?.pdos)) return asNum(s.value);
    return s.type === 'f32' ? String(Number(s.value.toPrecision(7))) : String(asNum(s.value));
  }

  // Eingabe in den zu sendenden JSON-Wert wandeln
  function outValue(raw, type, opts) {
    if (opts) return type === 'bool' ? Number(raw) !== 0 : Number(raw);
    return parseValue(raw, type);
  }

  async function run(key, fn) {
    busy = { ...busy, [key]: true };
    try {
      return await fn();
    } finally {
      const { [key]: _, ...rest } = busy;
      busy = rest;
    }
  }

  // --- PDO-Aktionen
  const entryKey = (e) => objKey(e.index, e.sub);
  const isGap = (e) => e.index === 0;
  // Zugeordnete Steuerbits werden im Prozessabbild übersteuert, alles andere per SDO
  const viaImage = (p, e) => p.rx && p.assigned && e.index === 0x7000;

  function editValue(p, e, opts) {
    const k = entryKey(e);
    if (k in edits) return edits[k];
    const cur = e.override ?? e.value ?? sdoVals[k]?.value;
    if (opts) return asNum(cur ?? 0);
    return cur === null || cur === undefined ? '' : String(asNum(cur));
  }

  async function readEntry(e) {
    const k = entryKey(e);
    await run(k, async () => {
      try {
        const r = await debugApi.sdo({ op: 'read', index: e.index, sub: e.sub, type: e.type });
        sdoVals = { ...sdoVals, [k]: { value: r.value, error: '' } };
      } catch (err) {
        sdoVals = { ...sdoVals, [k]: { value: null, error: err.message } };
      }
    });
  }

  async function sendEntry(p, e, opts) {
    const k = entryKey(e);
    let value;
    try {
      value = outValue(editValue(p, e, opts), e.type, opts);
    } catch (err) {
      notify('error', err.message);
      return;
    }
    await run(k, async () => {
      try {
        if (viaImage(p, e)) {
          await debugApi.override({ sub: e.sub, value: !!value });
        } else {
          await debugApi.sdo({ op: 'write', index: e.index, sub: e.sub, type: e.type, value });
          notify('ok', `${k} geschrieben`);
        }
        await loadPdos();
      } catch (err) {
        notify('error', `${k}: ${err.message}`);
      }
    });
  }

  async function release(e) {
    const k = entryKey(e);
    await run(k, async () => {
      try {
        await debugApi.override({ sub: e.sub, value: null });
        const { [k]: _, ...rest } = edits;
        edits = rest;
        await loadPdos();
      } catch (err) {
        notify('error', err.message);
      }
    });
  }

  async function releaseAll() {
    try {
      await debugApi.override({ clear: true });
      edits = {};
      await loadPdos();
    } catch (err) {
      notify('error', err.message);
    }
  }

  // --- Initialisierungssequenz
  async function readStep(i) {
    const s = steps[i];
    await run(`init:${i}`, async () => {
      try {
        const r = await debugApi.sdo({ op: 'read', index: s.index, sub: s.sub, type: s.type });
        steps[i] = { ...steps[i], actual: r.value, error: '' };
      } catch (err) {
        steps[i] = { ...steps[i], actual: null, error: err.message };
      }
    });
  }

  async function sendStep(i) {
    const s = steps[i];
    const opts = enumFor(s.index, s.sub, s.type, pdo?.pdos);
    await run(`init:${i}`, async () => {
      try {
        const value = outValue(s.target, s.type, opts);
        const r = await debugApi.sdo({ op: 'write', index: s.index, sub: s.sub, type: s.type, value });
        steps[i] = { ...steps[i], actual: r.value, error: '' };
      } catch (err) {
        steps[i] = { ...steps[i], error: err.message };
      }
    });
  }

  async function all(fn, okText) {
    seqBusy = true;
    for (let i = 0; i < steps.length; i++) await fn(i);
    seqBusy = false;
    const failed = steps.filter((s) => s.error).length;
    if (failed) notify('error', `${failed} von ${steps.length} Schritten fehlgeschlagen`);
    else notify('ok', okText);
  }

  async function reinit() {
    try {
      await debugApi.reinit();
      notify('ok', 'Klemme wird neu initialisiert');
    } catch (err) {
      notify('error', err.message);
    }
  }

  async function onClose() {
    open = false;
    if (hasOverride) {
      await releaseAll();
      notify('ok', 'Übersteuerungen aufgehoben');
    }
  }
</script>

{#snippet editor(value, opts, onchange, label)}
  {#if opts}
    <select aria-label={label} {value} onchange={(ev) => onchange(Number(ev.currentTarget.value))}>
      {#each opts as [v, text] (v)}<option value={v}>{text}</option>{/each}
    </select>
  {:else}
    <input
      type="text"
      inputmode="decimal"
      aria-label={label}
      {value}
      oninput={(ev) => onchange(ev.currentTarget.value)}
    />
  {/if}
{/snippet}

<dialog bind:this={dialog} onclose={onClose} aria-labelledby="dbg-title">
  <div class="wrap">
    <header>
      <h2 id="dbg-title">Debug EL3356</h2>
      <div class="tabs" role="tablist">
        <button role="tab" aria-selected={tab === 'pdo'} class:on={tab === 'pdo'} onclick={() => (tab = 'pdo')}
          >Prozessdaten (PDO)</button
        >
        <button role="tab" aria-selected={tab === 'init'} class:on={tab === 'init'} onclick={() => (tab = 'init')}
          >Initialisierungssequenz</button
        >
      </div>
      <button type="button" onclick={() => (open = false)}>Schließen</button>
    </header>

    <div class="body">
      {#if tab === 'pdo'}
        <p class="hint muted">
          Zugeordnete PDOs zeigen den Wert aus dem Prozessabbild (alle 300 ms). Steuerbits (0x7000) werden im
          Prozessabbild übersteuert, bis sie freigegeben werden oder der Dialog geschlossen wird. Nicht zugeordnete
          Einträge werden per SDO gelesen bzw. geschrieben.
          {#if isSim}Simulation: Steuerbits wirken nicht auf den Messwert.{/if}
        </p>
        {#if pdoError}<p class="err" role="alert">{pdoError}</p>{/if}
        {#if pdo}
          {#each pdo.pdos as p (p.index)}
            <section class="pdo">
              <h3>
                <code>{hex(p.index, 4)}</code>
                {p.name}
                <span class="tag">{p.rx ? 'RxPDO · Ausgang' : 'TxPDO · Eingang'}</span>
                <span class="tag" class:ok={p.assigned}>{p.assigned ? 'zugeordnet' : 'nicht zugeordnet'}</span>
                {#if p.excludes?.length}
                  <small class="muted">schließt aus: {p.excludes.map((x) => hex(x, 4)).join(', ')}</small>
                {/if}
              </h3>
              <div class="scroll">
                <table>
                  <thead>
                    <tr>
                      <th scope="col">Objekt</th>
                      <th scope="col">Name</th>
                      <th scope="col">Typ</th>
                      <th scope="col">Bit</th>
                      <th scope="col">Wert</th>
                      <th scope="col">{p.rx ? 'Sollwert' : ''}</th>
                      <th scope="col"><span class="sr">Aktionen</span></th>
                    </tr>
                  </thead>
                  <tbody>
                    {#each p.entries as e, i (i)}
                      {@const k = entryKey(e)}
                      {@const opts = enumFor(e.index, e.sub, e.type, pdo.pdos)}
                      {@const sdo = sdoVals[k]}
                      <tr class:gap={isGap(e)} class:forced={e.override !== null && e.override !== undefined}>
                        <td><code>{isGap(e) ? '—' : k}</code></td>
                        <td>{isGap(e) ? `Lücke (${e.bits} Bit)` : e.name}</td>
                        <td>{e.type}</td>
                        <td class="num">{e.offset ?? '–'}{e.bits > 1 ? `+${e.bits}` : ''}</td>
                        <td class="val">
                          {#if !isGap(e)}
                            {#if e.value !== null}
                              {formatValue(e.value, e.type, opts)}
                            {:else if sdo?.error}
                              <span class="err">{sdo.error}</span>
                            {:else if sdo}
                              {formatValue(sdo.value, e.type, opts)} <small class="muted">SDO</small>
                            {:else}
                              –
                            {/if}
                            {#if e.override !== null && e.override !== undefined}
                              <small class="forced-tag">übersteuert</small>
                            {/if}
                          {/if}
                        </td>
                        <td>
                          {#if p.rx && !isGap(e)}
                            {@render editor(editValue(p, e, opts), opts, (v) => (edits = { ...edits, [k]: v }), `Sollwert ${k}`)}
                          {/if}
                        </td>
                        <td class="act">
                          {#if !isGap(e)}
                            {#if !viaImage(p, e)}
                              <button disabled={busy[k]} onclick={() => readEntry(e)}>Lesen</button>
                            {/if}
                            {#if p.rx}
                              <button class="primary" disabled={busy[k]} onclick={() => sendEntry(p, e, opts)}
                                >Senden</button
                              >
                              {#if e.override !== null && e.override !== undefined}
                                <button disabled={busy[k]} onclick={() => release(e)}>Freigeben</button>
                              {/if}
                            {/if}
                          {/if}
                        </td>
                      </tr>
                    {/each}
                  </tbody>
                </table>
              </div>
            </section>
          {/each}
          <section class="image">
            <h3>Prozessabbild des Slaves</h3>
            <dl>
              <dt>Eingänge</dt>
              <dd><code>{pdo.inputs || '–'}</code></dd>
              <dt>Ausgänge</dt>
              <dd><code>{pdo.outputs || '–'}</code></dd>
            </dl>
          </section>
        {/if}
      {:else}
        <p class="hint muted">
          Sequenz des Übergangs PRE-OP → SAFE-OP mit den Werten der gespeicherten Konfiguration. Sollwerte können vor
          dem Senden geändert werden; sie werden nicht gespeichert. Die PDO-Zuordnung (0x1C12/0x1C13) ist nur in
          PRE-OP beschreibbar; im Betrieb antwortet die Klemme mit SDO-Abbruch. „Neu initialisieren“ durchläuft
          INIT → OP und sendet dabei die gespeicherte Sequenz.
        </p>
        <div class="toolbar">
          <button disabled={seqBusy || !steps.length} onclick={() => all(readStep, 'Alle Schritte gelesen')}>Alle lesen</button>
          <button class="primary" disabled={seqBusy || !steps.length} onclick={() => all(sendStep, 'Sequenz gesendet')}
            >Alle senden</button
          >
          <button disabled={seqBusy} onclick={loadInit}>Sollwerte zurücksetzen</button>
          <span class="spacer"></span>
          <button disabled={seqBusy} onclick={reinit}>Neu initialisieren</button>
        </div>
        <div class="scroll">
          <table>
            <thead>
              <tr>
                <th scope="col" class="num">#</th>
                <th scope="col">Objekt</th>
                <th scope="col">Name</th>
                <th scope="col">Typ</th>
                <th scope="col">Sollwert</th>
                <th scope="col">Istwert</th>
                <th scope="col"><span class="sr">Aktionen</span></th>
              </tr>
            </thead>
            <tbody>
              {#each steps as s, i (i)}
                {@const opts = enumFor(s.index, s.sub, s.type, pdo?.pdos)}
                {@const k = objKey(s.index, s.sub)}
                <tr>
                  <td class="num">{i + 1}</td>
                  <td>
                    <code>{k}</code>
                    {#if s.pre_op_only}<span class="tag">PRE-OP</span>{/if}
                  </td>
                  <td>{s.name}</td>
                  <td>{s.type}</td>
                  <td>{@render editor(s.target, opts, (v) => (steps[i].target = v), `Sollwert ${k}`)}</td>
                  <td class="val">
                    {#if s.error}
                      <span class="err">{s.error}</span>
                    {:else}
                      {formatValue(s.actual, s.type, opts)}
                    {/if}
                  </td>
                  <td class="act">
                    <button disabled={seqBusy || busy[`init:${i}`]} onclick={() => readStep(i)}>Lesen</button>
                    <button class="primary" disabled={seqBusy || busy[`init:${i}`]} onclick={() => sendStep(i)}
                      >Senden</button
                    >
                  </td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    </div>

    {#if tab === 'pdo'}
      <footer>
        <button disabled={!hasOverride} onclick={releaseAll}>Alle Übersteuerungen aufheben</button>
      </footer>
    {/if}
  </div>
</dialog>

<style>
  dialog {
    width: min(1080px, 100vw - 1rem);
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
  .wrap {
    display: flex;
    flex-direction: column;
    max-height: calc(100dvh - 1rem);
  }
  header,
  footer {
    display: flex;
    align-items: center;
    justify-content: space-between;
    flex-wrap: wrap;
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
  .tabs {
    display: inline-flex;
    border: 1px solid var(--line);
    border-radius: var(--r-sm);
    overflow: hidden;
  }
  .tabs button {
    border: 0;
    border-radius: 0;
    color: var(--muted);
  }
  .tabs button + button {
    border-left: 1px solid var(--line);
  }
  .tabs button.on {
    background: var(--ink);
    color: var(--panel);
  }
  .body {
    overflow: auto;
    padding: 0.75rem 1.1rem 1rem;
  }
  .hint {
    margin: 0 0 0.75rem;
    font-size: 0.85rem;
  }
  .toolbar {
    display: flex;
    flex-wrap: wrap;
    gap: 0.5rem;
    margin-bottom: 0.75rem;
  }
  .spacer {
    flex: 1;
  }
  section + section {
    margin-top: 1rem;
  }
  h3 {
    display: flex;
    flex-wrap: wrap;
    align-items: baseline;
    gap: 0.25rem 0.6rem;
    margin: 0 0 0.4rem;
    font-size: 0.95rem;
    font-weight: 650;
  }
  .tag {
    font-size: 0.75rem;
    font-weight: 560;
    padding: 0.05rem 0.4rem;
    border: 1px solid var(--line);
    border-radius: var(--r-sm);
    color: var(--muted);
    white-space: nowrap;
  }
  .tag.ok {
    color: var(--accent);
    border-color: var(--accent);
  }
  .scroll {
    overflow-x: auto;
  }
  table {
    width: 100%;
    border-collapse: collapse;
    font-size: 0.88rem;
  }
  th,
  td {
    text-align: left;
    padding: 0.3rem 0.5rem;
    border-bottom: 1px solid var(--line);
    vertical-align: middle;
  }
  th {
    font-weight: 600;
    color: var(--muted);
    font-size: 0.8rem;
    white-space: nowrap;
  }
  td.num,
  th.num {
    text-align: right;
    white-space: nowrap;
  }
  td.val {
    min-width: 9rem;
  }
  td.act {
    white-space: nowrap;
    text-align: right;
  }
  td.act button {
    padding: 0.25rem 0.6rem;
  }
  td.act button + button {
    margin-left: 0.3rem;
  }
  tr.gap td {
    color: var(--muted);
    font-size: 0.8rem;
  }
  tr.forced td {
    background: color-mix(in srgb, var(--st-settling) 12%, transparent);
  }
  .forced-tag {
    margin-left: 0.4rem;
    color: var(--st-settling);
    font-weight: 600;
  }
  code {
    font-family: ui-monospace, 'SFMono-Regular', Menlo, monospace;
    font-size: 0.85em;
  }
  .err {
    color: var(--st-fault);
  }
  p.err {
    margin: 0 0 0.75rem;
  }
  input[type='text'],
  select {
    font: inherit;
    font-size: 0.88rem;
    padding: 0.25rem 0.45rem;
    border: 1px solid var(--line);
    border-radius: var(--r-sm);
    background: var(--bg);
    color: var(--ink);
    min-width: 7rem;
    max-width: 16rem;
  }
  dl {
    display: grid;
    grid-template-columns: auto 1fr;
    gap: 0.25rem 1rem;
    margin: 0;
    font-size: 0.88rem;
  }
  dt {
    color: var(--muted);
  }
  dd {
    margin: 0;
    overflow-wrap: anywhere;
  }
  .sr {
    position: absolute;
    width: 1px;
    height: 1px;
    overflow: hidden;
    clip-path: inset(50%);
  }
</style>
