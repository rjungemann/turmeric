# Turmeric

A Lisp that compiles to C99.

## What

Turmeric is a statically-typed Lisp compiler that targets C99. Write expressive Lisp code and get fast, portable C out. The compiler (`tur`) elaborates surface syntax into a typed IR, performs borrow checking, reference-count elision, and then emits C source that you can compile with any C99 compiler.

## Why

Turmeric exists to explore the intersection of Lisp expressiveness and systems-level control — closures, algebraic effects, borrow checking, reference counting, and typeclasses — without a runtime VM or garbage collector as the default execution model. The generated C is readable and can be embedded in any C project.

## Setup

**Prerequisites:**
- A C99-compatible compiler (`cc`, `clang`, or `gcc`)
- CMake 3.20+
- [`just`](https://github.com/casey/just) (optional, but recommended)

**Build the compiler:**

```sh
git clone <repo-url>
cd turmeric
just configure   # cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug ...
just             # debug build with AddressSanitizer + UBSan
just release     # optimized build (no sanitizers)
```

Or with plain CMake:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j
```

The compiler binary lands at `build/tur`.

**Run the tests:**

```sh
just test
# or: ctest --output-on-failure --test-dir build
```

## Usage

```sh
./build/tur build path/to/file.tur     # compile to an executable
./build/tur emit-c path/to/file.tur    # print generated C to stdout
./build/tur run path/to/file.tur       # compile and immediately execute
```

## Examples

**Hello world:**

```lisp
(println "hi")
```

**FizzBuzz:**

```lisp
(let [^mut i 1]
  (while (<= i 100)
    (cond
      (= 0 (mod i 15)) (println "fizzbuzz")
      (= 0 (mod i 3))  (println "fizz")
      (= 0 (mod i 5))  (println "buzz")
      :else            (println i))
    (set! i (+ i 1))))
```

**Closures:**

```lisp
(defn main [] :int
  (let [x 10]
    (let [f (fn [] (+ x 1))]
      (println (f)))))
```

**Algebraic effects:**

```lisp
(defeffect Ask [] :int)

(defn use-ask [] :int
  (+ 1 (perform (Ask))))

(println (handle (use-ask)
  (Ask [] k) (resume k 41)))
; => 42
```

**Effect handler with multiple clauses:**

```lisp
(defeffect Add [x :int] :int)
(defeffect Mul [x :int] :int)

(defn compute [] :int
  (* (perform (Add 3)) (perform (Mul 4))))

(println (handle (compute)
  (Add [x] k) (resume k (+ x 10))
  (Mul [x] k) (resume k (* x 2))))
; => 104
```

**Typeclasses:**

```lisp
(defclass MyEq [a]
  (eq? [x y] :bool))

(definstance MyEq [int]
  (eq? [x y] (= x y)))

(defn main [] :int
  (if (.eq? 1 1) 0 1))
```

**Exceptions:**

```lisp
(defn safe-div [a b]
  (try
    (if (= b 0) (throw "division by zero") (/ a b))
    (catch [e]
      -1)))
```

**Reference counting:**

```lisp
(defn main [] :int
  (let [r (rc/of 42)]
    (println (rc/strong-count r))
    (let [r2 (rc/clone r)]
      (println (rc/strong-count r)))
    (rc/drop r)
    0))
```

**Borrows:**

```lisp
(defn main [] :int
  (let [x 42]
    (let [r1 (& x)]
      (let [r2 (& x)]   ; multiple immutable borrows OK
        (println 1)))
    (let [r3 &mut x]    ; mutable borrow after immutable borrows end
      (println 2)))
  0)
```

**FFI / inline C:**

```lisp
(extern-c printf [fmt :cstr] :int)

(println (inline-c "(__builtin_popcount(255))"))
```

## Features

| Feature | Status |
|---|---|
| Reader: vectors, keywords, hex/binary ints, curly-infix, neoteric | ✅ |
| Typed IR, elaborator, operator dispatch | ✅ |
| `def` / `let` / `if` / `do` / `when` / `unless` / `cond` / `set!` / `while` | ✅ |
| `defn`, mutual recursion, multi-file builds | ✅ |
| Closures with capture analysis, env struct synthesis | ✅ |
| `defer` with scope unwind | ✅ |
| Macros (`defmacro`, quasiquote, threading macros) | ✅ |
| `defstruct`, field access, copy/move annotations | ✅ |
| Borrow checker (`&`, `&mut`, reborrow, move semantics) | ✅ |
| Reference counting (`rc/of`, `rc/clone`, `rc/drop`, weak refs) | ✅ |
| RC elision (static analysis to remove redundant retain/release) | ✅ |
| Garbage collector (cycle detection, deterministic mode) | ✅ |
| Typeclasses with dictionary passing (`defclass`, `definstance`) | ✅ |
| Built-in typeclasses: `Eq`, `Ord`, `Show`, `Num` | ✅ |
| Capability passing (v1 effects via typeclasses) | ✅ |
| Exceptions (`try` / `catch` / `finally` / `throw`) | ✅ |
| Delimited continuations (`shift` / `reset`) | ✅ |
| Algebraic effects (`defeffect` / `perform` / `handle` / `resume`) | ✅ |
| `extern-c`, inline-C blocks, FFI | ✅ |
| Standard library: `option`, `result`, `vec`, `str`, `slice`, `io`, `log`, `test` | ✅ |
| Higher-kinded types | 📋 Planned |
| STM (`TVar`, `atomically`, `retry`) | 📋 Planned |
| Persistent collections (HAMT) | 📋 Planned |
| Backtracking / cloneable continuations | 📋 Planned |

## Status

Phases 0–19 are complete. The compiler passes its full fixture test suite with ASan/UBSan clean.

See [docs/turmeric-plan.md](docs/turmeric-plan.md) for the detailed design and roadmap.
