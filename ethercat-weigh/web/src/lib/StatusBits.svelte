<script>
  import { live } from './live.svelte.js';

  const s = $derived(live.sample);

  // Status bits of the terminal (0x6000) and connection state; good = green instead of red when set
  const bits = $derived([
    { key: 'link', label: 'Link', on: s?.link_ok && s?.wkc_ok, good: true },
    { key: 'steady', label: 'Steady state', on: s?.status?.steady_state, good: true },
    { key: 'cal', label: 'Kalibrierung', on: s?.status?.calibration_in_progress },
    { key: 'inv', label: 'Data invalid', on: s?.status?.data_invalid },
    { key: 'ovr', label: 'Overrange', on: s?.status?.overrange },
    { key: 'err', label: 'Error', on: s?.status?.error },
  ]);

  // Compact (only set bits, without Link) if not all bits fit into the free space.
  // The list fills the rest of the header (width 0, flex-grow), so its width does not depend
  // on the content. fullWidth is the width of all bits, measured while they are shown.
  let list;
  let fullWidth = 0;
  let compact = $state(false);

  /** Checks whether all bits fit into the available width. */
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

  // Set bits are bold and therefore wider: check again after every change
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
  /* single line, fills the rest of the header; compact instead of wrapping when space is short */
  .bits {
    list-style: none;
    display: flex;
    flex-wrap: nowrap;
    gap: 0.35rem;
    /* Width 0: does not count towards the width of the title, only fills the remaining space */
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

  /* Compact: only set bits; Link is already shown by the connection indicator */
  .compact li:not(.on),
  .compact li.link {
    display: none;
  }
</style>
