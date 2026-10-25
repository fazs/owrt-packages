<script>
  import { live, resetStatistics, displayUnit, outsideLimits } from './live.svelte.js';
  import { decimalsFor, fmt, fmtInt, fromG } from './format.js';
  import { CONFIRM_MS, VISIBLE_MEASUREMENTS } from './constants.js';

  const unit = $derived(displayUnit());
  const dec = $derived(decimalsFor(live.config?.measurement?.tolerance, unit));
  /** @param {number} v Wert [g] @param {number} d Nachkommastellen @returns {string} in der Anzeigeeinheit */
  const f = (v, d) => fmt(fromG(v, unit), d);
  const ses = $derived(live.statistics?.session);
  const rol = $derived(live.statistics?.rolling);

  /**
   * Kennwerte wie das Backend (Standardabweichung mit n − 1).
   * @param {number[]} v Werte [g]
   * @returns {{n: number, mean: number, stddev: number, min: number, max: number, range: number}}
   */
  function summarize(v) {
    const n = v.length;
    const mean = n ? v.reduce((a, x) => a + x, 0) / n : 0;
    const stddev = n > 1 ? Math.sqrt(v.reduce((a, x) => a + (x - mean) ** 2, 0) / (n - 1)) : 0;
    const min = n ? Math.min(...v) : 0;
    const max = n ? Math.max(...v) : 0;
    return { n, mean, stddev, min, max, range: max - min };
  }

  // in der Messwerttabelle angezeigte Messungen innerhalb der Grenzen; null, solange die Grenzen dort nichts ausschließen
  const inRange = $derived.by(() => {
    const shown = live.measurements.slice(0, VISIBLE_MEASUREMENTS);
    const kept = shown.filter((m) => !outsideLimits(m.value));
    return kept.length < shown.length ? summarize(kept.map((m) => m.value)) : null;
  });
  const rows = $derived([
    ['Anzahl', (s) => fmtInt(s?.n)],
    ['Mittelwert', (s) => (s?.n ? f(s.mean, dec) : '–')],
    ['Standardabweichung', (s) => (s?.n > 1 ? f(s.stddev, dec + 1) : '–')],
    ['Minimum', (s) => (s?.n ? f(s.min, dec) : '–')],
    ['Maximum', (s) => (s?.n ? f(s.max, dec) : '–')],
    ['Spannweite', (s) => (s?.n ? f(s.range, dec) : '–')],
  ]);

  let confirming = $state(false);
  /** Setzt die Statistik zurück; erster Klick fragt nach (CONFIRM_MS), zweiter führt aus. */
  async function reset() {
    if (!confirming) {
      confirming = true;
      setTimeout(() => (confirming = false), CONFIRM_MS);
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
        {#if inRange}
          <th scope="col" title="Angezeigte Messungen innerhalb der Grenzen (nicht rot hinterlegt)">Im Bereich</th>
        {/if}
      </tr>
    </thead>
    <tbody>
      {#each rows as [label, cell] (label)}
        <tr>
          <th scope="row">{label}</th>
          <td>{cell(ses)}</td>
          <td>{cell(rol)}</td>
          {#if inRange}<td class="filtered">{cell(inRange)}</td>{/if}
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
  .filtered {
    color: var(--accent);
  }
  button.warn {
    border-color: var(--st-fault);
    color: var(--st-fault);
  }
</style>
