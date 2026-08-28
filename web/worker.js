const INSTALL_SCRIPT = `#!/bin/sh
set -e

# Turmeric installer
# https://turmeric-lang.com

TAP="rjungemann/turmeric"
TAP_URL="https://github.com/rjungemann/turmeric"

if ! command -v brew >/dev/null 2>&1; then
  echo "Turmeric requires Homebrew. Install it first:"
  echo "  https://brew.sh"
  exit 1
fi

echo "Tapping $TAP..."
brew tap "$TAP" "$TAP_URL"

echo "Installing Turmeric..."
brew install --HEAD "$TAP/turmeric"
echo ""
echo "Done! Run 'tur --help' to get started."
echo "Try the online playground at https://turmeric-lang.com/try"
`;

const TIMINGS_BASE =
  'https://raw.githubusercontent.com/rjungemann/turmeric/ci-metrics';

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const { hostname, pathname } = url;

    if (pathname === '/install') {
      return new Response(INSTALL_SCRIPT, {
        headers: {
          'Content-Type': 'text/plain; charset=utf-8',
          'Cache-Control': 'no-cache',
        },
      });
    }

    // CI suite timings, proxied from the `ci-metrics` orphan branch so the
    // browser stays same-origin and the payload has one place to be shrunk.
    // The file is append-only NDJSON, year-partitioned by publish-timings.sh.
    // TODO: once suite-timings-<year>.jsonl passes ~5 MB, aggregate here
    // (group by run x suite, drop the raw rows) instead of streaming it whole.
    if (pathname === '/api/ci-timings') {
      const asked = url.searchParams.get('year') ?? '';
      const year = /^\d{4}$/.test(asked)
        ? asked
        : String(new Date().getUTCFullYear());

      // On Jan 1 the current year's file does not exist until the first push
      // to main lands, so fall back to the previous year rather than 502ing.
      for (const y of [year, String(Number(year) - 1)]) {
        const upstream = `${TIMINGS_BASE}/suite-timings-${y}.jsonl`;
        const res = await fetch(upstream, {
          cf: { cacheTtl: 300, cacheEverything: true },
        });
        if (res.ok) {
          return new Response(res.body, {
            headers: {
              'Content-Type': 'application/x-ndjson; charset=utf-8',
              'Cache-Control': 'public, max-age=300',
              'X-Timings-Year': y,
            },
          });
        }
      }

      return new Response('no timings available\n', {
        status: 502,
        headers: { 'Content-Type': 'text/plain; charset=utf-8' },
      });
    }

    // Rewrite try.turmeric-lang.com/* -> turmeric-lang.com/try/*
    // so both URLs serve the same page without a redirect round-trip.
    const response = await env.ASSETS.fetch(request);

    // SharedArrayBuffer (required for Emscripten pthreads) is only available
    // in cross-origin isolated contexts.
    const headers = new Headers(response.headers);
    headers.set('Cross-Origin-Opener-Policy', 'same-origin');
    headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
    return new Response(response.body, {
      status: response.status,
      statusText: response.statusText,
      headers,
    });
  },
};
