/**
 * Vite configuration for Try Turmeric web app
 */
import { defineConfig } from 'vite';
import { cloudflare } from "@cloudflare/vite-plugin";
import { resolve } from 'path';
import { readFileSync, existsSync, writeFileSync } from 'fs';
import { execSync } from 'child_process';

const turmericVersion = readFileSync(resolve(__dirname, '../VERSION'), 'utf-8').trim();

// The service-worker cache name used to be the release VERSION alone, which
// made every OUT-OF-BAND deploy invisible: a fix deployed at the same version
// reuses the cache name, `activate` evicts nothing, and every returning visitor
// keeps being served the old precached bundle and wasm cache-first. That is not
// hypothetical -- the Saffron language-picker fix deployed green and the live
// site went on rendering the four stale bases, because the cache was still
// `tur-try-v1-0.45.0` from the release cut an hour earlier.
//
// So the token carries the BUILD, not just the release: the commit the bundle
// was built from. Any deploy changes sw.js's bytes, which is what makes the
// browser re-install the worker and drop the old caches. Falls back to a
// timestamp outside a git checkout (a release tarball), which is still unique
// per build -- never a constant, or the bug comes back quietly.
function buildId() {
  try {
    const sha = execSync('git rev-parse --short HEAD', {
      cwd: resolve(__dirname, '..'),
      stdio: ['ignore', 'pipe', 'ignore'],
    }).toString().trim();
    if (sha) return sha;
  } catch { /* not a git checkout -- fall through */ }
  return String(Date.now());
}

const swCacheVersion = `tur-try-v1-${turmericVersion}-${buildId()}`;

function injectVersion() {
  return {
    name: 'inject-version',
    transformIndexHtml: (html) => html.replaceAll('%TURMERIC_VERSION%', turmericVersion),
  };
}

// Rewrite the service worker's cache-version token to the current build after
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
        // Matches the clean in-tree literal (public/sw.js is copied verbatim
        // into dist/ on every build, so what we rewrite is never an
        // already-stamped name), and tolerates a stamped one for safety.
        const rewritten = src.replace(
          /tur-try-v1-\d+\.\d+\.\d+(?:-[0-9a-zA-Z]+)?/g,
          swCacheVersion,
        );
        if (rewritten !== src) writeFileSync(swPath, rewritten);
      }
    },
  };
}

// Ship the kill-switch AT /sw.js instead of the real worker.
//
// A stuck client only ever refetches the worker script at the URL it
// registered, so /sw.js is the one address that can reach it. This overwrites
// the built worker rather than asking anyone to edit public/sw.js by hand and
// remember to put it back -- the tree is never left in the dangerous state, and
// reverting is an ordinary deploy with the flag off.
//
//   TUR_SW_KILL=1 npm run deploy     # wipe caches + unregister, on next launch
//   npm run deploy                   # restore the real worker
//
// Runs AFTER injectSwVersion in the plugin list, so the cache-version stamp it
// would otherwise apply is irrelevant -- there is no cache name left to stamp.
function swKillSwitch() {
  const armed = process.env.TUR_SW_KILL === '1';
  return {
    name: 'sw-kill-switch',
    apply: 'build',
    closeBundle() {
      if (!armed) return;
      const src = resolve(__dirname, 'public/sw-kill.js');
      const targets = [
        resolve(__dirname, 'dist/client/sw.js'),
        resolve(__dirname, 'dist/sw.js'),
      ].filter(existsSync);
      if (targets.length === 0) {
        // Louder than a warning would be: an armed build that silently shipped
        // the ordinary worker is a recovery everyone believes happened.
        throw new Error('TUR_SW_KILL=1 but no dist sw.js was found to replace; '
                        + 'the kill-switch was NOT deployed');
      }
      const kill = readFileSync(src, 'utf-8');
      for (const t of targets) writeFileSync(t, kill);
      console.warn('\n  *** TUR_SW_KILL=1: /sw.js is the KILL-SWITCH ***\n'
                   + '  This build unregisters the service worker and deletes\n'
                   + '  every cache on first launch. Deploy again without the\n'
                   + '  flag to restore the real worker.\n');
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
  plugins: [injectVersion(), injectSwVersion(), swKillSwitch(), cloudflare()],
});