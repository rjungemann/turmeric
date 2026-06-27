# `tur --interpret` segfaults: null `ReaderMacroRegistry` in `cmd_eval_h`

**Severity:** high -- the documented v1 Run target for the desktop editor
spike (`tur --interpret <file>`) is unusable on a minimal hello-world.

**Repro:**

```sh
cat > /tmp/spike-hello.tur <<'EOF'
;;; spike-hello -- minimal Turmeric run target.
(defn main [] : int
  (println "hello from spike")
  0)
EOF
./build/tur --interpret /tmp/spike-hello.tur
```

**Observed:**

```
src/main.c:5345:25: runtime error: member access within null pointer
  of type 'struct ReaderMacroRegistry'
SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior main.c:5345:25
AddressSanitizer:DEADLYSIGNAL
==... ERROR: AddressSanitizer: SEGV on unknown address 0x0...18
  #0 cmd_eval_h  src/main.c:5345
  #1 main        src/main.c
```

**Expected:** prints `hello from spike` and exits 0, the same way
`./build/tur run /tmp/spike-hello.tur` does today.

**Workaround:** use `tur run` (AOT) or `tur check` for now -- both work
on the same fixture.

**Root cause (likely):** `cmd_eval_h` at `src/main.c:5345` dereferences
the `ReaderMacroRegistry` global before it has been initialized on the
`--interpret` codepath. The AOT `run` path initializes it earlier in
`main`; the `--interpret` shortcut appears to skip that setup.

**Related work:** Phase 1 of
`docs/upcoming/turmeric-scite-desktop-plan.md` (and the in-progress
Lite XL replacement plan) bind Run to `tur --interpret`. With this bug
open they fall back to `tur run` instead.
