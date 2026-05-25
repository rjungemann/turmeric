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
