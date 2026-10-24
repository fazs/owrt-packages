<script>
  import { live, displayUnit, deleteMeasurement } from './live.svelte.js';
  import { decimalsFor, fmt, fromG } from './format.js';
  import { CONFIRM_MS, S_PER_H, S_PER_MIN, SNAPSHOT_MEASUREMENTS } from './constants.js';

  /** angezeigte Zeilen (wie der Snapshot des Backends) */
  const VISIBLE_ROWS = SNAPSHOT_MEASUREMENTS;

  const unit = $derived(displayUnit());
  const dec = $derived(decimalsFor(live.config?.measurement?.tolerance, unit));
  const rows = $derived(live.measurements.slice(0, VISIBLE_ROWS));
  const nowT = $derived(live.sample?.t ?? 0);

  let confirmId = $state(null); // Messung, deren Löschen auf Bestätigung wartet
  let confirmTimer;
  /**
   * Löschen mit Bestätigung: erster Klick markiert die Zeile (CONFIRM_MS), zweiter löscht.
   * @param {number} id Nummer der Messung
   */
  function del(id) {
    clearTimeout(confirmTimer);
    if (confirmId !== id) {
      confirmId = id;
      confirmTimer = setTimeout(() => (confirmId = null), CONFIRM_MS);
      return;
    }
    confirmId = null;
    deleteMeasurement(id);
  }

  /**
   * Alter einer Messung relativ zum letzten Sample.
   * @param {number} t Zeitpunkt der Messung [s]
   * @returns {string} z. B. "vor 12 s"
   */
  function ago(t) {
    const s = Math.max(0, Math.round(nowT - t));
    if (s < S_PER_MIN) return `vor ${s} s`;
    if (s < S_PER_H) return `vor ${Math.floor(s / S_PER_MIN)} min`;
    return `vor ${Math.floor(s / S_PER_H)} h`;
  }
</script>

<section class="panel">
  <div class="panel-head">
    <h2>Messungen</h2>
    <a href="/api/measurements.csv?unit={unit}" download>CSV herunterladen</a>
  </div>
  {#if rows.length === 0}
    <p class="muted empty">
      Noch keine Messung. Last auflegen; die Messung wird übernommen, sobald der Wert über die
      Fensterzeit stabil ist.
    </p>
  {:else}
    <div class="scroll">
      <table>
        <thead>
          <tr>
            <th scope="col">Nr.</th>
            <th scope="col">Wert [{unit}]</th>
            <th scope="col">s</th>
            <th scope="col">Einschwingen</th>
            <th scope="col">Zeit</th>
            <th scope="col"><span class="sr">Löschen</span></th>
          </tr>
        </thead>
        <tbody>
          {#each rows as r (r.id)}
            <tr class:confirm={confirmId === r.id}>
              <td class="id">{r.id}</td>
              <td class="val">{fmt(fromG(r.value, unit), dec)}</td>
              <td>{fmt(fromG(r.stddev, unit), dec + 1)}</td>
              <td>{Math.round(r.settle_ms)} ms</td>
              <td class="muted">{ago(r.t)}</td>
              <td class="del">
                <button
                  class:warn={confirmId === r.id}
                  title={confirmId === r.id ? 'Zum Löschen erneut klicken' : 'Messung löschen'}
                  aria-label={confirmId === r.id ? `Messung ${r.id} wirklich löschen` : `Messung ${r.id} löschen`}
                  onclick={() => del(r.id)}>×</button
                >
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
  {/if}
</section>

<style>
  .scroll {
    max-height: 360px;
    overflow-y: auto;
    overflow-x: hidden; /* Tabelle ist 100 % breit; Rundungsreste sonst als Scrollleiste */
  }
  table {
    width: 100%;
    border-collapse: collapse;
  }
  th {
    position: sticky;
    top: 0;
    background: var(--panel);
    font-weight: 560;
    font-size: 0.85rem;
    color: var(--muted);
    text-align: right;
    padding: 0.3rem 0 0.4rem 0.6rem;
    border-bottom: 1px solid var(--line);
  }
  td {
    text-align: right;
    padding: 0.32rem 0 0.32rem 0.6rem;
    border-bottom: 1px solid var(--line);
  }
  th:first-child,
  td.id {
    text-align: left;
    padding-left: 0;
  }
  td.id {
    color: var(--muted);
  }
  td.val {
    font-weight: 620;
  }
  td.del {
    width: 1%;
    padding: 0.15rem 0 0.15rem 0.35rem;
  }
  td.del button {
    width: 1.6rem;
    padding: 0.1rem 0;
    font-size: 0.85rem;
    line-height: 1.3;
    color: var(--muted);
    border-color: transparent;
    background: transparent;
  }
  td.del button:hover {
    border-color: var(--line);
    color: var(--ink);
  }
  td.del button.warn {
    border-color: var(--st-fault);
    background: var(--st-fault);
    color: #fff;
  }
  tr.confirm td {
    background: color-mix(in srgb, var(--st-fault) 8%, transparent);
  }
  .sr {
    position: absolute;
    width: 1px;
    height: 1px;
    overflow: hidden;
    clip-path: inset(50%);
  }
  .empty {
    margin: 0;
    max-width: 60ch;
  }
</style>
