#!/usr/bin/env bash
set -u
TUR=./build/tur
defs=0; calls=0; files=0
: > /tmp/rm1-real-callers.txt
while read -r f; do
    c=$(timeout 30 "$TUR" emit-c "$f" 2>/dev/null) || continue
    files=$((files+1))
    grep -qE '^static int64_t (some|ok|err)\(' <<<"$c" || continue
    defs=$((defs+1))
    # a real call: the name applied, on a line that is NOT the forward
    # declaration or the definition header (both start with `static`).
    n=$(grep -vE '^static ' <<<"$c" | grep -cE '[^_a-zA-Z0-9](some|ok|err)\(' || true)
    if [ "${n:-0}" -gt 0 ]; then
        calls=$((calls+1)); echo "$f $n" >> /tmp/rm1-real-callers.txt
    fi
done
echo "emitted: $files   base DEFINED in: $defs   base actually CALLED in: $calls"
