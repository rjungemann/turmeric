# Owned-`String` builders & optional accessors -- stdlib

> **Re-verified 2026-07-22:** all Part 1 + Part 2 deliverables confirmed present
> in the tree (`stdlib/str-build-string.tur`, `stdlib/range-bound-string.tur`,
> `stdlib/httpd-string.tur`, the four fixtures, guide prose, and docstring-table
> rows). The ONLY open line is the optional simplification in the "Implementation
> note (Part 2 accessors)" below -- the `*-raw` + Turmeric-wrapper form is still
> in place; collapsing it to a direct inline-C `(Option String)` return is now
> unblocked but has not been done. Everything else is done. This plan is a
> candidate to relocate out of `docs/upcoming/`.
>
> **Status:** LANDED 2026-07-21. Split out of
> [string-adoption-stdlib-plan.md](string-adoption-stdlib-plan.md) as its
> "Bucket B" -- the deferred sites where the mechanical `path-string` recipe
> (one `string/adopt-cstr` wrapper per function) does **not** apply and a real
> design decision is required. Both parts shipped; see the per-part LANDED notes
> below.
>
> **Part 1 landed:** `stdlib/str-build-string.tur` (`str-concat-string`,
> `cstr-sub-string`), the wrap-vs-build guidance in the module docstring and
> [strings-guide.md](../../guides/strings-guide.md), and fixture
> `tests/fixtures/str-build-string`. (`show-concat` left internal/untouched, as
> the plan allows -- no Show-side owned path materialized.)
>
> **Part 2 landed:** `bound->str-string` (in `stdlib/range-bound-string.tur`,
> per-branch adopt/from-cstr) and `httpd-req-cookie-opt` / `httpd-req-form-opt`
> (`(Option String)`, in `stdlib/httpd-string.tur`), additive alongside the
> existing `cstr` accessors. Each `*-opt` is a thin Turmeric wrapper (`some`/
> `none`) over an inline-C `*-raw` helper that returns a fresh `cstr` on a match
> and NULL on any miss. Fixtures: `tests/fixtures/bound-string` (all three
> `Bound` variants incl. the Unbounded no-crash case) and
> `tests/fixtures/httpd-req-string-opt` (cookie/form present / present-empty /
> absent -> `some`/`some ""`/`none`).
>
> **Implementation note (Part 2 accessors).** The `*-opt` accessors assemble the
> `(Option String)` in Turmeric (`some`/`none`) over a low-level `*-raw` helper,
> rather than returning `(Option String)` straight from inline-C via
> `tur_some_ptr` / `tur_none`. That is the workaround a compiler carrier-straddle
> forced (an inline-C-built `(Option String)` could not be passed into a by-value
> `(Option String)` parameter). That straddle has since been **fixed** (a
> carrier->by-value bridge at the call-argument boundary --
> [docs/archive/inline-c-option-byvalue-carrier-straddle.md](../../archive/inline-c-option-byvalue-carrier-straddle.md)),
> so the `*-raw` + wrapper form is no longer *required*; simplifying it to a
> direct inline-C return is an available, optional follow-up.
>
> **Prerequisite:** the owned `String` type and its foundation --
> `string/adopt-cstr`, `string/from-cstr`, `int->string`, and the
> `StringBuilder` surface (`builder/new`, `builder/push-cstr!`,
> `builder/push-string!`, `builder/push-byte!`, `builder/len`,
> `builder/finish`) -- all in [stdlib/string.tur](../../../stdlib/string.tur).

## Why these are not mechanical

The remaining fresh-alloc clusters (`json/encode`, `csv/emit*`, `re/*`,
`term/*`, pure `range*` formatters) always return a freshly-malloc'd `cstr`, so
each gets a one-line `string/adopt-cstr` sibling exactly like `path-string.tur`.
Those stay tracked as a checklist in the parent plan.

The two clusters below are different:

1. **Foundational builders (`str-concat`, `cstr-sub`)** compose into nearly
   every other formatter. A blind `adopt`-wrapper is *correct* but leaves a
   real performance/design fork on the table (repeated concatenation stays
   O(n^2) and allocates a throwaway `cstr` per step). The choice we make here
   dictates how every downstream `*-string` formatter should be written.
2. **Sentinel-mixed accessors** return a fresh malloc on the hit path but a
   **static string literal** on the miss path. `string/adopt-cstr` *frees* its
   argument, so wrapping these frees a static literal -> undefined behavior the
   moment the miss path fires. They need real optional semantics, not a wrapper.

---

## Part 1 -- Foundational owned builders

### Sites

| Site | Current return |
|---|---|
| `stdlib/str-build.tur:30 str-concat` | `malloc(la+lb+1)` + memcpy of both inputs; caller frees. |
| `stdlib/cstr.tur:59 cstr-sub` | `malloc(outlen+1)` + memcpy of `[start,end)`; caller frees. |
| `stdlib/typeclass-show.tur:179 show-concat` | private copy of `str-concat`; mallocs a fresh joined buffer. |

(`int->str` already has its owned sibling `int->string`, landed in batch 1.)

### The design fork

Both `str-concat` and `cstr-sub` always allocate (even the empty result is
malloc'd), so a blind wrapper is memory-safe:

```turmeric
(defn str-concat-string [a : cstr b : cstr] : String
  (string/adopt-cstr (str-concat a b)))
```

The problem is only visible under composition. Today a formatter like
`bound-fmt` nests raw cstr builders:

```turmeric
(str-concat "[" (int->str v))   ;; 2 mallocs, 1 leaked-typed-as-borrowed result
```

A `*-string` formatter built by nesting `adopt(str-concat(...))` inherits the
same quadratic allocation profile for anything longer than one join -- it just
launders the leak, it doesn't remove the intermediate churn. `re/union-acc`
(right-fold of `str-concat`) and `csv/emit-with-delim` (whole-document
accumulation) are the sites where this actually bites.

### Decision (proposed)

Ship **both**, with a documented rule for which to reach for:

- **`str-concat-string` / `cstr-sub-string`** -- thin `string/adopt-cstr`
  wrappers. For the *single-join* case (the common one), these are the right
  tool: minimal, obviously correct, no new surface. Provide them.
- **`StringBuilder` is the sanctioned path for multi-join accumulation.**
  Downstream `*-string` formatters that concatenate more than twice
  (`csv`, `json`, `re/union`, `re/replace-all`) build into a `StringBuilder`
  and `builder/finish` once -- linear, single allocation of the final buffer.
  Do **not** implement those by folding `str-concat-string`.

Document this fork explicitly in the module docstring and in
[docs/guides/strings-guide.md](../../guides/strings-guide.md) so the Bucket-A
executor picks the builder path for the accumulating formatters instead of the
nesting one. This is the whole reason Part 1 lands before the mechanical
clusters: it sets the pattern they follow.

`show-concat` is internal to `typeclass-show`; give it a `String`-returning
internal form only if a `Show`-side owned path materializes -- otherwise leave
it (tracked, not migrated).

### Deliverables

- `str-concat-string`, `cstr-sub-string` in a new opt-in module (proposed
  `stdlib/str-build-string.tur`, `(load ...)` of `str-build`, `cstr`, and
  `string`), each a documented `string/adopt-cstr` wrapper.
- A `;;;`-documented note on each steering multi-join callers to `StringBuilder`.
- Guide paragraph in `strings-guide.md` (wrap-vs-build decision).
- Fixture `tests/fixtures/str-build-string` exercising a single join, a
  substring, and one `StringBuilder`-accumulated multi-join for contrast.

---

## Part 2 -- Sentinel-mixed accessors (optional semantics)

### Sites (confirmed hit-mallocs / miss-returns-static-literal)

| Site | Hit path | Miss path |
|---|---|---|
| `stdlib/httpd.tur:1296 httpd-req-cookie` | `malloc(vlen+1)` + memcpy of decoded value | static `""` (4 distinct early returns: bad args, no header, no match) |
| `stdlib/httpd.tur:1488 httpd-req-form` | `malloc(vlen+1)` + URL-decode | static `""` (bad args, empty body, no match) |
| `stdlib/range-bound.tur:265 bound->str` | fresh `bound-fmt` (`str-concat`) for Inclusive/Exclusive | static `"unbounded"` for `Unbounded` |

### Why a wrapper is wrong

`string/adopt-cstr` takes ownership and **frees the original**. On the miss
path these return a pointer into static/read-only storage (a string literal).
Adopting it calls `free()` on a non-heap pointer -> UB / crash. So the recipe
that works for `path/*` (always-fresh) is unsafe here.

### Decision (proposed): return `option<String>`

`option<String>` is the honest type for "a value that may be absent," and it
sidesteps the static-literal hazard by construction -- the miss path is `none`,
never an adopted literal.

- **`httpd-req-cookie-opt : option<String>`** -- `some(adopt(...))` on the
  match branch, `none` on every miss branch. This is strictly better than the
  `""` sentinel it replaces: today a caller cannot distinguish "cookie present
  and empty" from "cookie absent"; `option` makes that explicit.
- **`httpd-req-form-opt : option<String>`** -- same shape.
- **`bound->str-string : String`** -- here `option` is *not* the right call:
  `Unbounded` has a perfectly good rendering (`"unbounded"`), it is simply not
  heap-allocated. Return a plain `String` and build the miss branch with
  `string/from-cstr "unbounded"` (copy, do not adopt). The Inclusive/Exclusive
  branches adopt the fresh `bound-fmt` result. So the rule is *per-branch*:
  adopt fresh allocations, `from-cstr` static literals.

The generalizable lesson (worth a line in the guide): **for a mixed
fresh/static return, choose per branch -- `adopt` the malloc'd branches,
`from-cstr` the literal branches -- and never blind-`adopt` the whole thing.**
Prefer `option<String>` when absence is semantically meaningful (accessors);
prefer a total `String` when every branch has a valid rendering (formatters).

`schema-error-message` (`stdlib/schema.tur:696`) is **not** in this bucket -- it
always `malloc`s (`sprintf` of all error lines), so its owned sibling
`schema-error-message-string` is a plain `adopt` wrapper and belongs in the
Bucket-A checklist.

### Deliverables

- `httpd-req-cookie-opt`, `httpd-req-form-opt` (`option<String>`) alongside the
  existing `cstr` accessors -- additive, the `cstr` forms stay for callers that
  want the borrow-and-immediately-use path.
- `bound->str-string` (`String`) using per-branch adopt/from-cstr.
- Fixtures: cookie/form present-vs-absent (assert `none` on absent, `some` with
  the right bytes on present); `bound->str-string` over all three `Bound`
  variants (Unbounded must not crash -- the regression this part exists to
  prevent).

---

## Ordering

1. **Part 1** first -- it sets the wrap-vs-build pattern the Bucket-A mechanical
   clusters inherit.
2. **Part 2** second -- independent of Part 1; can land in parallel.

Each deliverable is its own small change with fixtures, per the parent plan's
"one item, one change" discipline.
