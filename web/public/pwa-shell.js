// pwa-shell.js -- mark the installed-app shell BEFORE first paint.
//
// Loaded as a classic, parser-blocking <script src> in try/index.html's <head>,
// so it still runs before first paint. It was an inline <script> until the
// site's Content-Security-Policy (web/csp.js) forbade inline scripts.
//
// styles.css gates the pinned shell and the safe-area insets on the `pwa` class
// instead of on (display-mode: standalone): iOS reports `fullscreen`, not
// `standalone`, for a home-screen app whose status bar is black-translucent --
// which this one is. A standalone-only media query therefore never matched on
// the only platform with a notch to reserve, so #app kept `height: 100dvh` from
// the plain mobile block, which on iOS lands short of the screen that
// viewport-fit=cover hands the page. navigator.standalone is the definitive iOS
// signal; the media queries cover every other browser.
(function () {
  try {
    var queries = ['(display-mode: standalone)',
                   '(display-mode: fullscreen)',
                   '(display-mode: minimal-ui)'];
    var sync = function () {
      var installed = navigator.standalone === true;
      for (var i = 0; !installed && i < queries.length; i++) {
        installed = !!(window.matchMedia && window.matchMedia(queries[i]).matches);
      }
      document.documentElement.classList.toggle('pwa', installed);
    };
    sync();
    if (window.matchMedia) {
      for (var i = 0; i < queries.length; i++) {
        var mql = window.matchMedia(queries[i]);
        if (mql.addEventListener) mql.addEventListener('change', sync);
        else if (mql.addListener) mql.addListener(sync);
      }
    }
  } catch (e) { /* a missing shell is better than a blank page */ }
})();
