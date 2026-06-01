# Runtime Symbols: `:Sym`

> **Status:** experimental, opt-in behind `-Xsymbols`. Phases SYM0, SYM1,
> and SYM4 are implemented (type machinery, per-TU codegen, stdlib surface).
> Cross-TU interning (SYM2), the map-literal retyping (SYM3), the dynamic
> `str->sym` intern table (SYM5), and `Hash`/`Eq` typeclass dispatch for
> `:Sym` are not yet wired -- see "Not yet implemented" below.

Turmeric's `:foo` keyword syntax has always parsed cleanly and interned its
name at read time, but in *expression position* a keyword had no value and no
type. With `-Xsymbols`, `:foo` becomes a first-class expression of type
`:Sym` whose runtime value is a unique, deduplicated pointer into `.rodata`.

```turmeric
;; tur run -Xsymbols / tur build -Xsymbols
(println (sym->str :hello))   ; => hello
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

Within a translation unit every distinct `:foo` lowers to **one** static
record, so:

- **Equality is pointer equality** -- two `:foo` references are the same
  pointer (`==`); `:foo` and `:bar` are distinct.
- **Hashing is a single field load** -- no string work, no allocation. The
  hash is precomputed by the compiler.
- **`sym->str` is free** -- it returns the address of the embedded name; no
  copy, no `strlen`.

The C identifier of each record is `__tur_sym_` followed by a percent-encoded
mangling of the name, so punctuated keywords (`:a-b`, `:*x*`) still produce a
unique, ASCII-only symbol.

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

## Not yet implemented

The following phases of `docs/runtime-symbols-plan.md` are not wired yet;
attempting to rely on them will not work as the plan describes:

- **SYM2 (cross-TU interning).** Records are emitted `static` per translation
  unit. A single-binary build (`tur build <file>` / `emit-c`) is correct
  because the whole program is one unit; a multi-`.c` link does not yet fold
  `:foo` from different units into one pointer.
- **SYM3 (map-literal integration + typeclass dispatch).** `#map{:foo 1}`
  still uses the legacy `hamt/hash-str` lowering, and `(eq? :foo :foo)` /
  `(hash :foo)` do **not** resolve to a `:Sym` instance yet. Use `sym=?` and
  the precomputed `hash` field (via inline-C) instead.
- **SYM5 (`str->sym`).** No runtime intern table; only literal keywords
  produce symbols.

See `docs/runtime-symbols-plan.md` for the full phase breakdown.
