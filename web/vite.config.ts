import preact from '@preact/preset-vite';
import { defineConfig } from 'vite';
import { viteSingleFile } from 'vite-plugin-singlefile';

/**
 * @description Builds the editor as one self-contained index.html (embedded in the firmware).
 * In dev, /api is proxied to the simulator (default) or to a real screen: DM_DEVICE=http://desk-monitor.local npm run dev
 */
export default defineConfig({
  plugins: [preact(), viteSingleFile()],
  server: {
    proxy: {
      '/api': process.env.DM_DEVICE ?? 'http://localhost:8080',
    },
  },
  build: {
    target: 'es2020',
    assetsInlineLimit: 100_000_000,
  },
});
