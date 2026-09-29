#!/usr/bin/env bash
# Temporary diagnostic for rjungemann/turmeric#966 on macOS, round 19.
# macOS's jmp_buf is int[48], so the thread record's `regs` sat at 4 mod 8
# and the word-aligned scan never read a parked thread's registers.  The
# candidate aligns it and spills in the blocking call's frame.  Variants,
# patched into each fixture's C:
#   fix        the candidate as built
#   unaligned  the candidate with the record's jmp_bufs left unaligned
#   ownframe   aligned, but park a function spilling its own registers
# Then the whole r7rs-gc suite on the candidate.
set -u
cd "$(dirname "$0")/../.."
ROOT="$(pwd)"
TUR="$ROOT/build/tur"
W="$(mktemp -d)"
echo "cc: $(cc --version | head -1)"
mkvar() {
  python3 - "$1" "$2" "$3" <<'PY'
import sys
src, v, out = sys.argv[1:4]
s = open(src).read()
fields = ['jmp_buf        regs __attribute__((aligned(16)));',
          'jmp_buf        sig_regs __attribute__((aligned(16)));']
park = '''#define tur_gc_park() do {                                                  \\
        tur_gc_thread *tpk_ = TUR_GC_SELF_FRESH();                          \\
        if (tpk_ && tpk_->park_depth++ == 0) {                              \\
            setjmp(tpk_->regs);         /* callee-saved registers */        \\
            tur_gc_park_at(tpk_);                                           \\
        }                                                                   \\
    } while (0)
'''
own = '''TUR_GC_NOASAN __attribute__((noinline)) static void tur_gc_park(void) {
    tur_gc_thread *t = TUR_GC_SELF_FRESH();
    if (!t) return;
    if (t->park_depth++ > 0) return;
    setjmp(t->regs);
    volatile unsigned char here = 0;
    t->stack_sp = (unsigned char *)&here;
    TUR_GC_STORE(&t->parked, 1);
}
'''
assert all(s.count(f) == 1 for f in fields) and s.count(park) == 1
if v == 'unaligned':
    for f in fields: s = s.replace(f, f.replace(' __attribute__((aligned(16)))', ''))
elif v == 'ownframe':
    s = s.replace(park, own)
open(out, 'w').write(s)
PY
}
probe() {  # probe <fixture> <torture> <runs> <variants...>
  local fx="$1" tort="$2" n="$3"; shift 3
  local dir="$ROOT/tests/fixtures/$fx"
  rm -rf /tmp/tur-build "${TMPDIR:-/tmp}/tur-build"
  (cd "$dir" && TUR_PRELUDE_SPLIT=0 TUR_SHOW_CC=1 "$TUR" build input.tur -o "$W/$fx-base" > "$W/$fx.build" 2>&1)
  local cc; cc="$(grep -m1 '^CC: ' "$W/$fx.build" | sed 's/^CC: //')"
  local csrc; csrc="$(echo "$cc" | grep -oE '[^ ]*/tur-build/input_tur\.c' | head -1)"
  [ -n "$csrc" ] && [ -f "$csrc" ] || { echo "$fx: no C"; return; }
  cp "$csrc" "$W/$fx.c"
  local want; want="$(cat "$dir/expected.stdout")"
  for v in "$@"; do
    mkvar "$W/$fx.c" "$v" "$W/$fx-$v.c" 2> "$W/$fx-$v.mk" || { echo "$fx $v: variant failed: $(tail -1 "$W/$fx-$v.mk")"; continue; }
    local cmd="${cc//$csrc/$W/$fx-$v.c}"; cmd="$(echo "$cmd" | sed -E "s#-o [^ ]+#-o $W/$fx-$v#")"
    (cd "$dir" && eval "$cmd") > "$W/$fx-$v.cc" 2>&1 || { echo "$fx $v: compile failed: $(grep -m1 error "$W/$fx-$v.cc")"; continue; }
    local ok=0 bad=""
    for i in $(seq 1 "$n"); do
      (cd "$dir" && TUR_GC_TORTURE="$tort" timeout 120 "$W/$fx-$v" > "$W/out" 2> "$W/err"); local rc=$?
      if [ $rc = 0 ] && [ "$(cat "$W/out")" = "$want" ]; then ok=$((ok+1)); else bad="$bad [rc=$rc $(tail -1 "$W/err" | cut -c1-40)]"; fi
    done
    echo "$fx $v (torture $tort): ok=$ok/$n$bad"
  done
}
V="fix unaligned ownframe"
probe r7rs-threads-roots 1 6 $V
probe r7rs-threads-pause 1 4 $V
probe r7rs-threads-syscall 1 3 $V
probe r7rs-threads-lifecycle 31 3 $V
probe r7rs-threads-fiber-migration 31 8 $V
echo "== tests/run-r7rs-gc.sh =="
timeout 2400 bash tests/run-r7rs-gc.sh 2>&1 | grep -E '^FAIL|r7rs-gc:'
