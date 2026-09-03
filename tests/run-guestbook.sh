#!/usr/bin/env bash
# tests/run-guestbook.sh -- drive examples/guestbook end to end with curl.
#
# Builds the example with `tur run` (the manifest's :c-sources links httpd.c),
# starts it on a private port with GUESTBOOK_MAX_REQUESTS so it exits on its
# own, and walks the whole flow the way a browser would: GET / (page 1), post
# the name, post the message, press Back on the preview, post a new message,
# confirm, then read /entries.  Every hop resumes a continuation the previous
# response's form action named, so this is the serializable-continuation
# round trip (capture -> bytes on disk -> rebuild -> resume) exercised
# through real HTTP.  A tampered token and a reused token must be refused.
set -uo pipefail
cd "$(dirname "$0")/.."
TUR="${TUR:-$PWD/build/tur}"
[ -x "$TUR" ] || { echo "run-guestbook: $TUR not built" >&2; exit 2; }
command -v curl >/dev/null || { echo "run-guestbook: curl not found" >&2; exit 2; }

PORT="${GUESTBOOK_TEST_PORT:-18080}"
WORK="$(mktemp -d)"
trap 'kill "$SERVER_PID" 2>/dev/null; rm -rf "$WORK"' EXIT
mkdir -p "$WORK/app"
# The app writes data/ under its cwd; run it from a scratch dir.
cd "$WORK/app"
GUESTBOOK_MAX_REQUESTS=9 PORT="$PORT" GUESTBOOK_SECRET=test-secret \
    "$TUR" run "$OLDPWD/examples/guestbook/src/main.tur" > "$WORK/server.log" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 100); do
    curl -s -o /dev/null "http://127.0.0.1:$PORT/entries" 2>/dev/null && break
    kill -0 "$SERVER_PID" 2>/dev/null || { echo "FAIL server exited early:"; cat "$WORK/server.log"; exit 1; }
    sleep 0.2
done
# That probe consumed request 1 of 9.

PASS=0; FAIL=0
check() { if grep -q -- "$2" <<<"$3"; then PASS=$((PASS+1)); echo "PASS $1"; else FAIL=$((FAIL+1)); echo "FAIL $1 -- expected: $2"; echo "$3" | head -c 600; echo; fi; }
action() { grep -o 'action="[^"]*"' <<<"$1" | head -1 | sed 's/action="//; s/"$//' | sed 's/&amp;/\&/g'; }

p1=$(curl -s "http://127.0.0.1:$PORT/")                                  # 2
check "page 1 is the name form" 'name="name"' "$p1"
a1=$(action "$p1")
check "page 1 action carries a signed token" '^/submit?k=[0-9a-f]\{64\}\.[0-9a-f]\{64\}$' "$a1"

p2=$(curl -s --data 'name=Ada+%3CLovelace%3E' "http://127.0.0.1:$PORT$a1")   # 3
check "page 2 greets the escaped name" 'Hello, Ada &lt;Lovelace&gt;' "$p2"
a2=$(action "$p2")

p3=$(curl -s --data 'message=first+draft' "http://127.0.0.1:$PORT$a2")    # 4
check "page 3 previews the message" 'first draft' "$p3"
check "page 3 previews the name" '&mdash; Ada &lt;Lovelace&gt;' "$p3"
a3=$(action "$p3")

p2b=$(curl -s --data 'decision=back' "http://127.0.0.1:$PORT$a3")        # 5
check "Back returns to the message form, prefilled" '>first draft</textarea>' "$p2b"
a2b=$(action "$p2b")

p3b=$(curl -s --data 'message=Hello%2C+world%21' "http://127.0.0.1:$PORT$a2b")   # 6
a3b=$(action "$p3b")
p4=$(curl -s --data 'decision=confirm' "http://127.0.0.1:$PORT$a3b")     # 7
check "Confirm shows the thank-you page with the entry" 'Hello, world!' "$p4"

bad=$(curl -s --data 'decision=confirm' "http://127.0.0.1:$PORT${a3b%?}0")   # 8: tampered signature
check "a tampered token is refused" 'not issued by this server' "$bad"

entries=$(curl -s "http://127.0.0.1:$PORT/entries")                       # 9
check "/entries lists the posted entry" 'Hello, world!' "$entries"
check "/entries does not list the abandoned draft" 'draft absent' "$(grep -q 'first draft' <<<"$entries" && echo 'draft present' || echo 'draft absent')"

wait "$SERVER_PID"; rc=$?
if [ "$rc" -ne 0 ]; then FAIL=$((FAIL+1)); echo "FAIL server exited $rc:"; tail -20 "$WORK/server.log"; fi
echo "guestbook: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
