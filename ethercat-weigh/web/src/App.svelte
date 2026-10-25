<script>
  import { onMount } from 'svelte';
  import {
    live,
    connect,
    refreshInfo,
    tare,
    clearTare,
    setUnit,
    displayUnit,
    simLoad,
    simDefaultGrams,
  } from './lib/live.svelte.js';
  import { INFO_REFRESH_MS } from './lib/constants.js';
  import { UNITS } from './lib/format.js';
  import Calibration from './lib/Calibration.svelte';
  import Readout from './lib/Readout.svelte';
  import Trend from './lib/Trend.svelte';
  import Histogram from './lib/Histogram.svelte';
  import Stats from './lib/Stats.svelte';
  import Measurements from './lib/Measurements.svelte';
  import ConfigPanel from './lib/ConfigPanel.svelte';
  import Debug from './lib/Debug.svelte';

  let configOpen = $state(false);
  let calOpen = $state(false);
  let debugOpen = $state(false);
  const isSim = $derived(live.info?.backend === 'simulation');
  const hasTare = $derived(live.sample?.tare != null);

  /** Nachkommastellen der Zykluszeit in der Fußzeile */
  const CYCLE_MS_DECIMALS = 2;

  let simGrams = $state(null); // Vorbelegung aus der Nennlast, sobald die Konfiguration da ist
  $effect(() => {
    if (simGrams === null && live.config) simGrams = simDefaultGrams();
  });

  onMount(() => {
    connect();
    const id = setInterval(refreshInfo, INFO_REFRESH_MS);
    return () => clearInterval(id);
  });

  const info = $derived(live.info);
  const conn = $derived(
    !live.connected
      ? { cls: 'bad', text: 'Keine Verbindung zum Gerät' }
      : info?.backend_state === 'error'
        ? { cls: 'bad', text: `EtherCAT: ${info.backend_error}` }
        : info?.backend_state === 'init'
          ? { cls: 'warn', text: 'EtherCAT wird initialisiert' }
          : {
              cls: 'ok',
              text:
                info?.backend === 'simulation'
                  ? 'Simulation'
                  : `EtherCAT ${info?.interface ?? ''}, ${info?.cycle_ms ?? '–'} ms`,
            },
  );
</script>

<header class="top">
  <div class="title">
    <h1>Wägung</h1>
    <span class="conn {conn.cls}"><span class="led" aria-hidden="true"></span>{conn.text}</span>
  </div>
  <div class="actions">
    <div class="units" role="radiogroup" aria-label="Anzeigeeinheit">
      {#each UNITS as u (u.id)}
        <button
          role="radio"
          aria-checked={displayUnit() === u.id}
          class:on={displayUnit() === u.id}
          title={u.title ?? u.label}
          onclick={() => setUnit(u.id)}>{u.label}</button
        >
      {/each}
    </div>
    <button
      class="primary"
      class:on={hasTare}
      aria-pressed={hasTare}
      title={hasTare ? 'Tara aktiv; erneut klicken löscht sie' : 'Aktuellen Wert als Tara übernehmen'}
      onclick={hasTare ? clearTare : tare}
      disabled={!live.connected}>Tara</button
    >
    <button onclick={() => (calOpen = true)} disabled={!live.config}>Kalibrieren</button>
    <button onclick={() => (configOpen = true)} disabled={!live.config}>Parameter</button>
    <button onclick={() => (debugOpen = true)} disabled={!live.connected}>Debug</button>
  </div>
</header>

{#if live.calibration?.active && !calOpen}
  <p class="banner">
    Kalibrierung aktiv; Messungen sind bis zum Abschluss gesperrt.
    <button class="inline" onclick={() => (calOpen = true)}>Fortsetzen</button>
  </p>
{/if}

{#if live.restartRequired}
  <p class="banner">Geänderte EtherCAT- oder Webserver-Parameter werden nach einem Neustart des Dienstes wirksam.</p>
{/if}

<main>
  <div class="col-main">
    <Readout />
    <Trend />
    <Histogram />
  </div>
  <div class="col-side">
    <Stats />
    {#if isSim}
      <section class="panel">
        <div class="panel-head">
          <h2>Simulation</h2>
        </div>
        <form class="sim"
              onsubmit={(e) => {
              e.preventDefault();
              simLoad(Number(simGrams));
            }}>
          <span class="muted">aufgelegte Masse [g] </span>
          <input type="number" step="0.1" min="0" bind:value={simGrams} />
          <button type="submit">Auflegen</button>
          <button type="button" onclick={() => simLoad(0)}>Entlasten</button>
          <button type="button" onclick={() => simLoad(null)}>Automatik</button>
        </form>
      </section>
    {/if}
    <Measurements />
  </div>
</main>

<footer class="foot muted">
  {#if info}
    <span>ethercat-weigh {info.version}</span>
    <span>Zyklen {info.cycles?.toLocaleString('de-DE')}</span>
    <span>Überläufe {info.overruns}</span>
    <span>max. Zykluszeit {info.max_cycle_ms?.toFixed(CYCLE_MS_DECIMALS)} ms</span>
  {/if}
</footer>

{#if live.notice}
  <div class="toast {live.notice.kind}" role="status">{live.notice.text}</div>
{/if}

<ConfigPanel bind:open={configOpen} />
<Calibration bind:open={calOpen} />
<Debug bind:open={debugOpen} />

<style>
  .top {
    display: flex;
    align-items: center;
    justify-content: space-between;
    flex-wrap: wrap;
    gap: 0.75rem;
    padding: 0.9rem clamp(0.75rem, 3vw, 1.75rem);
    border-bottom: 1px solid var(--line);
    background: var(--panel);
  }
  .title {
    display: flex;
    align-items: baseline;
    flex-wrap: wrap;
    gap: 0.25rem 1rem;
  }
  h1 {
    margin: 0;
    font-size: 1.35rem;
    font-stretch: 85%;
    font-weight: 680;
  }
  .conn {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
    font-size: 0.88rem;
    color: var(--muted);
  }
  .led {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    background: var(--st-idle);
  }
  .conn.ok .led {
    background: var(--st-captured);
  }
  .conn.warn .led {
    background: var(--st-settling);
  }
  .conn.bad {
    color: var(--st-fault);
  }
  .conn.bad .led {
    background: var(--st-fault);
  }
  .actions {
    display: flex;
    flex-wrap: wrap;
    gap: 0.5rem;
  }
  .units {
    display: inline-flex;
    border: 1px solid var(--line);
    border-radius: var(--r-sm);
    overflow: hidden;
  }
  .units button {
    border: 0;
    border-radius: 0;
    padding: 0.45rem 0.7rem;
    min-width: 2.8rem;
    color: var(--muted);
  }
  .units button + button {
    border-left: 1px solid var(--line);
  }
  .units button.on {
    background: var(--ink);
    color: var(--panel);
  }
  /* aktive Tara: invertiert wie die gewählte Einheit */
  .actions button.primary.on {
    background: var(--ink);
    border-color: var(--ink);
    color: var(--panel);
  }
  .banner .inline {
    margin-left: 0.5rem;
    padding: 0.2rem 0.6rem;
  }

  .banner {
    margin: 0;
    padding: 0.6rem clamp(0.75rem, 3vw, 1.75rem);
    background: color-mix(in srgb, var(--st-settling) 16%, var(--panel));
    border-bottom: 1px solid var(--line);
  }

  main {
    display: grid;
    grid-template-columns: minmax(0, 1.55fr) minmax(0, 1fr);
    gap: 1rem;
    padding: 1rem clamp(0.75rem, 3vw, 1.75rem);
    width: 100%; /* als Flex-Element von #app sonst nur so breit wie der Inhalt */
    max-width: 1480px;
    margin: 0 auto;
  }
  /* zweispaltig: restliche Bildschirmhöhe füllen; Verlauf und Verteilung wachsen mit,
     reicht die Höhe nicht, ragt die Fußzeile über den Rand */
  @media (min-width: 961px) {
    main {
      flex: 1 0 auto;
    }
  }
  .col-main,
  .col-side {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    min-width: 0;
  }
  @media (max-width: 960px) {
    main {
      grid-template-columns: minmax(0, 1fr);
    }
  }

  .foot {
    display: flex;
    flex-wrap: wrap;
    justify-content: center;
    gap: 0.25rem 1.5rem;
    font-size: 0.8rem;
    padding: 0.5rem 1rem 1.5rem;
  }

  .toast {
    position: fixed;
    left: 50%;
    bottom: 1.25rem;
    transform: translateX(-50%);
    background: var(--ink);
    color: var(--panel);
    padding: 0.6rem 1rem;
    border-radius: var(--r-sm);
    max-width: calc(100vw - 2rem);
    font-weight: 520;
  }
  .toast.error {
    background: var(--st-fault);
    color: #fff;
  }
</style>
