#!/usr/bin/env bash
# tests/check-httpd-conn-single-def.sh -- enforce the HttpdConn single-
# definition invariant in stdlib/httpd.tur.
#
# The per-connection record HttpdConn (and its HttpdHeader / HttpdAttr
# helpers) used to be redeclared in full inside ~30 separate inline-C
# blocks. Those copies drifted into 7 divergent layouts; reading a late
# field (e.g. attr_list) through a stale, shorter copy overflowed the
# stack object and crashed (see
# docs/archive/history/httpd-conn-struct-consolidation-plan.md).
#
# They are now defined exactly once at file scope, hoisted via the
# __tur_include__ directives in httpd/autolink-hint. This guard fails the
# build if any inline-C block reintroduces a local redeclaration -- i.e.
# if a `} HttpdConn;` / `} HttpdHeader;` / `} HttpdAttr;` closer appears on
# a line that is NOT one of the canonical __tur_include__ directives.
set -euo pipefail
cd "$(dirname "$0")/.."

FILE="stdlib/httpd.tur"
status=0

for name in HttpdConn HttpdHeader HttpdAttr; do
    # Closing-brace lines for the struct, excluding the hoisted file-scope
    # __tur_include__ directives (which legitimately carry the canonical
    # definition).
    hits=$(grep -n "} ${name};" "$FILE" | grep -v "__tur_include__" || true)
    if [ -n "$hits" ]; then
        echo "FAIL httpd-conn-single-def: local redeclaration(s) of ${name} found in ${FILE}:"
        echo "$hits" | sed 's/^/    /'
        echo "    Define ${name} once via __tur_include__ in httpd/autolink-hint instead."
        status=1
    fi
done

if [ "$status" -ne 0 ]; then
    exit 1
fi

echo "PASS httpd-conn-single-def"
