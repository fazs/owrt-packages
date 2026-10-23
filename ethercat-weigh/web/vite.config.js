import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';

// Entwicklung: `npm run dev` gegen ein laufendes Backend (z. B. `ethercat-weigh --sim`)
const backend = process.env.EW_BACKEND ?? 'http://127.0.0.1:8080';

export default defineConfig({
  plugins: [svelte()],
  build: { outDir: 'dist', emptyOutDir: true, chunkSizeWarningLimit: 600 },
  server: {
    proxy: {
      '/api': backend,
      '/ws': { target: backend.replace(/^http/, 'ws'), ws: true },
    },
  },
});
