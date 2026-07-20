# Strings in Turmeric -- `cstr` vs `str` vs `String`

Turmeric has three string-shaped types. They are not interchangeable, and the
difference that matters is **ownership**: who is responsible for the bytes, and
how long they stay valid.

## The three tiers

| Type | Owns bytes? | Has length? | Typeclasses | Reach for it when ... |
|---|---|---|---|---|
| `cstr` | no (borrowed `const char*`) | no (NUL-terminated) | `Eq`, `Show` | string literals, FFI boundaries, static text |
| `str` | no (borrowed pointer+len view) | yes | `Eq` | a zero-copy sub-view over a buffer you already own |
| `String` | **yes** (owned, immutable, refcounted) | yes | `Eq`, `Ord`, `Show`, `Hash`, `Clone`, `MapKey` | a string that must **outlive its source** |

### `cstr` -- borrowed literal / FFI

`cstr` is a bare `const char*`: no stored length, no ownership. It is the type
of every `"..."` literal and the right type at an FFI boundary. `Eq[cstr]` is a
content `strcmp` and `Show[cstr]` is identity, so at the scalar level a `cstr`
already behaves like a value -- but it **borrows**. The moment you store a
`cstr` somewhere that outlives its buffer, or use a *computed* `cstr` as a
Map/Set key, you have a latent dangling pointer.

### `str` -- borrowed view

`str` (`stdlib/str.tur`) is a borrowed pointer+length view: a zero-copy window
onto bytes some other buffer owns. Useful for substring-without-copy over a
buffer whose lifetime you already control. Still borrowed, still not a safe
owning key.

### `String` -- owned, immutable, refcounted

`String` (`stdlib/string.tur`) **owns** its bytes. It is a refcounted immutable
heap payload (`{ rc; len; bytes[len+1] }`, NUL-terminated), so:

- `Clone[String]` is an O(1) retain (refcount++), and structural sharing in a
  persistent Map/Set is free.
- It is immutable, so there is no torn-read hazard and every transform returns a
  fresh `String`.
- As a `MapKey`, `mk-owned? = 1`: the map **copies and owns** the key bytes, so a
  `String` key never dangles the way a computed `cstr` key does.

## The decision, in one line

> Use `cstr` for a literal or an FFI call. Use `String` the moment the string
> must outlive the thing that produced it -- a returned value, a stored field, or
> a collection key. Use `str` only for a genuine zero-copy sub-view.

Literals stay `cstr` (that is exactly right for a literal). Do **not** do a
big-bang `cstr -> String` sweep: most `cstr` sites are borrowed literals where
`cstr` is correct and `String` would just add a copy.

## Why `String` keys don't dangle

```turmeric
(load "stdlib/string.tur")

(defn make-key [] : String                 ;; bytes on the heap, not a literal
  (let [b (builder/new)]
    (do (builder/push-cstr! b "al")
        (builder/push-cstr! b "pha")
        (builder/finish b))))

(defn demo [] : int
  (let [k (make-key)
        m (map-assoc (:: (map-new) (Map String int)) k 42)]
    (do
      (string/release k)                    ;; drop the SOURCE key
      (let [probe (string/from-cstr "alpha")] ;; independent, equal key
        (do
          (println (map-get m probe))       ;; => 42  (map owns its own copy)
          (string/release probe)
          (map-free m)                      ;; frees the owned key box once
          0)))))
```

The same program with a *computed* `cstr` key would read freed memory after the
source was dropped. `MapKey[String]` boxes the bytes into a key the map owns and
frees exactly once (the WKC2 owned-key path), which is the whole point of the
type.

## API summary

Construct / convert:

- `string/from-cstr` -- copy a `cstr` into a fresh owned `String`.
- `string/to-cstr` -- borrow the NUL-terminated payload (O(1); valid only while
  the `String` is retained).

Refcount:

- `string/retain` / `string/release` -- share / drop an owning handle.

Query:

- `string/len`, `string/empty?`, `string/byte-at`, `string/eq?`,
  `string/compare`, `string/hash`, `string/starts-with?`, `string/ends-with?`,
  `string/contains?`.

Transform (each returns a fresh immutable `String`):

- `string/concat`, `string/substring`, `string/to-upper`, `string/to-lower`,
  `string/trim`.

Incremental construction:

- `StringBuilder` (`builder/new`, `builder/push-cstr!`, `builder/push-string!`,
  `builder/push-byte!`, `builder/len`, `builder/finish`) -- accumulate bytes in
  linear time, then `builder/finish` freezes them into an immutable `String`.
  This is the mutable escape hatch; `String` itself stays immutable.

Typeclasses: `Eq`, `Ord` (lexicographic), `Show` (the bytes), `Hash` (content),
`Clone` (retain), `MapKey` (owned key). Every op has an interpreter native
override, so `String` behaves identically under `--interpret` / the REPL and
when compiled.

## Lifetimes -- the rules

- A freshly constructed `String` (`from-cstr`, `concat`, `substring`, `to-*`,
  `trim`, `builder/finish`) has refcount 1 and is owned by its creator. Release
  it with `string/release`, or hand ownership to a container.
- `string/retain` hands out a second owning handle; balance every `retain` with a
  `release`.
- `string/to-cstr` returns a **borrow** into the payload -- do not use it after
  the `String` is released.
- A `String` used as a Map/Set key is copied into a key box the collection owns;
  the collection frees it on `map-free` / `set-free`. Releasing your own handle
  afterward is independent and safe.

## See also

- `stdlib/string.tur` -- the module.
- `src/runtime/tur_string.c` -- the refcounted payload + operations.
- `docs/upcoming/v2/owned-string-type-plan.md` -- the design plan.
