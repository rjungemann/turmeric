/**
 * Vite configuration for Try Turmeric web app
 */
import { defineConfig } from 'vite';
import { cloudflare } from "@cloudflare/vite-plugin";
import { resolve } from 'path';
import { readFileSync } from 'fs';

const turmericVersion = readFileSync(resolve(__dirname, '../VERSION'), 'utf-8').trim();

function injectVersion() {
  return {
    name: 'inject-version',
    transformIndexHtml: (html) => html.replaceAll('%TURMERIC_VERSION%', turmericVersion),
  };
}

export default defineConfig({
  base: '/',
  environments: {
    // Target only the browser/client build — the worker SSR pass does not
    // accept HTML entry points and must be left with its own defaults.
    client: {
      build: {
        rollupOptions: {
          input: {
            main: resolve(__dirname, 'index.html'),
            try: resolve(__dirname, 'try/index.html'),
            tour: resolve(__dirname, 'tour/index.html'),
          },
        },
      },
    },
  },
  build: {
    outDir: 'dist',
    assetsDir: 'assets',
  },
  server: {
    port: 3000,
    host: true,
    headers: {
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
    },
  },
  preview: {
    headers: {
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
    },
  },
  plugins: [injectVersion(), cloudflare()],
});