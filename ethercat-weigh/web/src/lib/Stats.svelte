<script>
  import { live, resetStatistics, displayUnit } from './live.svelte.js';
  import { decimalsFor, fmt, fmtInt, fromG } from './format.js';

  const unit = $derived(displayUnit());
  const dec = $derived(decimalsFor(live.config?.measurement?.tolerance, unit));
  /** @param {number} v Wert [g] @param {number} d Nachkommastellen @returns {string} in der Anzeigeeinheit */
  const f = (v, d) => fmt(fromG(v, unit), d);
  const ses = $derived(live.statistics?.session);
  const rol = $derived(live.statistics?.rolling);
  const rows = $derived([
    ['Anzahl', (s) => fmtInt(s?.n)],
    ['Mittelwert', (s) => (s?.n ? f(s.mean, dec) : '–')],
    ['Standardabweichung', (s) => (s?.n > 1 ? f(s.stddev, dec + 1) : '–')],
    ['Minimum', (s) => (s?.n ? f(s.min, dec) : '–')],
    ['Maximum', (s) => (s?.n ? f(s.max, dec) : '–')],
    ['Spannweite', (s) => (s?.n ? f(s.range, dec) : '–')],
  ]);

  let confirming = $state(false);
  /** Setzt die Statistik zurück; erster Klick fragt nach (3 s), zweiter führt aus. */
  async function reset() {
    if (!confirming) {
      confirming = true;
      setTimeout(() => (confirming = false), 3000);
      return;
    }
    confirming = false;
    await resetStatistics();
  }
</script>

<section class="panel">
  <div class="panel-head">
    <h2>Statistik</h2>
    <button onclick={reset} class:warn={confirming}>
      {confirming ? 'Wirklich zurücksetzen?' : 'Zurücksetzen'}
    </button>
  </div>
  <table>
    <thead>
      <tr>
        <th scope="col"><span class="muted">{unit}</span></th>
        <th scope="col">Seit Reset</th>
        <th scope="col">Letzte {live.config?.statistics?.rolling_window ?? ''}</th>
      </tr>
    </thead>
    <tbody>
      {#each rows as [label, cell] (label)}
        <tr>
          <th scope="row">{label}</th>
          <td>{cell(ses)}</td>
          <td>{cell(rol)}</td>
        </tr>
      {/each}
    </tbody>
  </table>
</section>

<style>
  table {
    width: 100%;
    border-collapse: collapse;
  }
  th,
  td {
    padding: 0.35rem 0;
    border-bottom: 1px solid var(--line);
  }
  tbody tr:last-child th,
  tbody tr:last-child td {
    border-bottom: 0;
  }
  thead th {
    font-weight: 560;
    font-size: 0.85rem;
    color: var(--muted);
    text-align: right;
  }
  thead th:first-child {
    text-align: left;
  }
  tbody th {
    text-align: left;
    font-weight: 450;
    color: var(--muted);
  }
  td {
    text-align: right;
    font-weight: 560;
  }
  button.warn {
    border-color: var(--st-fault);
    color: var(--st-fault);
  }
</style>
