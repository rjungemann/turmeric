/**
 * Vite configuration for Try Turmeric web app
 */
import { defineConfig } from 'vite';
import { cloudflare } from "@cloudflare/vite-plugin";
import { resolve } from 'path';
import { readFileSync, existsSync, writeFileSync } from 'fs';

const turmericVersion = readFileSync(resolve(__dirname, '../VERSION'), 'utf-8').trim();

function injectVersion() {
  return {
    name: 'inject-version',
    transformIndexHtml: (html) => html.replaceAll('%TURMERIC_VERSION%', turmericVersion),
  };
}

// Rewrite the service worker's cache-version token to the current VERSION after
// the bundle is written. sw.js lives in public/ (copied verbatim into dist/), so
// transformIndexHtml never touches it -- without this the CACHE_VERSION would
// stay pinned to whatever literal was last hand-edited, and every returning
// visitor keeps getting the stale precached turmeric.wasm cache-first. Bumping
// the token changes sw.js's bytes, which is what makes the browser re-install
// the worker and evict the old caches in `activate`.
function injectSwVersion() {
  return {
    name: 'inject-sw-version',
    apply: 'build',
    closeBundle() {
      // The Cloudflare plugin splits the output into dist/client/ and
      // dist/<worker>/, so public/ assets land at dist/client/sw.js -- not
      // dist/sw.js, which is where this looked and silently found nothing.
      // Check both, and say so if neither is there: a rewrite that quietly
      // does not happen is exactly the stale-precache bug this plugin exists
      // to prevent.
      const candidates = [
        resolve(__dirname, 'dist/client/sw.js'),
        resolve(__dirname, 'dist/sw.js'),
      ].filter(existsSync);

      if (candidates.length === 0) {
        this.warn('sw.js not found in dist/ -- CACHE_VERSION was not stamped, '
                  + 'so returning visitors may be served stale precached assets');
        return;
      }

      for (const swPath of candidates) {
        const src = readFileSync(swPath, 'utf-8');
        const rewritten = src.replace(
          /tur-try-v1-\d+\.\d+\.\d+/g,
          `tur-try-v1-${turmericVersion}`,
        );
        if (rewritten !== src) writeFileSync(swPath, rewritten);
      }
    },
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
            trowel: resolve(__dirname, 'trowel/index.html'),
            ci: resolve(__dirname, 'ci/index.html'),
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
  plugins: [injectVersion(), injectSwVersion(), cloudflare()],
});