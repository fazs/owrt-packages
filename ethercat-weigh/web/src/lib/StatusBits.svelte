<script>
  import { live } from './live.svelte.js';

  const s = $derived(live.sample);

  // Statusbits der Klemme (0x6000) und Verbindungszustand; good = grün statt rot, wenn gesetzt
  const bits = $derived([
    { key: 'link', label: 'Link', on: s?.link_ok && s?.wkc_ok, good: true },
    { key: 'steady', label: 'Steady state', on: s?.status?.steady_state, good: true },
    { key: 'cal', label: 'Kalibrierung', on: s?.status?.calibration_in_progress },
    { key: 'inv', label: 'Data invalid', on: s?.status?.data_invalid },
    { key: 'ovr', label: 'Overrange', on: s?.status?.overrange },
    { key: 'err', label: 'Error', on: s?.status?.error },
  ]);

  // Kompakt (nur gesetzte Bits, ohne Link), wenn nicht alle Bits in den freien Platz passen.
  // Die Liste füllt den Rest der Kopfzeile (Breite 0, flex-grow), ihre Breite hängt also nicht
  // vom Inhalt ab. fullWidth ist die Breite aller Bits, gemessen, solange sie angezeigt werden.
  let list;
  let fullWidth = 0;
  let compact = $state(false);

  /** Prüft, ob alle Bits in die verfügbare Breite passen. */
  function fit() {
    if (!list) return;
    if (!compact) fullWidth = list.scrollWidth;
    compact = list.clientWidth < fullWidth;
  }

  $effect(() => {
    const ro = new ResizeObserver(fit);
    ro.observe(list);
    return () => ro.disconnect();
  });

  // Gesetzte Bits sind fett und damit breiter: nach jeder Änderung neu prüfen
  $effect(() => {
    bits;
    requestAnimationFrame(fit);
  });
</script>

<ul class="bits" class:compact bind:this={list} aria-label="Statusbits der Klemme">
  {#each bits as b (b.key)}
    <li class="{b.key}" class:on={b.on} class:good={b.good}>{b.label}</li>
  {/each}
</ul>

<style>
  /* einzeilig, füllt den Rest der Kopfzeile; bei Platzmangel kompakt statt umgebrochen */
  .bits {
    list-style: none;
    display: flex;
    flex-wrap: nowrap;
    gap: 0.35rem;
    /* Breite 0: zählt nicht zur Breite des Titels, füllt nur den freien Rest */
    flex: 1 1 0;
    width: 0;
    min-width: 0;
    overflow: hidden;
    margin: 0;
    padding: 0;
  }
  .bits li {
    white-space: nowrap;
    font-size: 0.78rem;
    padding: 0.1rem 0.5rem;
    border-radius: 999px;
    border: 1px solid var(--line);
    color: var(--muted);
  }
  .bits li.on {
    border-color: var(--st-fault);
    color: var(--st-fault);
    font-weight: 600;
  }
  .bits li.on.good {
    border-color: var(--st-captured);
    color: var(--st-captured);
  }

  /* Kompakt: nur gesetzte Bits; Link zeigt bereits die Verbindungsanzeige */
  .compact li:not(.on),
  .compact li.link {
    display: none;
  }
</style>
