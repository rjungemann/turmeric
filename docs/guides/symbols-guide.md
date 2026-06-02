# Runtime Symbols: `:Sym`

> **Status:** experimental, opt-in behind `-Xsymbols`. All phases (SYM0--SYM6)
> are implemented: the `:Sym` type, per-TU codegen, cross-TU interning,
> map-literal + typeclass integration, the stdlib surface, and the opt-in
> dynamic `str->sym` intern table.

Turmeric's `:foo` keyword syntax has always parsed cleanly and interned its
name at read time, but in *expression position* a keyword had no value and no
type. With `-Xsymbols`, `:foo` becomes a first-class expression of type
`:Sym` whose runtime value is a unique, deduplicated pointer into `.rodata`.

```turmeric
;; tur run -Xsymbols / tur build -Xsymbols
(println (sym->str :hello))   ; => hello
```
```sweet-exp
;; tur run -Xsymbols / tur build -Xsymbols
println(sym->str(:hello))
; => hello
```

## The model

A `:Sym` value is a non-null pointer to a static, process-lifetime record:

```c
struct __tur_sym {
    uint64_t hash;   /* precomputed xxHash64 of the name */
    uint32_t len;    /* byte length, excluding NUL */
    uint32_t _pad;
    char     name[]; /* NUL-terminated UTF-8 */
};
```

Every distinct `:foo` lowers to **one** record, so:

- **Equality is pointer equality** -- two `:foo` references are the same
  pointer (`==`); `:foo` and `:bar` are distinct.
- **Hashing is a single field load** -- skipping any string work or allocation. The
  hash is precomputed by the compiler.
- **`sym->str` is free** -- it returns the address of the embedded name; no
  copy, no `strlen`.

The C identifier of each record is `__tur_sym_` followed by a percent-encoded
mangling of the name, so punctuated keywords (`:a-b`, `:*x*`) still produce a
unique, ASCII-only symbol.

In a multi-module build (`tur build <dir>`) the records are emitted with
external weak linkage, so the linker folds the per-module copies of `:foo`
into a single object -- `:foo` is one pointer across the whole program, not
just within a single file. Single-file `emit-c` output keeps the records
`static` so it stays self-contained.

## Expression position vs. syntactic position

Adding a runtime value for `:foo` does **not** change any of its existing
syntactic uses. These are consumed by earlier elaborator passes and behave
identically with or without `-Xsymbols`:

- Type annotations: `:int`, `:cstr`, `:Sym`
- Import directives: `:refer [...]`, `:as foo`
- Struct field selectors: `(:name p)`
- `:else` in `cond`/`case`

The only behavioral change is in **expression position**, where `:foo` was
previously a hard error. Without the flag it still is:

```
error: keyword in expression position requires -Xsymbols
```

## The `:Sym` type

`:Sym` is a nullary type that prints as `:Sym`. It is **not** a subtype of
`cstr`; conversion is explicit via `sym->str`. A value of type `:Sym` is
freely copyable (it is just an interned pointer).

You can annotate parameters and returns with it:

```turmeric
(defn label [tag :Sym] :cstr
  (sym->str tag))
```
```sweet-exp
defn label [tag :Sym] :cstr
  sym->str(tag)
```

## Stdlib surface (`stdlib/sym.tur`)

`sym.tur` is auto-loaded into the global namespace **only** under `-Xsymbols`
(so default builds and their codegen snapshots are unaffected). It provides:

| Function | Signature | Notes |
|---|---|---|
| `sym->str` | `(sym->str s :Sym) :cstr` | embedded name, no allocation |
| `sym=?`    | `(sym=? a :Sym b :Sym) :bool` | pointer-identity equality |

```turmeric
(sym->str :hello)   ; => "hello"
(sym=? :a :a)       ; => true
(sym=? :a :b)       ; => false
```
```sweet-exp
sym->str(:hello)
; => "hello"
sym=?(:a :a)
; => true
sym=?(:a :b)
; => false
```

## Equality, hashing, and maps

Under `-Xsymbols`, `:Sym` participates in the `Eq`, `Hash`, and `MapKey`
typeclasses (instances live in `stdlib/sym.tur`, auto-loaded with the flag):

```turmeric
(eq? :foo :foo)   ; => true   (pointer identity)
(eq? :foo :bar)   ; => false
(hash :foo)       ; precomputed field load
```
```sweet-exp
eq?(:foo :foo)
; => true   (pointer identity)
eq?(:foo :bar)
; => false
hash(:foo)
; precomputed field load
```

With `-Xdata-literals` as well, a keyword key in a map literal is a
first-class `:Sym` key rather than a content-hashed string:

```turmeric no-check
(let [m #map{:foo 10 :bar 20}]
  (map-get m :foo)      ; => 10
  (map-has? m :missing) ; => false (0))
```
```sweet-exp
let [m #map{:foo 10 :bar 20}]
  map-get(m :foo)
  ; => 10
  map-has?(m :missing)
  ; => false (0))
```

The map is keyed by `Sym` pointer identity (via `Hash[Sym]` + `MapKey[Sym]`),
so lookups are pointer comparisons with the precomputed hash -- no string
work. Without `-Xsymbols`, keyword map keys keep the legacy content-hashed
lowering, and string keys (`#map{"foo" 1}`) are unchanged in either mode.

## Dynamic interning: `str->sym` (opt-in)

Literal `:foo` symbols need no runtime table. To build a symbol from a string
at runtime (deserialization, REPL tools), load the opt-in module:

```turmeric
(load "stdlib/sym-dynamic.tur")

(eq? :hello (str->sym "hello"))   ; => true  (same pointer as the literal)
(eq? (str->sym "x") (str->sym "x")) ; => true  (stable; one record allocated)
(sym->str (str->sym "round"))     ; => "round"
```
```sweet-exp
load("stdlib/sym-dynamic.tur")
eq?(:hello str->sym("hello"))
; => true  (same pointer as the literal)
eq?(str->sym("x") str->sym("x"))
; => true  (stable; one record allocated)
sym->str(str->sym("round"))
; => "round"
```

`str->sym` returns a stable, process-lifetime symbol; repeated calls with the
same name return the same pointer (allocating at most one record), and the
table is mutex-guarded for concurrent use. For a name that also appears as a
literal in the program, `str->sym` returns the **same pointer** as the literal
(a startup constructor seeds the table with the static records).

Keep it opt-in: loading `sym-dynamic.tur` links the process-global intern
table (`src/runtime/symbols.c`). Programs that use only literal `:foo` symbols
never link it. `str->sym` hashes and locks, so it is slower than a literal --
prefer literals on hot paths.

> **Multi-module note:** `str->sym("foo")` matches a literal `:foo` only when
> that literal appears in a module that also loads `sym-dynamic.tur` (or is
> linker-folded with one). A `:foo` confined to a module that never loads the
> dynamic surface is interned as a separate record. Single-file programs are
> unaffected.

See `docs/archive/history/runtime-symbols-plan.md` for the full phase breakdown.
