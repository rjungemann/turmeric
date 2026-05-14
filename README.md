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
./build/tur repl                       # interactive REPL
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

**Typeclasses:**

```lisp
(defclass MyEq [a]
  (eq? [x y] :bool))

(definstance MyEq [int]
  (eq? [x y] (= x y)))

(defn main [] :int
  (if (.eq? 1 1) 0 1))
```

**Contracts:**

```lisp
(defn safe-div [a :int b :int] :int
  (require! (not= b 0))
  (/ a b))
```

**Set literals:**

```lisp
(def primes #s(2 3 5 7 11 13))
```

**Structural equality:**

```lisp
(println (=struct= [1 2 3] [1 2 3]))   ; => true
(println (=struct= {:a 1} {:a 2}))     ; => false
```

**Defer:**

```lisp
(defn main [] :int
  (let [f (open-file "log.txt")]
    (defer (close-file f))   ; runs on scope exit
    (write-file f "hello")
    0))
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

**FFI / inline C:**

```lisp
(extern-c printf [fmt :cstr] :int)

(println (inline-c "(__builtin_popcount(255))"))
```

**Persistent maps (HAMT):**

```lisp
(let [m (hamt-new)]
  (let [m2 (hamt-set m 1 "one")]
    (println (hamt-get m2 1))))
```

## Features

| Feature | Status |
|---|---|
| Reader: vectors, keywords, hex/binary ints, curly-infix, neoteric | ✅ |
| Set literals (`#s(...)`) | ✅ |
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
| Serializable continuations | ✅ |
| Backtracking / cloneable continuations | ✅ |
| `extern-c`, inline-C blocks, FFI | ✅ |
| Structural equality (`=struct=`) | ✅ |
| Runtime contracts (`require!` / `ensure!` / `assert!` / `invariant!`) | ✅ |
| Standard library: `option`, `result`, `vec`, `str`, `slice`, `io`, `log`, `test` | ✅ |
| Persistent collections (HAMT) | ✅ |
| STM (`TVar`, `atomically`, `retry`) | ✅ |
| Higher-kinded types (Functor, Applicative, Monad, Foldable, Traversable; `^f`/`^^f` syntax) | ✅ |
| Interactive REPL with `:type`, `:doc`, history | ✅ |
| WebAssembly playground | ✅ |

## Status

The compiler passes its full fixture test suite with ASan/UBSan clean. All planned phases through Phase 21 (serializable continuations) are complete.

See [docs/turmeric-plan.md](docs/turmeric-plan.md) for the detailed design and roadmap.
