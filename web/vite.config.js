/**
 * Vite configuration for Try Turmeric web app
 */
import { defineConfig } from 'vite';

import { cloudflare } from "@cloudflare/vite-plugin";

export default defineConfig({
  base: '/',
  build: {
    outDir: 'dist',
    assetsDir: 'assets',
  },
  server: {
    port: 3000,
    host: true,
  },
  plugins: [cloudflare()],
});