// csp.js -- the Content-Security-Policy for everything turmeric-lang.com serves.
//
// One source, three places it is applied (security-audit-plan WP6, W-1):
//   - vite.config.js sends it from the dev and preview servers, so the
//     Playwright suite runs with the policy enforced;
//   - vite.config.js (stampCsp) writes it into the built dist/client/_headers,
//     which is what Cloudflare attaches to every static asset;
//   - worker.js sets it on every response the Worker itself produces, which
//     `_headers` never reaches.
//
// What each allowance is for -- remove one only with its reason:
//
//   'wasm-unsafe-eval'   The interpreter and the language server are WebAssembly,
//                        compiled in the eval and LSP workers (and their pthread
//                        workers). Nothing here needs JavaScript eval.
//   .../mermaid@11/dist/ The guide runtime imports mermaid on demand from
//                        jsDelivr (tools/genguides.py, MERMAID_SRC). A path, not
//                        the host: no other package on jsDelivr may load.
//   style 'unsafe-inline' Monaco writes <style> elements and style attributes,
//                        and the generated doc pages carry <style> blocks. Only
//                        STYLES; script-src has no 'unsafe-inline', so an
//                        injected handler or <script> does not run.
//   fonts.googleapis.com / fonts.gstatic.com / .../@fontsource/
//                        The generated doc pages' web fonts (genguides
//                        font_links). The app bundles its own fonts.
//   data: (img, font)    Monaco's and the bundle's small inlined icons/fonts.
//
// connect-src is 'self' only: /api/ci-timings is proxied by the Worker, so the
// browser never talks to raw.githubusercontent.com itself.
export const CONTENT_SECURITY_POLICY = [
    "default-src 'self'",
    "script-src 'self' 'wasm-unsafe-eval' https://cdn.jsdelivr.net/npm/mermaid@11/dist/",
    "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com https://cdn.jsdelivr.net/npm/@fontsource/",
    "font-src 'self' data: https://fonts.gstatic.com https://cdn.jsdelivr.net/npm/@fontsource/",
    "img-src 'self' data:",
    "connect-src 'self'",
    "worker-src 'self'",
    "manifest-src 'self'",
    "object-src 'none'",
    "base-uri 'self'",
    "form-action 'self'",
    "frame-ancestors 'none'",
].join('; ');
