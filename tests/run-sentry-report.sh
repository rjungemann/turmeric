#!/usr/bin/env bash
# tests/run-sentry-report.sh -- tools/ci/sentry-report.py against a stub Sentry.
#
# The fuzz and TSan workflows send their findings to Sentry and NOTHING else --
# no public artifact, no reproducer in the log (see section 7 question 8 of
# docs/upcoming/security-audit-plan.md). That makes this script the only path a
# finding takes, so a bug in it loses the finding silently, which is strictly
# worse than the public issue it replaced.
#
# So it is tested against a real HTTP server that captures what arrives and
# parses it back, rather than by eyeballing a --dry-run. What matters:
#
#   - the envelope is well-formed (headers are JSON, item lengths are BYTE
#     counts, the attachment survives byte-for-byte);
#   - crash type and top frame are extracted, so grouping is stable across
#     seeds -- the whole reason this is not one issue per night;
#   - the seed and run id are NOT in the fingerprint;
#   - the exit codes the workflows branch on are distinct: 0 sent, 2 no DSN,
#     1 send failed. The workflows fall back to an artifact on 1 and 2, so
#     confusing them means either losing a finding or publishing one.
#   - a DSN is never echoed, on any path.
set -u
cd "$(dirname "$0")/.."

PASS=0
FAIL=0
FAILED=()
ok()  { PASS=$((PASS+1)); echo "PASS $1"; }
bad() { FAIL=$((FAIL+1)); FAILED+=("$1"); echo "FAIL $1"; [ -n "${2:-}" ] && printf '  %s\n' "$2"; }

command -v python3 >/dev/null 2>&1 || { echo "tests: no python3; skipping"; exit 0; }

WORK="$(mktemp -d -t tur-sentry-XXXXXX)"
WORK="$(cd "$WORK" && pwd)"
trap 'rm -rf "$WORK"; [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2>/dev/null' EXIT INT TERM

# ------------------------------------------------------------------ #
# A stub Sentry: accepts one envelope, writes it to disk, replies 200.
# Port 0 so the OS picks a free one -- never a fixed port.
# ------------------------------------------------------------------ #
cat > "$WORK/stub.py" <<'PY'
import http.server, sys, threading
OUT = sys.argv[1]
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n)
        with open(OUT + "/body.bin", "wb") as f:
            f.write(body)
        with open(OUT + "/headers.txt", "w") as f:
            f.write(f"PATH {self.path}\n")
            for k, v in self.headers.items():
                f.write(f"{k}: {v}\n")
        self.send_response(200); self.end_headers(); self.wfile.write(b'{"id":"x"}')
    def log_message(self, *a): pass
srv = http.server.HTTPServer(("127.0.0.1", 0), H)
with open(OUT + "/port", "w") as f:
    f.write(str(srv.server_address[1]))
srv.serve_forever()
PY

python3 "$WORK/stub.py" "$WORK" &
SRV_PID=$!
for _ in $(seq 1 50); do [ -s "$WORK/port" ] && break; sleep 0.1; done
PORT="$(cat "$WORK/port" 2>/dev/null)"
[ -n "$PORT" ] || { echo "FAIL could not start the stub server"; exit 1; }

# A representative ASan report and a reproducer with bytes that would break a
# naive shell-built JSON writer: quotes, a backslash, a newline, and a NUL.
cat > "$WORK/target.log" <<'LOG'
==12345==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x602000000d18
READ of size 4 at 0x602000000d18 thread T0
    #0 0x4f1a2b in __asan_memcpy /build/llvm/compiler-rt/asan_interceptors.cpp:22
    #1 0x5a3b4c in json_decode_array /src/compiler/json.c:412:9
    #2 0x5a1f00 in json_decode /src/compiler/json.c:88:5
SUMMARY: AddressSanitizer: heap-buffer-overflow /src/compiler/json.c:412:9
LOG
printf 'crash"payload\\with\nnewline\000and-nul' > "$WORK/crash-deadbeef"

run_report() {
  python3 tools/ci/sentry-report.py \
    --kind fuzz-parser --target fuzz_json \
    --fingerprint fuzz-parser fuzz_json heap-buffer-overflow \
    --seed 20260930 --run-url "https://example.invalid/run/1" \
    --log "$WORK/target.log" --attach "$WORK/crash-deadbeef" "$@"
}

# ------------------------------------------------------------------ #
# 1. no DSN -> exit 2, and the caller falls back rather than losing it.
# ------------------------------------------------------------------ #
out="$(SENTRY_DSN= run_report 2>&1)"; rc=$?
[ "$rc" -eq 2 ] && ok "no DSN exits 2 (caller falls back)" \
                || bad "no DSN exits 2 (caller falls back)" "rc=$rc $out"

# ------------------------------------------------------------------ #
# 2. a good send -> exit 0, and the envelope actually arrives.
# ------------------------------------------------------------------ #
DSN="http://abc123def@127.0.0.1:$PORT/42"
out="$(SENTRY_DSN="$DSN" run_report 2>&1)"; rc=$?
[ "$rc" -eq 0 ] && ok "a good send exits 0" || bad "a good send exits 0" "rc=$rc $out"

# The project id becomes the path; the public key becomes the auth header.
if grep -q "PATH /api/42/envelope/" "$WORK/headers.txt" 2>/dev/null; then
    ok "posts to /api/<project>/envelope/"
else
    bad "posts to /api/<project>/envelope/" "$(cat "$WORK/headers.txt" 2>&1)"
fi
if grep -q "sentry_key=abc123def" "$WORK/headers.txt" 2>/dev/null; then
    ok "sends the public key in X-Sentry-Auth"
else
    bad "sends the public key in X-Sentry-Auth" "$(cat "$WORK/headers.txt" 2>&1)"
fi

# ------------------------------------------------------------------ #
# 3. the envelope parses, and says what it should.
# ------------------------------------------------------------------ #
cat > "$WORK/check.py" <<'PY'
import json, sys
raw = open(sys.argv[1], "rb").read()
# Envelope: a header line, then (item header line, payload of declared length).
nl = raw.index(b"\n"); head = json.loads(raw[:nl]); pos = nl + 1
items = []
while pos < len(raw):
    nl = raw.index(b"\n", pos)
    ih = json.loads(raw[pos:nl]); pos = nl + 1
    n = ih["length"]
    items.append((ih, raw[pos:pos + n])); pos += n + 1
ev = json.loads([p for h, p in items if h["type"] == "event"][0])
atts = [(h, p) for h, p in items if h["type"] == "attachment"]
res = {
  "head_has_event_id": head.get("event_id") == ev.get("event_id"),
  "n_attachments": len(atts),
  "attachment_bytes": atts[0][1] if atts else b"",
  "crash_type": ev["exception"]["values"][0]["type"],
  "top_frame": (ev["exception"]["values"][0].get("stacktrace") or {}).get("frames", [{}])[0].get("function"),
  "fingerprint": ev.get("fingerprint"),
  "tags": ev.get("tags"),
  "seed": ev.get("extra", {}).get("seed"),
  "has_report": "report" in ev.get("extra", {}),
}
print(json.dumps({k: (v.decode("latin1") if isinstance(v, bytes) else v) for k, v in res.items()}))
PY
J="$(python3 "$WORK/check.py" "$WORK/body.bin" 2>&1)" || { bad "envelope parses" "$J"; J='{}'; }
get() { python3 -c "import json,sys;print(json.loads(sys.argv[1]).get('$1'))" "$J" 2>/dev/null; }

[ "$(get head_has_event_id)" = "True" ] && ok "envelope header carries the event id" \
    || bad "envelope header carries the event id" "$J"

# The crash type and OUR frame, not the ASan interceptor -- grouping every
# overflow under __asan_memcpy would defeat the point.
[ "$(get crash_type)" = "heap-buffer-overflow" ] && ok "extracts the crash type" \
    || bad "extracts the crash type" "$(get crash_type)"
[ "$(get top_frame)" = "json_decode_array" ] && ok "skips the sanitizer interceptor frame" \
    || bad "skips the sanitizer interceptor frame" "$(get top_frame)"

# The seed is CONTEXT, never grouping: in the fingerprint it would make every
# night a new Sentry issue, which is the noise this replaces.
fp="$(get fingerprint)"
case "$fp" in
  *20260930*) bad "the seed is not in the fingerprint" "$fp" ;;
  *fuzz_json*) ok "the seed is not in the fingerprint" ;;
  *) bad "the seed is not in the fingerprint" "$fp" ;;
esac
[ "$(get seed)" = "20260930" ] && ok "the seed is recorded as context" \
    || bad "the seed is recorded as context" "$(get seed)"
[ "$(get has_report)" = "True" ] && ok "the sanitizer report travels with the event" \
    || bad "the sanitizer report travels with the event"

# ------------------------------------------------------------------ #
# 4. the reproducer survives byte for byte -- quotes, backslash, newline, NUL.
# ------------------------------------------------------------------ #
[ "$(get n_attachments)" = "1" ] && ok "the reproducer is attached" \
    || bad "the reproducer is attached" "$(get n_attachments)"
python3 - "$J" "$WORK/crash-deadbeef" <<'PY' && ok "the reproducer is byte-exact (quotes, backslash, newline, NUL)" \
                                             || bad "the reproducer is byte-exact (quotes, backslash, newline, NUL)"
import json, sys
got = json.loads(sys.argv[1])["attachment_bytes"].encode("latin1")
want = open(sys.argv[2], "rb").read()
sys.exit(0 if got == want else 1)
PY

# ------------------------------------------------------------------ #
# 5. a send that fails -> exit 1, distinct from 2, and no DSN in the output.
# ------------------------------------------------------------------ #
kill "$SRV_PID" 2>/dev/null; wait "$SRV_PID" 2>/dev/null; SRV_PID=""
out="$(SENTRY_DSN="$DSN" run_report 2>&1)"; rc=$?
[ "$rc" -eq 1 ] && ok "a failed send exits 1 (distinct from 2)" \
                || bad "a failed send exits 1 (distinct from 2)" "rc=$rc $out"
case "$out" in
  *abc123def*) bad "a failed send does not echo the DSN" "$out" ;;
  *)           ok "a failed send does not echo the DSN" ;;
esac

# A malformed DSN is a configuration error, not a reason to print it.
out="$(SENTRY_DSN="not-a-dsn-abc123def" run_report 2>&1)"; rc=$?
case "$rc:$out" in
  1:*abc123def*) bad "a malformed DSN is not echoed" "$out" ;;
  1:*)           ok "a malformed DSN exits 1 without echoing it" ;;
  *)             bad "a malformed DSN exits 1 without echoing it" "rc=$rc $out" ;;
esac

# ------------------------------------------------------------------ #
# 6. --dry-run needs no DSN and emits a parseable envelope, so the workflow
#    can be exercised by hand on a fork with no secret.
# ------------------------------------------------------------------ #
if SENTRY_DSN= run_report --dry-run > "$WORK/dry.bin" 2>/dev/null \
   && python3 "$WORK/check.py" "$WORK/dry.bin" >/dev/null 2>&1; then
    ok "--dry-run builds a valid envelope with no DSN"
else
    bad "--dry-run builds a valid envelope with no DSN"
fi

echo
echo "sentry-report summary: $PASS passed, $FAIL failed"
if [ "$FAIL" -gt 0 ]; then
    echo "failed:"
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    exit 1
fi
exit 0
