<script>
  import { live, displayUnit, deleteMeasurement, outsideLimits } from './live.svelte.js';
  import { decimalsFor, fmt, fromG } from './format.js';
  import {
    CONFIRM_MS,
    MEASUREMENT_COLUMNS,
    S_PER_H,
    S_PER_MIN,
    VISIBLE_MEASUREMENTS,
  } from './constants.js';

  const unit = $derived(displayUnit());
  const dec = $derived(decimalsFor(live.config?.measurement?.tolerance, unit));
  const nowT = $derived(live.sample?.t ?? 0);
  // pos: lückenlose Nummer der angezeigten Zeilen, älteste = 1 (Liste ist neueste zuerst)
  const rows = $derived(
    live.measurements.slice(0, VISIBLE_MEASUREMENTS).map((m, i, shown) => ({ ...m, pos: shown.length - i })),
  );
  const columns = $derived(
    MEASUREMENT_COLUMNS.filter((c) => !(live.config?.ui?.hidden_columns ?? []).includes(c.key)),
  );

  let sortKey = $state('id');
  let sortDir = $state('desc');

  /**
   * Sortiert nach @p key; erneuter Klick auf dieselbe Spalte kehrt die Richtung um.
   * @param {{key: string, dir: 'asc'|'desc'}} col Spalte aus MEASUREMENT_COLUMNS
   */
  function sortBy(col) {
    if (sortKey === col.key) {
      sortDir = sortDir === 'asc' ? 'desc' : 'asc';
    } else {
      sortKey = col.key;
      sortDir = col.dir;
    }
  }

  // erst auf VISIBLE_MEASUREMENTS kürzen, dann sortieren: die Tabelle zeigt immer die letzten Messungen
  const sorted = $derived.by(() => {
    // ausgeblendete Sortierspalte: Messreihenfolge, neueste zuerst
    const shown = columns.some((c) => c.key === sortKey);
    const key = shown ? sortKey : 'id';
    const sign = shown && sortDir === 'asc' ? 1 : -1;
    // id als zweites Kriterium, damit gleiche Werte in Messreihenfolge bleiben
    return [...rows].sort((a, b) => sign * (a[key] - b[key]) || b.id - a.id);
  });

  /**
   * @param {string} key Spalte
   * @returns {'ascending'|'descending'|'none'} Wert für aria-sort
   */
  function ariaSort(key) {
    if (sortKey !== key) return 'none';
    return sortDir === 'asc' ? 'ascending' : 'descending';
  }

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

{#snippet head(col)}
  <th scope="col" aria-sort={ariaSort(col.key)}>
    <button class="sort" onclick={() => sortBy(col)}>
      {col.key === 'value' ? `${col.label} [${unit}]` : col.label}<span
        class="arrow"
        aria-hidden="true">{sortKey === col.key ? (sortDir === 'asc' ? '▲' : '▼') : ''}</span
      >
    </button>
  </th>
{/snippet}

{#snippet cell(key, r)}
  {#if key === 'pos' || key === 'id'}
    <td class="num">{r[key]}</td>
  {:else if key === 'value'}
    <td class="val">{fmt(fromG(r.value, unit), dec)}</td>
  {:else if key === 'stddev'}
    <td>{fmt(fromG(r.stddev, unit), dec + 1)}</td>
  {:else if key === 'settle_ms'}
    <td>{Math.round(r.settle_ms)} ms</td>
  {:else if key === 't'}
    <td class="muted">{ago(r.t)}</td>
  {/if}
{/snippet}

<section class="panel measurements">
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
            {#each columns as col (col.key)}{@render head(col)}{/each}
            <th scope="col"><span class="sr">Löschen</span></th>
          </tr>
        </thead>
        <tbody>
          {#each sorted as r (r.id)}
            {@const out = outsideLimits(r.value)}
            <tr class:out class:confirm={confirmId === r.id} title={out ? 'Außerhalb der Grenzen (Verteilung)' : undefined}>
              {#each columns as col (col.key)}{@render cell(col.key, r)}{/each}
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
  /* füllt die rechte Spalte bis zur Unterkante der linken; die Tabelle scrollt darin */
  .measurements {
    flex: 1 1 auto;
    display: flex;
    flex-direction: column;
  }
  .scroll {
    flex: 1 1 0; /* Höhe kommt aus der linken Spalte, nicht aus der Zeilenzahl */
    min-height: 200px;
    overflow-y: auto;
    overflow-x: hidden; /* Tabelle ist 100 % breit; Rundungsreste sonst als Scrollleiste */
  }
  /* einspaltig (Umbruch wie in App.svelte): keine linke Spalte, an der sich die Höhe ausrichten kann */
  @media (max-width: 960px) {
    .scroll {
      flex: none;
      max-height: 360px;
    }
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
  button.sort {
    all: unset;
    cursor: pointer;
  }
  button.sort:hover {
    color: var(--ink);
  }
  button.sort:focus-visible {
    outline: 2px solid var(--accent);
  }
  .arrow {
    display: inline-block;
    width: 1em; /* reserviert Platz, damit die Spalte beim Umschalten nicht springt */
    font-size: 0.7em;
    text-align: center;
  }
  td {
    text-align: right;
    padding: 0.32rem 0 0.32rem 0.6rem;
    border-bottom: 1px solid var(--line);
  }
  th:first-child,
  td:first-child {
    text-align: left;
    padding-left: 0;
  }
  td.num {
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
  tr.out td {
    background: color-mix(in srgb, var(--st-fault) 16%, transparent);
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
