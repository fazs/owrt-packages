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
  } from './lib/live.svelte.js';
  import { UNITS } from './lib/format.js';
  import Calibration from './lib/Calibration.svelte';
  import Readout from './lib/Readout.svelte';
  import Trend from './lib/Trend.svelte';
  import Stats from './lib/Stats.svelte';
  import Measurements from './lib/Measurements.svelte';
  import ConfigPanel from './lib/ConfigPanel.svelte';

  let configOpen = $state(false);
  let calOpen = $state(false);
  const hasTare = $derived(live.sample?.tare != null);

  onMount(() => {
    connect();
    const id = setInterval(refreshInfo, 5000);
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
    <button class="primary" onclick={tare} disabled={!live.connected}>Tara</button>
    {#if hasTare}<button onclick={clearTare}>Tara löschen</button>{/if}
    <button onclick={() => (calOpen = true)} disabled={!live.config}>Kalibrieren</button>
    <button onclick={() => (configOpen = true)} disabled={!live.config}>Parameter</button>
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
  </div>
  <div class="col-side">
    <Stats />
    <Measurements />
  </div>
</main>

<footer class="foot muted">
  {#if info}
    <span>ethercat-weigh {info.version}</span>
    <span>Zyklen {info.cycles?.toLocaleString('de-DE')}</span>
    <span>Überläufe {info.overruns}</span>
    <span>max. Zykluszeit {info.max_cycle_ms?.toFixed(2)} ms</span>
  {/if}
</footer>

{#if live.notice}
  <div class="toast {live.notice.kind}" role="status">{live.notice.text}</div>
{/if}

<ConfigPanel bind:open={configOpen} />
<Calibration bind:open={calOpen} />

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
    max-width: 1480px;
    margin: 0 auto;
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
