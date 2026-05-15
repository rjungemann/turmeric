/**
 * Vite configuration for Try Turmeric web app
 */
import { defineConfig } from 'vite';
import { cloudflare } from "@cloudflare/vite-plugin";
import { copyFileSync, mkdirSync } from 'fs';
import { resolve } from 'path';

function copyTryPage() {
  return {
    name: 'copy-try-page',
    closeBundle() {
      const src = resolve(__dirname, 'try/index.html');
      const destDir = resolve(__dirname, 'dist/client/try');
      mkdirSync(destDir, { recursive: true });
      copyFileSync(src, `${destDir}/index.html`);
    },
  };
}

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
  plugins: [cloudflare(), copyTryPage()],
});