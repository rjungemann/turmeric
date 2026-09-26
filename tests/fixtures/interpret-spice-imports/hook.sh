#!/usr/bin/env bash
# interpret-spice-imports: the interpreter resolves imports the way the
# compiling per-file commands do -- `-I` dirs, and the enclosing spice's src/
# found by the build.tur walk-up.  A spice test lives in tests/, a sibling of
# src/, so neither the importing file's directory nor the stdlib has the
# module; before the fix every row below but the first control failed with
# "module 'demo/core' not found" (or, for -I, "load: cannot open '-I'").
# See docs/archive/interpret-takes-no-include-path-or-spice-discovery.md.
set -u
TMP="$1"
# The rows run from inside the spice, so a relative $TUR (run.sh passes
# ./build/tur) must be anchored first.
TUR="$(cd "$(dirname "$TUR")" && pwd)/$(basename "$TUR")"
SP="$TMP/spice"
mkdir -p "$SP/src/demo" "$SP/tests/demo" "$TMP/loose"
cat > "$SP/build.tur" <<'TUR_EOF'
(defpackage demo
  :modules [demo/core])
TUR_EOF
cat > "$SP/src/demo/core.tur" <<'TUR_EOF'
(defmodule demo/core
  (export triple)
  (defn triple [x : int] : int {x * 3}))
TUR_EOF
cat > "$SP/tests/demo/test_core.tur" <<'TUR_EOF'
(defmodule demo/test-core
  (import demo/core :refer [triple])
  (defn main [] : int
    (println (triple 14))
    (println (list-length *args*))
    0))
TUR_EOF
# The same program outside any spice: only -I can find the module.
cp "$SP/tests/demo/test_core.tur" "$TMP/loose/prog.tur"

T="$SP/tests/demo/test_core.tur"
L="$TMP/loose/prog.tur"
row() { # $1=label, rest=command; print the label, stdout, and exit status
    local label="$1"; shift
    local out rc
    out=$("$@" 2>"$TMP/err"); rc=$?
    echo "$label: $(echo $out) (exit $rc)"
    [ $rc -eq 0 ] || grep -o -m1 "module '[^']*' not found\|cannot open '[^']*'\|-I requires a directory argument" "$TMP/err" | sed 's/^/    /' || true
}
cd "$SP"
row "compiled control" "$TUR" run tests/demo/test_core.tur
row "interpret, auto-spice" "$TUR" --interpret tests/demo/test_core.tur
row "interpret, abs path" "$TUR" --interpret "$T"
row "interpret, -I src" "$TUR" --no-auto-spice --interpret -I src tests/demo/test_core.tur
row "interpret, -Isrc + args" "$TUR" --no-auto-spice --interpret -Isrc tests/demo/test_core.tur a -I b
row "interpret, no-auto-spice" "$TUR" --no-auto-spice --interpret tests/demo/test_core.tur
row "engine=interp" "$TUR" run --engine=interp tests/demo/test_core.tur
row "engine=interp, -I src" "$TUR" --no-auto-spice run --engine=interp -I src tests/demo/test_core.tur
row "eval --file -I" "$TUR" --no-auto-spice eval --file tests/demo/test_core.tur -I src
row "bare -I" "$TUR" --interpret -I
cd "$TMP"
row "loose, -I" "$TUR" --interpret -I "$SP/src" "$L"
row "loose, no -I" "$TUR" --interpret "$L"
exit 0
