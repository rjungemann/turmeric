#!/usr/bin/env bash
# Guard the single-source-of-truth invariant for the HttpdConn struct.
#
# stdlib/httpd.tur implements the HTTP server in inline-C blocks. The
# per-connection record HttpdConn (and its HttpdHeader/HttpdAttr helpers) used
# to be redeclared in full inside ~30 separate blocks, which drifted into 7
# divergent layouts and caused an out-of-bounds write on the async path (see
# docs/upcoming/httpd-conn-struct-consolidation-plan.md). The canonical layout
# now lives once at file scope, hoisted via the `__tur_include__` directives in
# httpd/autolink-hint.
#
# This guard fails the build if any local (non-hoisted) redeclaration of those
# structs reappears. The hoisted directives carry the type names inside a
# `__tur_include__:` block comment, so they are excluded from the count.
set -euo pipefail
cd "$(dirname "$0")/.."

FILE=stdlib/httpd.tur
status=0

for ty in HttpdConn HttpdHeader HttpdAttr; do
  # A local redeclaration ends with "} <Type>;" on a real C line. The single
  # canonical definition is on a "__tur_include__" directive line; exclude it.
  hits="$(grep -n "} ${ty};" "$FILE" | grep -v "__tur_include__" || true)"
  if [ -n "$hits" ]; then
    echo "FAIL httpd-conn-single-def: local redeclaration(s) of ${ty} found in ${FILE}:"
    echo "$hits"
    echo "  -> Remove the local typedef; the canonical definition is hoisted"
    echo "     via __tur_include__ in httpd/autolink-hint."
    status=1
  fi
done

if [ "$status" -eq 0 ]; then
  echo "PASS httpd-conn-single-def"
fi
exit "$status"
