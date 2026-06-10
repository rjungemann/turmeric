#!/usr/bin/env bash
# tests/run-build-project.sh -- manifest-driven `tur build <dir>` smoke test.
#
# Verifies that `tur build <project-dir>` (where the directory carries a
# build.tur manifest) descends into src/ -- including nested src/<pkg>/ --
# compiles every module, resolves cross-module imports, and links a runnable
# binary.  Also asserts the bug it fixes: the manifest itself must not be
# compiled as source.
#
# The fixture is copied into a scratch dir before building so the generated
# .h/.c, _main.c, and .tur-abi-cache/ never land in the repo.

set -uo pipefail
cd "$(dirname "$0")/.."
REPO="$PWD"

TUR="$REPO/build/tur"
FIXTURE="$REPO/tests/fixtures/build-project-smoke"
WORK="$(mktemp -d -t tur-bp.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# Leaks in the Debug (ASan) build of tur are out of scope here; we are
# exercising the build-driver path, not chasing compiler-internal leaks.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

if [ ! -x "$TUR" ]; then
    echo "tests: $TUR not built; run 'just build' first" >&2
    exit 2
fi

cp -R "$FIXTURE" "$WORK/proj"
PROJ="$WORK/proj"
BIN="$WORK/app"

# Build the project from a scratch CWD so generated files land in $WORK.
build_out=$(cd "$WORK" && "$TUR" build "$PROJ" -o "$BIN" 2>&1)
build_rc=$?
if [ $build_rc -ne 0 ]; then
    fail "build-project-compiles" "tur build exit=$build_rc: $build_out"
else
    pass "build-project-compiles"
fi

# The manifest must never be compiled as source.
if echo "$build_out" | grep -q "unbound symbol 'tur-build-project-smoke'"; then
    fail "build-project-skips-manifest" "build.tur was compiled as source"
else
    pass "build-project-skips-manifest"
fi

# The linked binary must exist and run, returning double-it(21) = 42.
if [ -x "$BIN" ]; then
    pass "build-project-binary-exists"
    "$BIN"
    run_rc=$?
    if [ "$run_rc" -eq 42 ]; then
        pass "build-project-cross-module-import"
    else
        fail "build-project-cross-module-import" "exit=$run_rc (expected 42)"
    fi
else
    fail "build-project-binary-exists" "expected $BIN to exist"
fi

# A module declared in :exports with no backing source file must fail loudly
# (Phase 2: parsed :exports map keys are validated against on-disk sources).
GHOST="$WORK/ghost"
mkdir -p "$GHOST/src/app"
cat > "$GHOST/build.tur" <<'EOF'
(defpackage tur-ghost
  :name    "tur-ghost"
  :version "0.1.0"
  :exports #{ "app/util" ["double-it"] "app/missing" ["nope"] })
EOF
cat > "$GHOST/src/app/util.tur" <<'EOF'
(defmodule app/util (export double-it) (defn double-it [x :int] :int (* x 2)))
EOF
ghost_out=$(cd "$WORK" && "$TUR" build "$GHOST" -o "$WORK/ghostbin" 2>&1)
ghost_rc=$?
if [ $ghost_rc -ne 0 ] && echo "$ghost_out" | grep -q "declares export 'app/missing'"; then
    pass "build-project-missing-export-fails"
else
    fail "build-project-missing-export-fails" "rc=$ghost_rc out=$ghost_out"
fi

# Flat layout: build.tur at the root with sources alongside it (no src/ dir).
# Exercises the shallow-scan fallback in collect_project_src_files and the
# regression guard that the stray manifest is never compiled as source.
FLAT="$WORK/flat"
mkdir -p "$FLAT"
cat > "$FLAT/build.tur" <<'EOF'
(defpackage tur-flat
  :name    "tur-flat"
  :version "0.1.0")
EOF
cat > "$FLAT/prog.tur" <<'EOF'
(defmodule prog (defn main [] :int 7))
EOF
flat_out=$(cd "$WORK" && "$TUR" build "$FLAT" -o "$WORK/flatbin" 2>&1)
flat_rc=$?
if [ $flat_rc -eq 0 ] && [ -x "$WORK/flatbin" ] \
   && ! echo "$flat_out" | grep -q "unbound symbol 'tur-flat'"; then
    pass "build-project-flat-skips-manifest"
else
    fail "build-project-flat-skips-manifest" "rc=$flat_rc out=$flat_out"
fi

# T2: project-mode `tur run` from an arbitrary cwd must resolve the turmeric
# runtime sources (e.g. src/runtime/hamt.c, pulled in by the hamt autolink
# marker) absolutely, not relative to cwd.  Before T2 this failed with
# `src/runtime/hamt.c: No such file` whenever cwd was not the turmeric tree.
HAMT="$WORK/hamtproj"
mkdir -p "$HAMT/src"
cat > "$HAMT/build.tur" <<'EOF'
(defpackage tur-hamtproj
  :name    "tur-hamtproj"
  :version "0.1.0")
EOF
# hamt/new + hamt/count force the src/runtime/hamt.c autolink marker; an empty
# map has count 0, so a clean exit code proves the runtime linked and ran.
cat > "$HAMT/src/main.tur" <<'EOF'
(defn main [] :int
  (let [m (hamt/new)]
    (hamt/count m)))
EOF
hamt_out=$(cd "$WORK" && "$TUR" run --offline "$HAMT/src/main.tur" 2>&1)
hamt_rc=$?
# Reproduce the exact T2 entry point too: project mode with no file arg,
# discovered by walking up from cwd inside the project.
hamt_proj_out=$(cd "$HAMT" && "$TUR" run --offline 2>&1)
hamt_proj_rc=$?
if [ $hamt_rc -eq 0 ] && [ $hamt_proj_rc -eq 0 ] \
   && ! echo "$hamt_out$hamt_proj_out" | grep -q "src/runtime/hamt.c: No such file"; then
    pass "run-project-resolves-runtime-from-foreign-cwd"
else
    fail "run-project-resolves-runtime-from-foreign-cwd" \
        "file-mode rc=$hamt_rc proj-mode rc=$hamt_proj_rc out=$hamt_out$hamt_proj_out"
fi

# T3: `tur build <dir>` must compile and link a :spices dependency's modules,
# not just resolve them for type-checking.  A consumer that imports a module
# from a :path-linked spice previously failed at cc with
# `sib__api.h: No such file` because the dep's module was never generated.
DEPROOT="$WORK/cross"
mkdir -p "$DEPROOT/sib/src/sib" "$DEPROOT/consumer/src/consumer"
cat > "$DEPROOT/sib/build.tur" <<'EOF'
(defpackage sib :name "sib")
EOF
cat > "$DEPROOT/sib/src/sib/api.tur" <<'EOF'
(defmodule sib/api
  (export answer)
  (defn answer [] :int 42))
EOF
cat > "$DEPROOT/consumer/build.tur" <<'EOF'
(defpackage consumer
  :name "consumer"
  :spices #{ "sib" #{:path "../sib"} })
EOF
cat > "$DEPROOT/consumer/src/consumer/main.tur" <<'EOF'
(defmodule consumer/main
  (import sib/api :refer [answer])
  (defn main [] :int (answer)))
EOF
xspice_out=$(cd "$DEPROOT/consumer" && "$TUR" build "$DEPROOT/consumer" -o "$WORK/xspice" 2>&1)
xspice_rc=$?
if [ $xspice_rc -ne 0 ] || [ ! -x "$WORK/xspice" ]; then
    fail "build-project-links-cross-spice-dep" "rc=$xspice_rc out=$xspice_out"
else
    "$WORK/xspice"
    xrun_rc=$?
    # answer = 42, proving the dep module was compiled, linked, and called.
    if [ "$xrun_rc" -eq 42 ]; then
        pass "build-project-links-cross-spice-dep"
    else
        fail "build-project-links-cross-spice-dep" "exit=$xrun_rc (expected 42)"
    fi
fi

# SYM2 (runtime-symbols-plan): a keyword `:foo` referenced from two different
# module TUs must fold to a single interned record (one pointer) in the final
# binary.  Build a two-module project under -Xsymbols, where each module returns
# `:foo`, and assert pointer identity across the module boundary (exit 0) plus a
# single weak symbol via nm.
SYMP="$WORK/symx"
mkdir -p "$SYMP/src/app"
cat > "$SYMP/build.tur" <<'EOF'
(defpackage tur-sym-cross-tu
  :name    "tur-sym-cross-tu"
  :version "0.1.0"
  :exports #{
    "app/main" ["main"]
    "app/a"    ["a-foo"]
    "app/b"    ["b-foo"]
  })
EOF
cat > "$SYMP/src/app/a.tur" <<'EOF'
(defmodule app/a (export a-foo) (defn a-foo [] :Sym :foo))
EOF
cat > "$SYMP/src/app/b.tur" <<'EOF'
(defmodule app/b (export b-foo) (defn b-foo [] :Sym :foo))
EOF
cat > "$SYMP/src/app/main.tur" <<'EOF'
(defmodule app/main
  (import app/a :refer [a-foo])
  (import app/b :refer [b-foo])
  (defn same? [x :Sym y :Sym] :int
    ```c
    return (int64_t)((const void *)x == (const void *)y);
    ```)
  (defn main [] :int
    ;; 0 = the two TUs' :foo are the same pointer (cross-TU interning works).
    (if (= (same? (a-foo) (b-foo)) 1) 0 1)))
EOF
sym_out=$(cd "$WORK" && "$TUR" -Xsymbols build "$SYMP" -o "$WORK/symbin" 2>&1)
sym_rc=$?
if [ $sym_rc -ne 0 ] || [ ! -x "$WORK/symbin" ]; then
    fail "build-project-sym-cross-tu" "rc=$sym_rc out=$sym_out"
else
    "$WORK/symbin"
    sym_run_rc=$?
    if [ "$sym_run_rc" -eq 0 ]; then
        pass "build-project-sym-cross-tu"
    else
        fail "build-project-sym-cross-tu" "exit=$sym_run_rc (expected 0; :foo not folded across TUs)"
    fi
    # nm: exactly one __tur_sym_foo object in the linked binary (weak-folded).
    if command -v nm >/dev/null 2>&1; then
        sym_count=$(nm "$WORK/symbin" 2>/dev/null | grep -c "__tur_sym_foo")
        if [ "$sym_count" -eq 1 ]; then
            pass "build-project-sym-cross-tu-single-symbol"
        else
            fail "build-project-sym-cross-tu-single-symbol" "nm found $sym_count __tur_sym_foo symbols (expected 1)"
        fi
    fi
fi

# F1 regression: prelude macros (when/cond/for/unless) must be visible inside
# a defmodule body compiled by project mode (`tur build <dir>`).  Before F1 the
# project-mode entry points (compile_to_h / compile_to_implementation) skipped
# the stdlib auto-load entirely, so `when` was "unknown function or operator".
WHEN_PROJ="$WORK/prelude-when"
mkdir -p "$WHEN_PROJ/src/app"
cat > "$WHEN_PROJ/build.tur" <<'EOF'
(defpackage prelude-when :name "prelude-when" :version "0.1.0")
EOF
cat > "$WHEN_PROJ/src/app/main.tur" <<'EOF'
(defmodule app/main
  (defn classify [x : int] : int
    (cond
      (> x 0) 3
      (< x 0) 7
      :else   0))
  (defn main [] : int
    ;; when/cond are prelude macros; using cond with an :else branch gives a
    ;; deterministic return value -- 3 if x>0, which proves the prelude loaded.
    (classify 5)))
EOF
when_out=$(cd "$WORK" && "$TUR" build "$WHEN_PROJ" -o "$WORK/whenbin" 2>&1)
when_rc=$?
if [ $when_rc -ne 0 ]; then
    fail "build-project-prelude-when" "tur build exit=$when_rc: $when_out"
else
    "$WORK/whenbin"
    when_run_rc=$?
    if [ "$when_run_rc" -eq 3 ]; then
        pass "build-project-prelude-when"
    else
        fail "build-project-prelude-when" "exit=$when_run_rc (expected 3)"
    fi
fi

# F4 regression: min/max macros added to stdlib/macros.tur must be visible
# inside a defmodule body compiled by project mode.
MINMAX_PROJ="$WORK/prelude-minmax"
mkdir -p "$MINMAX_PROJ/src/app"
cat > "$MINMAX_PROJ/build.tur" <<'EOF'
(defpackage prelude-minmax :name "prelude-minmax" :version "0.1.0")
EOF
cat > "$MINMAX_PROJ/src/app/main.tur" <<'EOF'
(defmodule app/main
  (defn main [] : int
    (+ (min 3 7) (max 2 5))))
EOF
minmax_out=$(cd "$WORK" && "$TUR" build "$MINMAX_PROJ" -o "$WORK/minmaxbin" 2>&1)
minmax_rc=$?
if [ $minmax_rc -ne 0 ]; then
    fail "build-project-prelude-minmax" "tur build exit=$minmax_rc: $minmax_out"
else
    "$WORK/minmaxbin"
    minmax_run_rc=$?
    if [ "$minmax_run_rc" -eq 8 ]; then
        pass "build-project-prelude-minmax"
    else
        fail "build-project-prelude-minmax" "exit=$minmax_run_rc (expected 8)"
    fi
fi

# F3 regression: the user-callable `cons` runtime list constructor must be
# available inside a defmodule body compiled by project mode (`tur build <dir>`).
# `cons` is registered as a builtin lowering to a {head,tail} cons-cell helper
# emitted into each TU's preamble, so it resolves without the stdlib auto-load
# that project mode skips (closes the c-dsl/glsl gap; F6 in the report).
CONS_PROJ="$WORK/prelude-cons"
mkdir -p "$CONS_PROJ/src/app"
cat > "$CONS_PROJ/build.tur" <<'EOF'
(defpackage prelude-cons :name "prelude-cons" :version "0.1.0")
EOF
cat > "$CONS_PROJ/src/app/main.tur" <<'EOF'
(defmodule app/main
  (defn list-head [l : int] : int
    ```c struct __tur_cell_t { int64_t head; int64_t tail; };
    return ((struct __tur_cell_t *)(intptr_t)l)->head; ```)
  (defn list-tail [l : int] : int
    ```c struct __tur_cell_t { int64_t head; int64_t tail; };
    return ((struct __tur_cell_t *)(intptr_t)l)->tail; ```)
  (defn main [] : int
    ;; Build a cons list with the runtime `cons` builtin and walk two cells.
    ;; head=11, head(tail)=22 -> 33 proves cons cells are allocated and the
    ;; {head,tail} layout matches the list.tur walkers.
    (let [lst (cons 11 (cons 22 0))]
      (+ (list-head lst) (list-head (list-tail lst))))))
EOF
cons_out=$(cd "$WORK" && "$TUR" build "$CONS_PROJ" -o "$WORK/consbin" 2>&1)
cons_rc=$?
if [ $cons_rc -ne 0 ]; then
    fail "build-project-prelude-cons" "tur build exit=$cons_rc: $cons_out"
else
    "$WORK/consbin"
    cons_run_rc=$?
    if [ "$cons_run_rc" -eq 33 ]; then
        pass "build-project-prelude-cons"
    else
        fail "build-project-prelude-cons" "exit=$cons_run_rc (expected 33)"
    fi
fi

# file-scope-c-block-emit-order: a module with a file-scope ```c typedef block
# placed AFTER the defns that use it.  Without the Pass 1a pre-pass fix in
# emit_implementation, the typedef lands after the function bodies in the
# generated .c and cc fails with "use of undeclared identifier".  With the fix
# all EX_INLINE_C blocks are emitted before any defn body regardless of source
# position in the flat item array.
CBLOCK="$WORK/cblock"
mkdir -p "$CBLOCK/src"
cat > "$CBLOCK/build.tur" <<'EOF'
(defpackage cblock-order :name "cblock-order" :version "0.1.0")
EOF
# __mk-cell references tur_cell_t, which is declared in the file-scope C block.
# The C block appears AFTER __mk-cell in source.  Without the fix the typedef
# ends up after the function body in the generated .c and cc rejects it.
cat > "$CBLOCK/src/main.tur" <<'EOF'
(defmodule main
  (defn main [] : int
    (__mk-cell 21))

  (defn __mk-cell [v : int] #{Unsafe} : int
    ```c
    tur_cell_t *p = (tur_cell_t *)malloc(sizeof(tur_cell_t));
    p->val = v * 2;
    return p->val;
    ```)

  ```c
  typedef struct { int64_t val; } tur_cell_t;
  ```)
EOF
cblock_out=$(cd "$WORK" && "$TUR" build "$CBLOCK" -o "$WORK/cblockbin" 2>&1)
cblock_rc=$?
if [ $cblock_rc -ne 0 ]; then
    fail "build-project-cblock-emit-order" "tur build exit=$cblock_rc: $cblock_out"
else
    "$WORK/cblockbin"
    cblock_run_rc=$?
    if [ "$cblock_run_rc" -eq 42 ]; then
        pass "build-project-cblock-emit-order"
    else
        fail "build-project-cblock-emit-order" "exit=$cblock_run_rc (expected 42)"
    fi
fi

# project-mode-defstruct-typedef-missing: a (defstruct ...) defined in a
# project-mode module must emit `typedef struct Name { ... } Name;` into the
# generated header so both the owning .c and any importing modules see the
# type.  Without the fix, the .h was missing the typedef and the .c emitted a
# spurious `Name Name_N;` variable declaration referring to an undeclared
# type, failing cc with "unknown type name".
DSPROJ="$WORK/defstruct-proj"
mkdir -p "$DSPROJ/src/foo"
cat > "$DSPROJ/build.tur" <<'EOF'
(defpackage tur-defstruct-proj
  :name    "tur-defstruct-proj"
  :version "0.1.0"
  :exports #{ "foo/box" ["Box" "make-box" "box-x"] })
EOF
# Box is declared via defstruct; make-box returns a heap pointer cast via the
# Box typedef in inline-C, which only links if `typedef struct Box {...} Box;`
# is visible.  box-x reads the x field as proof the layout matches.
cat > "$DSPROJ/src/foo/box.tur" <<'EOF'
(defmodule foo/box
  (export Box make-box box-x)
  (defstruct Box [x : float y : float])
  (defn make-box [a : float b : float] : int
    ```c
    typedef struct { double x; double y; } Box_;
    Box_ *p = (Box_ *)malloc(sizeof(*p));
    p->x = a; p->y = b;
    return (int64_t)(intptr_t)p;
    ```)
  (defn box-x [b : int] : float
    ```c
    typedef struct { double x; double y; } Box_;
    return ((Box_ *)(intptr_t)b)->x;
    ```)
  (defn fto-i [x : float] : int
    ```c return (int64_t)x; ```)
  (defn main [] : int
    (fto-i (box-x (make-box 21.0 0.0)))))
EOF
ds_out=$(cd "$WORK" && "$TUR" build "$DSPROJ" -o "$WORK/dsbin" 2>&1)
ds_rc=$?
if [ $ds_rc -ne 0 ]; then
    fail "build-project-defstruct-typedef" "tur build exit=$ds_rc: $ds_out"
else
    "$WORK/dsbin"
    ds_run_rc=$?
    if [ "$ds_run_rc" -eq 21 ]; then
        pass "build-project-defstruct-typedef"
    else
        fail "build-project-defstruct-typedef" "exit=$ds_run_rc (expected 21)"
    fi
fi

# load-not-expanded-in-imported-or-project-modules: a module that does a
# top-level (load "stdlib/...") of a bare-defn stdlib file, consumed across
# modules in a `tur build <dir>` project.  The (load ...) must be expanded in
# separate-compilation (project) mode -- exactly as the entry unit does -- and
# the spliced bare defns (sqrt/floor from stdlib/math.tur) must reach codegen so
# the exported defn that calls them links.  Before this fix the load either
# survived to elab_load ("load is only valid at the top level") or its spliced
# defns never reached the per-module .c.
LOADP="$WORK/loadproj"
mkdir -p "$LOADP/src/app"
cat > "$LOADP/build.tur" <<'EOF'
(defpackage tur-loadproj
  :name    "tur-loadproj"
  :version "0.1.0"
  :exports #{ "app/main" ["main"] "app/ops" ["hypot-floor"] })
EOF
# app/ops top-level-loads stdlib/math.tur (bare defns) and exports a defn that
# uses the spliced sqrt/floor.  app/main imports it cross-module.
cat > "$LOADP/src/app/ops.tur" <<'EOF'
(load "stdlib/math.tur")

(defmodule app/ops
  (export hypot-floor)
(defn hypot-floor [a : float b : float] : int
  (float->int (floor (sqrt (+ (* a a) (* b b)))))))
EOF
cat > "$LOADP/src/app/main.tur" <<'EOF'
(defmodule app/main
  (import app/ops :refer [hypot-floor])
  (defn main [] : int
    ;; floor(sqrt(3*3 + 4*4)) = floor(5.0) = 5 -- the process exit code.
    (hypot-floor 3.0 4.0)))
EOF
loadp_out=$(cd "$WORK" && "$TUR" build "$LOADP" -o "$WORK/loadpbin" 2>&1)
loadp_rc=$?
if [ $loadp_rc -ne 0 ]; then
    fail "build-project-load-bare-defn-module" "tur build exit=$loadp_rc: $loadp_out"
else
    "$WORK/loadpbin"
    loadp_run_rc=$?
    if [ "$loadp_run_rc" -eq 5 ]; then
        pass "build-project-load-bare-defn-module"
    else
        fail "build-project-load-bare-defn-module" "exit=$loadp_run_rc (expected 5)"
    fi
fi

# load-not-expanded-in-imported-or-project-modules (codegen): a module that
# top-level-(load ...)s a *runtime-preamble-dependent* stdlib file -- here
# stdlib/either.tur, which brings in the `Either` ADT and a higher-kinded
# `Functor` instance whose `fmap` dispatches through `tur_poly_fn_t` -- must
# build in separate-compilation (project) mode.  This exercises the per-module
# emission of the base ADT typedef + constructors, the `tur_poly_fn_t` carrier,
# and the on-demand fn-ptr typedefs that the whole-program preamble provides but
# the separate-compilation path historically omitted (unknown type name
# 'tur_poly_fn_t' / 'tur_adt_Either').  fmap (*2) over (Right 21) = 42.
LOADHK="$WORK/loadhk"
mkdir -p "$LOADHK/src/app"
cat > "$LOADHK/build.tur" <<'EOF'
(defpackage tur-loadhk
  :name    "tur-loadhk"
  :version "0.1.0"
  :exports #{ "app/main" ["main"] })
EOF
cat > "$LOADHK/src/app/main.tur" <<'EOF'
(load "stdlib/either.tur")

(defmodule app/main
  (defn main [] : int
    (let [e (Right 21)]
      (match (fmap e (fn [x : int] : int (* x 2)))
        (Left l)  l
        (Right r) r))))
EOF
loadhk_out=$(cd "$WORK" && "$TUR" build "$LOADHK" -o "$WORK/loadhkbin" 2>&1)
loadhk_rc=$?
if [ $loadhk_rc -ne 0 ]; then
    fail "build-project-load-higher-kinded-module" "tur build exit=$loadhk_rc: $loadhk_out"
else
    "$WORK/loadhkbin"
    loadhk_run_rc=$?
    if [ "$loadhk_run_rc" -eq 42 ]; then
        pass "build-project-load-higher-kinded-module"
    else
        fail "build-project-load-higher-kinded-module" "exit=$loadhk_run_rc (expected 42)"
    fi
fi

# load-not-expanded-in-imported-or-project-modules (^fat / closure runtime): a
# module that loads arrow.tur and composes via the bare `>>>` arrow exercises
# the `^fat` auto-shim (__tur_fatshim1) and the closure-application macros that
# the whole-program runtime preamble provides but separate compilation must emit
# per module .c.  arrow.tur is also self-contained re: tuple.tur's `Tuple2` (it
# uses a local layout-compatible pair struct), so no ambient stdlib type leaks.
# (3+1)*2 = 8.
ARROWP="$WORK/arrowproj"
mkdir -p "$ARROWP/src/app"
cat > "$ARROWP/build.tur" <<'EOF'
(defpackage tur-arrowproj
  :name    "tur-arrowproj"
  :version "0.1.0"
  :exports #{ "app/main" ["main"] })
EOF
cat > "$ARROWP/src/app/main.tur" <<'EOF'
(load "stdlib/arrow.tur")

(defmodule app/main
  (defn main [] : int
    (let [inc (fn [x : int] : int (+ x 1))
          dbl (fn [x : int] : int (* x 2))
          c   (>>> inc dbl)]
      (c 3))))
EOF
arrowp_out=$(cd "$WORK" && "$TUR" build "$ARROWP" -o "$WORK/arrowpbin" 2>&1)
arrowp_rc=$?
if [ $arrowp_rc -ne 0 ]; then
    fail "build-project-load-arrow-fatshim" "tur build exit=$arrowp_rc: $arrowp_out"
else
    "$WORK/arrowpbin"
    arrowp_run_rc=$?
    if [ "$arrowp_run_rc" -eq 8 ]; then
        pass "build-project-load-arrow-fatshim"
    else
        fail "build-project-load-arrow-fatshim" "exit=$arrowp_run_rc (expected 8)"
    fi
fi

# load-not-expanded-in-imported-or-project-modules (cross-module instance): a
# module A that (load ...)s a higher-kinded typeclass instance (either.tur's
# Functor [Either]) and is consumed by a module B via `import`.  The instance
# must NOT leak into B's translation unit (where the owner's internal ADT
# typedef is absent); B calls A's exported entry point and A dispatches through
# its own dictionary.  fmap (*2) over (Right 21) = 42.
IMPHK="$WORK/imphk"
mkdir -p "$IMPHK/src/app"
cat > "$IMPHK/build.tur" <<'EOF'
(defpackage tur-imphk
  :name    "tur-imphk"
  :version "0.1.0"
  :exports #{ "app/main" ["main"] "app/lib" ["run"] })
EOF
cat > "$IMPHK/src/app/lib.tur" <<'EOF'
(load "stdlib/either.tur")

(defmodule app/lib
  (export run)
(defn run [] : int
  (let [e (Right 21)]
    (match (fmap e (fn [x : int] : int (* x 2)))
      (Left l)  l
      (Right r) r))))
EOF
cat > "$IMPHK/src/app/main.tur" <<'EOF'
(defmodule app/main
  (import app/lib :refer [run])
  (defn main [] : int (run)))
EOF
imphk_out=$(cd "$WORK" && "$TUR" build "$IMPHK" -o "$WORK/imphkbin" 2>&1)
imphk_rc=$?
if [ $imphk_rc -ne 0 ]; then
    fail "build-project-import-higher-kinded" "tur build exit=$imphk_rc: $imphk_out"
else
    "$WORK/imphkbin"
    imphk_run_rc=$?
    if [ "$imphk_run_rc" -eq 42 ]; then
        pass "build-project-import-higher-kinded"
    else
        fail "build-project-import-higher-kinded" "exit=$imphk_run_rc (expected 42)"
    fi
fi

# parametric-struct-by-value-carrier-inconsistency: a project module with a
# generic parametric struct (`Box2 [A B]`), a by-value constructor, and a
# by-value accessor.  Whole-program prunes the generic templates and emits the
# monomorphized `Box2__int__int` struct by value; separate compilation must do
# the same -- prune the invalid generic carrier bodies AND emit the
# monomorphized struct-app typedef in the header before the spec-clone decls.
# box-1st(mk-box(21, 99)) = 21.
PSTRUCT="$WORK/pstruct"
mkdir -p "$PSTRUCT/src/app"
cat > "$PSTRUCT/build.tur" <<'EOF'
(defpackage tur-pstruct
  :name    "tur-pstruct"
  :version "0.1.0"
  :exports #{ "app/main" ["main"] })
EOF
cat > "$PSTRUCT/src/app/main.tur" <<'EOF'
(defmodule app/main
(defstruct Box2 [A B] (e1 A) (e2 B))
(defn mk-box [A B] [a :A b :B] : (Box2 A B) (make-struct Box2 a b))
(defn box-1st [A B] [t : (Box2 A B)] :A (.e1 t))
  (defn main [] : int
    (box-1st (mk-box 21 99))))
EOF
pstruct_out=$(cd "$WORK" && "$TUR" build "$PSTRUCT" -o "$WORK/pstructbin" 2>&1)
pstruct_rc=$?
if [ $pstruct_rc -ne 0 ]; then
    fail "build-project-parametric-struct-by-value" "tur build exit=$pstruct_rc: $pstruct_out"
else
    "$WORK/pstructbin"
    pstruct_run_rc=$?
    if [ "$pstruct_run_rc" -eq 21 ]; then
        pass "build-project-parametric-struct-by-value"
    else
        fail "build-project-parametric-struct-by-value" "exit=$pstruct_run_rc (expected 21)"
    fi
fi

echo
echo "summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
