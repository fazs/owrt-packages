<script>
  import { live, displayUnit } from './live.svelte.js';
  import { decimalsFor, fmt, fromG } from './format.js';

  const unit = $derived(displayUnit());
  const dec = $derived(decimalsFor(live.config?.measurement?.tolerance, unit));
  const rows = $derived(live.measurements.slice(0, 50));
  const nowT = $derived(live.sample?.t ?? 0);

  function ago(t) {
    const s = Math.max(0, Math.round(nowT - t));
    if (s < 60) return `vor ${s} s`;
    if (s < 3600) return `vor ${Math.floor(s / 60)} min`;
    return `vor ${Math.floor(s / 3600)} h`;
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
          </tr>
        </thead>
        <tbody>
          {#each rows as r (r.id)}
            <tr>
              <td class="id">{r.id}</td>
              <td class="val">{fmt(fromG(r.value, unit), dec)}</td>
              <td>{fmt(fromG(r.stddev, unit), dec + 1)}</td>
              <td>{Math.round(r.settle_ms)} ms</td>
              <td class="muted">{ago(r.t)}</td>
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
    overflow: auto;
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
    padding: 0.3rem 0 0.4rem 0.75rem;
    border-bottom: 1px solid var(--line);
  }
  td {
    text-align: right;
    padding: 0.32rem 0 0.32rem 0.75rem;
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
  .empty {
    margin: 0;
    max-width: 60ch;
  }
</style>
