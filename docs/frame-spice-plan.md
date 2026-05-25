# Spice Plan: tur-frame

> **Status:** Draft Plan
> **Last Updated:** 2026-05-24
> **Type:** Spice Design

---

## Overview

One new spice for the `turmeric-spices` monorepo:

| Spice | Tag | Depends on | Purpose |
|-------|-----|------------|---------|
| `tur-frame` | `frame-v0.1.0` | (none, pure Turmeric) | In-memory tabular data (dataframe) |

`tur-frame` is a pure-Turmeric spice modeled on the column-major subset of
R's `data.frame`, pandas `DataFrame`, and Racket's `data-frame`. It stores
data as typed columnar buffers laid out in **Apache Arrow's in-memory format**
(C Data Interface compatible), with all operations -- filter, select, group-by,
join, aggregate, sort, reshape -- implemented in Turmeric.

`tur-frame` has no cmake C dependency of its own. The Arrow C Data Interface is
a header-only ABI (`ArrowSchema` + `ArrowArray` structs, ~200 lines, MIT) that
is vendored into `src/frame/arrow_c.h` -- no link-time dependency on the Arrow
C++ library is required to *use* the format. Optional sub-spices may later add
Parquet / IPC / Feather IO; see [Future spices](#future-spices) below.

Inline-C is limited to:
- buffer allocation (64-byte-aligned `aligned_alloc`),
- validity bitmap fiddling,
- a `memcpy`-style fast path for primitive columns,
- the Arrow C Data Interface struct definitions and release callbacks.

---

## Why Arrow (and why not libarrow)

### Why the Arrow memory format

Arrow's columnar layout is well-specified, language-neutral, and already the
de-facto interchange format for data tooling (DuckDB, Polars, pandas 2.x,
ClickHouse, BigQuery, Snowflake, Spark, R `arrow`). Adopting it as the *in-memory*
representation buys:

1. **Zero-copy interop**: a `tur-frame` value can be handed to any process or
   library speaking the Arrow C Data Interface (Python via PyCapsule, R via
   `nanoarrow`, DuckDB via `arrow_scan`, Polars, etc.) by just exporting two
   structs and a release callback -- no serialization.
2. **Known good column layout**: validity bitmap + offsets + values buffers is
   a battle-tested design covering nullability, variable-length strings, lists,
   and structs without re-inventing layout decisions.
3. **Forward path to Parquet / Arrow IPC**: future IO spices can read those
   formats directly into our buffers with no transcoding step.

### Why not libarrow as a cmake dep

The Apache Arrow C++ runtime is ~300 MB built, drags in jemalloc, snappy, lz4,
brotli, zstd, RE2, gRPC (for Flight), and a full C++17 toolchain. Vendoring it
into a spice would dwarf every other spice in the repo and break the project's
minimal-dep philosophy.

The **Arrow C Data Interface** is the deliberate escape hatch: it is a header-only
ABI for moving Arrow data between runtimes without anyone needing to link
libarrow. We follow that path -- spend ~200 lines on the ABI, implement the
format ourselves, and stay header-only.

### Alternatives considered

| Option | Verdict |
|--------|---------|
| Bundle libarrow C++ | Rejected -- size, build complexity, C++ ABI surface |
| Roll a custom columnar format | Rejected -- loses interop, re-solves layout problems |
| Wrap DuckDB and treat it as the engine | Rejected -- a SQL engine is a different product; `tur-sqlite` already covers the embedded-SQL niche |
| Pure cons-list "alist-of-vectors" | Rejected -- O(n) per column access, no SIMD path, no interop |

---

## Conventions

Standard spice layout:

```
spices/frame/
  build.tur
  src/frame/
    arrow_c.h       -- vendored Arrow C Data Interface header (no .tur file)
    buffer.tur      -- "frame/buffer"  aligned buffer alloc, validity bitmaps
    type.tur        -- "frame/type"    type tags, type predicates, type->str
    column.tur      -- "frame/column"  typed column constructors and accessors
    schema.tur      -- "frame/schema"  named field list, schema diff/merge
    frame.tur       -- "frame/frame"   core data-frame struct + basic ops
    select.tur      -- "frame/select"  select, drop, rename, with-col, slice
    filter.tur      -- "frame/filter"  filter, mask, drop-nulls, sample
    sort.tur        -- "frame/sort"    arrange (multi-key, asc/desc)
    group.tur       -- "frame/group"   group-by, agg, summarize
    join.tur        -- "frame/join"    inner/left/right/full/semi/anti joins
    reshape.tur     -- "frame/reshape" pivot, melt, transpose
    io_csv.tur      -- "frame/csv"     read-csv, write-csv (RFC 4180 subset)
    interop.tur     -- "frame/interop" Arrow C Data Interface import/export
    print.tur       -- "frame/print"   pretty-printing + summary
  tests/frame/
    column_test.tur
    frame_test.tur
    select_test.tur
    filter_test.tur
    sort_test.tur
    group_test.tur
    join_test.tur
    reshape_test.tur
    csv_test.tur
    interop_test.tur
```

---

## Architecture

```
caller
  |
  v
frame/frame     -- the data-frame struct: schema + parallel columns
  |
  +-- frame/schema   -- (name, type, nullable) field list
  +-- frame/column   -- one typed column = { type, length, validity, buffers }
  |     |
  |     v
  |   frame/buffer   -- 64-byte-aligned :ptr<void> buffer + length
  |   frame/type     -- type-tag enum (int32, int64, f32, f64, bool, utf8, ...)
  |
  +-- frame/select   -- pure transforms (return new frame, columns may be shared)
  +-- frame/filter
  +-- frame/sort
  +-- frame/group    -- group-by => grouped-frame -> agg => frame
  +-- frame/join     -- hash join driver for inner/left/right/full/semi/anti
  +-- frame/reshape  -- pivot / melt
  +-- frame/csv      -- streaming CSV reader/writer
  +-- frame/interop  -- Arrow C Data Interface bridge (import & export)
  +-- frame/print    -- ascii table renderer, summary stats
```

Frames are **immutable values** at the API level. Internally, columns can be
shared between frames (selecting a subset of columns does not copy the
underlying buffers). Mutation operations return a new frame whose schema and
column list differ; unchanged columns are re-used by handle.

---

## Type system

Columns carry a type tag (a small integer). v0.1.0 supports the primitive Arrow
types most users need; extension types (timestamps, decimals, lists, structs)
follow in later phases.

```turmeric
;;; Type tags (frame/type).
(type-int32)     ;; => :int  1   (Arrow "i" format)
(type-int64)     ;; => :int  2   (Arrow "l")
(type-float32)   ;; => :int  3   (Arrow "f")
(type-float64)   ;; => :int  4   (Arrow "g")
(type-bool)      ;; => :int  5   (Arrow "b", 1-bit packed)
(type-utf8)      ;; => :int  6   (Arrow "u", variable-length)
(type-date32)    ;; => :int  7   (Arrow "tdD", days since epoch)
(type-timestamp) ;; => :int  8   (Arrow "tsu:", microseconds since epoch, UTC)
(type-null)      ;; => :int  9   (Arrow "n", all-null sentinel)

(type-name t)    ;; => :cstr  e.g. (type-int32) => "int32"
(type-arrow-fmt t) ;; => :cstr  e.g. (type-int32) => "i"
(type-size t)    ;; => :int   byte size of one element (0 for variable-length)
```

---

## Style and option types

```turmeric
;;; field -- schema entry: name + type + nullability.
(defstruct field
  name      :cstr
  type      :int    ;; type tag from frame/type
  nullable  :int)   ;; 0 = no, 1 = yes

;;; csv-opts -- options for read-csv / write-csv.
(defstruct csv-opts
  delim         :int    ;; 0 = default (','); else codepoint of separator
  has-header    :int    ;; 0 = false, 1 = true (default 1)
  null-str      :cstr   ;; string treated as null; 0 (nil ptr) = default ("")
  quote         :int    ;; 0 = default ('"'); else codepoint
  infer-rows    :int)   ;; rows to scan for type inference; 0 = default (100)

(default-csv-opts)   ;; => csv-opts with all defaults filled in

;;; join-opts -- options for *-join family.
(defstruct join-opts
  on-left       :int    ;; cons list of :cstr column names
  on-right      :int    ;; cons list of :cstr column names (same length as on-left)
  suffix-left   :cstr   ;; default "_x"
  suffix-right  :cstr)  ;; default "_y"

(default-join-opts)  ;; => join-opts with defaults
```

---

## Modules and exports

### frame/type

Already shown above. Exports: `type-int32`, `type-int64`, `type-float32`,
`type-float64`, `type-bool`, `type-utf8`, `type-date32`, `type-timestamp`,
`type-null`, `type-name`, `type-arrow-fmt`, `type-size`, `type-numeric?`,
`type-variable-length?`.

---

### frame/buffer

Low-level building blocks. End users rarely call these directly; `frame/column`
wraps them. Documented for spice authors who want to extend the type system.

```turmeric
;; Allocate a 64-byte-aligned :ptr<void> buffer of n bytes.  Returns 0 on OOM.
(buffer-alloc n)                          ;; => :ptr<void>
(buffer-free p)                           ;; => :void
(buffer-size p)                           ;; => :int  (bytes; tracked internally)

;; Validity bitmap (1 bit per row; 1 = valid, 0 = null).
;; Allocates a buffer of ceil(n/8) bytes.
(bitmap-alloc n)                          ;; => :ptr<void>
(bitmap-set p i v)                        ;; => :void   v in {0,1}
(bitmap-get p i)                          ;; => :int    0 or 1
(bitmap-null-count p n)                   ;; => :int

;; Primitive copy helpers.
(buffer-copy dst src offset n-bytes)      ;; => :void
```

---

### frame/column

```turmeric
;; Constructors from cons lists.  Pass nil for the validity arg to mean "all valid".
(column-int32 vs nullable validity)        ;; => column :int
(column-int64 vs nullable validity)
(column-float32 vs nullable validity)
(column-float64 vs nullable validity)
(column-bool vs nullable validity)
(column-utf8 vs nullable validity)         ;; vs = cons list of :cstr

;; Inspection.
(column-type col)                          ;; => :int  type tag
(column-length col)                        ;; => :int
(column-null-count col)                    ;; => :int
(column-valid? col i)                      ;; => :int  0 or 1

;; Element access (boxed; caller checks type).  Returns option-style:
;; some-value if valid, none if null.
(column-get col i)                         ;; => option<any>

;; Typed fast paths (caller has already confirmed the type).  Undefined if
;; the column is the wrong type or i is out of range.
(column-int32-at col i)                    ;; => :int
(column-int64-at col i)                    ;; => :int
(column-float64-at col i)                  ;; => :float
(column-bool-at col i)                     ;; => :int  0 or 1
(column-utf8-at col i)                     ;; => :cstr

;; Transformations (return new columns; do not mutate).
(column-slice col offset length)           ;; => column   (zero-copy view via offset)
(column-take col indices)                  ;; => column   indices = list<:int>
(column-cast col target-type)              ;; => result<column>   numeric-only in v0.1.0

;; Builder API for incremental construction (e.g. by readers).
(column-builder type capacity)             ;; => builder :int
(builder-append-int32 b v)                 ;; => :void
(builder-append-int64 b v)
(builder-append-float64 b v)
(builder-append-bool b v)
(builder-append-utf8 b s)
(builder-append-null b)
(builder-finish b)                         ;; => column   (frees builder)
```

---

### frame/schema

```turmeric
;; Construct a schema from a cons list of field structs.
(schema fields)                            ;; => schema :int

;; Inspection.
(schema-fields s)                          ;; => list<field>
(schema-field-by-name s name)              ;; => option<field>
(schema-index-of s name)                   ;; => option<:int>
(schema-names s)                           ;; => list<:cstr>
(schema-types s)                           ;; => list<:int>
(schema-arity s)                           ;; => :int

;; Schema arithmetic (used by join / select / rename).
(schema-select s names)                    ;; => schema   names = list<:cstr>
(schema-drop s names)                      ;; => schema
(schema-rename s old new)                  ;; => schema
(schema-merge a b)                         ;; => result<schema>   errors on duplicate names
```

---

### frame/frame

```turmeric
;; Build a frame from a schema and a parallel cons list of columns.
;; All columns must share the same length and match the schema's types.
(frame schema columns)                     ;; => result<frame :int>

;; Convenience: build from a flat alist of (name . column) pairs; schema is inferred.
(frame-from-cols pairs)                    ;; => result<frame>

;; Build from a cons list of rows, each row a cons list of values matching schema.
;; Slower than column-wise construction; intended for small literals and tests.
(frame-from-rows schema rows)              ;; => result<frame>

;; Empty frame with a given schema.
(frame-empty schema)                       ;; => frame

;; Inspection.
(frame-nrows f)                            ;; => :int
(frame-ncols f)                            ;; => :int
(frame-schema f)                           ;; => schema
(frame-columns f)                          ;; => list<column>
(frame-column f name)                      ;; => option<column>
(frame-column-at f i)                      ;; => column

;; Equality.  Compares schema + every cell.
(frame=? a b)                              ;; => :int  0 or 1

;; Slicing.
(frame-head f n)                           ;; => frame
(frame-tail f n)                           ;; => frame
(frame-slice f offset length)              ;; => frame
```

---

### frame/select

```turmeric
;; Keep only the named columns, in the given order.
(select f names)                           ;; => result<frame>   names = list<:cstr>

;; Drop the named columns.
(drop-cols f names)                        ;; => result<frame>

;; Rename a column.
(rename f old new)                         ;; => result<frame>

;; Add or replace a column (length must match nrows).
(with-col f name col)                      ;; => result<frame>

;; Apply f : column -> column to one named column; returns a new frame.
(map-col f name col-fn)                    ;; => result<frame>

;; Apply a row-wise function to produce a new derived column.
;; row-fn : frame, :int -> some value of type `out-type`
(mutate f new-name out-type row-fn)        ;; => result<frame>
```

---

### frame/filter

```turmeric
;; Keep rows where mask = 1.  mask must be a bool column of length nrows.
(filter-mask f mask-col)                   ;; => result<frame>

;; Keep rows where pred(frame, i) returns non-zero.
;; pred : (fn [frame :int row :int] :int)
(filter f pred)                            ;; => frame

;; Drop rows that have any null in the named columns (or all columns if nil).
(drop-nulls f names)                       ;; => frame

;; Random sample without replacement.  seed = 0 uses time-based seed.
(sample f n seed)                          ;; => frame

;; De-duplicate rows by the named columns (or all columns if nil).
(distinct f names)                         ;; => frame
```

---

### frame/sort

```turmeric
;; Sort by one or more columns.
;; keys = cons list of (name . direction), where direction = 0 (asc) or 1 (desc).
(arrange f keys)                           ;; => result<frame>

;; Compute the permutation that would arrange f.  Useful for applying the same
;; ordering to other frames.
(arrange-indices f keys)                   ;; => list<:int>

;; Apply a precomputed permutation to a frame.
(reorder f indices)                        ;; => frame
```

Internally uses a stable LSD radix sort for integer keys and a stable
introsort for floats / strings, both implemented inline-C against the column
buffers.

---

### frame/group

```turmeric
;; Group by one or more columns.  Returns an opaque grouped-frame value.
(group-by f names)                         ;; => grouped-frame :int

;; Number of groups.
(grouped-count g)                          ;; => :int

;; Iterate groups: returns a cons list of (key-row . frame) pairs.
(grouped->list g)                          ;; => list<(cons list<any> frame)>

;; Apply aggregations.  aggs = cons list of (out-name . (in-name . agg-tag)) cells.
;; Returns a frame with one row per group: [group-keys..., agg-outputs...].
(agg g aggs)                               ;; => result<frame>

;; Built-in aggregation tags.
(agg-count)        ;; => :int  0
(agg-sum)          ;; => :int  1
(agg-mean)         ;; => :int  2
(agg-min)          ;; => :int  3
(agg-max)          ;; => :int  4
(agg-median)       ;; => :int  5
(agg-std)          ;; => :int  6   (sample std)
(agg-var)          ;; => :int  7
(agg-first)        ;; => :int  8
(agg-last)         ;; => :int  9
(agg-collect)      ;; => :int 10   collects values into a list-typed column (post-v0)

;; Summarize without grouping (whole-frame aggregation).
(summarize f aggs)                         ;; => result<frame>   one-row
```

---

### frame/join

```turmeric
;; All return result<frame>.  opts.on-left / on-right give the join keys.
(inner-join l r opts)                      ;; => result<frame>
(left-join l r opts)                       ;; => result<frame>
(right-join l r opts)                      ;; => result<frame>
(full-join l r opts)                       ;; => result<frame>
(semi-join l r opts)                       ;; => result<frame>
(anti-join l r opts)                       ;; => result<frame>

;; Cross join (Cartesian product); no keys.
(cross-join l r)                           ;; => result<frame>

;; Convenience: same key column name on both sides.
(join l r how keys)                        ;; => result<frame>
                                           ;; how = "inner"|"left"|"right"|"full"|"semi"|"anti"
                                           ;; keys = list<:cstr>
```

Hash join is the only physical strategy in v0.1.0; the build side is whichever
input has fewer rows.

---

### frame/reshape

```turmeric
;; Wide -> long.  id-cols stay; the remaining columns become two output columns
;; named by `var-name` and `value-name`.
(melt f id-cols var-name value-name)       ;; => result<frame>

;; Long -> wide.  Each unique key-col value becomes a new column whose values
;; come from value-col.  index-cols stay as identifying columns.
(pivot f index-cols key-col value-col)     ;; => result<frame>

;; Transpose: rows become columns and vice versa.  All columns must share type.
(transpose f new-key-col)                  ;; => result<frame>
```

---

### frame/csv

```turmeric
;; Read CSV from a path.  Types are inferred from the first opts.infer-rows rows.
(read-csv path opts)                       ;; => result<frame>

;; Read CSV with an explicit schema (skips inference; faster).
(read-csv-typed path schema opts)          ;; => result<frame>

;; Read from an in-memory cstr (entire file content, NUL-terminated).
(read-csv-string s opts)                   ;; => result<frame>

;; Write a frame as CSV.
(write-csv f path opts)                    ;; => result<:void>
(write-csv-string f opts)                  ;; => result<:cstr>
```

Spec: RFC 4180 subset -- comma (or chosen delim) separator, CRLF or LF line
endings on read (LF on write), `"`-quoted fields with `""` escape. No support
in v0.1.0 for: column-name mangling, multi-character delimiters, comment lines,
gzip auto-decoding (those are tracked as future-spice ideas below).

---

### frame/interop

This is the Arrow C Data Interface bridge: zero-copy hand-off between
`tur-frame` and any other runtime speaking that ABI.

```turmeric
;; Export a frame as an (ArrowSchema*, ArrowArray*) pair, both heap-allocated.
;; The caller (often another language runtime) is responsible for calling the
;; structs' release callbacks, which free the buffers.
;; Returns a (cons schema-ptr array-ptr) pair as a list<:ptr<void>>.
(arrow-export f)                           ;; => list<:ptr<void>>

;; Import an external (ArrowSchema*, ArrowArray*) pair into a frame.
;; The caller transfers ownership: tur-frame will invoke the release callbacks
;; when the frame is garbage-collected.
(arrow-import schema-ptr array-ptr)        ;; => result<frame>

;; Export a single column.
(arrow-export-column col name)             ;; => list<:ptr<void>>
(arrow-import-column schema-ptr array-ptr) ;; => result<column>
```

For consumers: Python `pyarrow.Array._import_from_c(ptr_schema, ptr_array)`,
R `nanoarrow::as_nanoarrow_array(...)`, DuckDB `arrow_scan(...)`, Polars
`pl.from_arrow(...)`.

---

### frame/print

```turmeric
;; Pretty-print a frame to stdout (truncates wide frames).
(print-frame f)                            ;; => :void
(print-frame-opts f max-rows max-cols max-col-width)
                                           ;; => :void

;; Render to a :cstr (caller frees with cstr-free).
(frame->str f)                             ;; => :cstr
(frame->str-opts f max-rows max-cols max-col-width)
                                           ;; => :cstr

;; Summary stats per numeric column: count, mean, std, min, 25%, 50%, 75%, max.
;; Returns a frame; print it for pandas-style "describe" output.
(describe f)                               ;; => frame
```

---

## Implementation phases

- [ ] **FR0** -- `build.tur`; vendor `arrow_c.h`; `frame/type` (tags, names,
  formats, sizes); `frame/buffer` (aligned alloc, bitmap helpers); `frame/column`
  for the four numeric primitives (int32, int64, f32, f64) and bool, with
  `column-length`, `column-null-count`, typed `*-at` accessors, `column-slice`;
  `frame/schema`; minimal `frame/frame` (`frame`, `frame-nrows`, `frame-ncols`,
  `frame-column`, `frame-head`, `frame-slice`); `frame/print` ascii table for
  numeric columns. Tag this as a checkpoint -- everything below assumes FR0.

- [ ] **FR1** -- `frame/column` utf8 support (offsets + values buffers);
  `column-utf8-at`; pretty-printing strings (truncate + ellipsis); builder API
  for incremental column construction (used by CSV reader in FR3).

- [ ] **FR2** -- `frame/select`: `select`, `drop-cols`, `rename`, `with-col`,
  `map-col`, `mutate`; `frame/filter`: `filter-mask`, `filter`, `drop-nulls`;
  `frame=?`; tests that compose select + filter.

- [ ] **FR3** -- `frame/csv`: `read-csv` (with type inference over the first
  `opts.infer-rows` lines), `read-csv-typed`, `read-csv-string`, `write-csv`,
  `write-csv-string`; round-trip test (read -> write -> read produces equal
  frame).

- [ ] **FR4** -- `frame/sort`: `arrange`, `arrange-indices`, `reorder`; stable
  LSD radix sort for integer keys; stable introsort for floats and strings.

- [ ] **FR5** -- `frame/group`: `group-by` (hash on key columns), `agg` with
  count / sum / mean / min / max / first / last; `summarize`; tests that
  reproduce a small "group by category, sum amount" workflow end-to-end.

- [ ] **FR6** -- `frame/join`: hash-join driver shared by `inner-join`,
  `left-join`, `right-join`, `full-join`, `semi-join`, `anti-join`; `cross-join`;
  string-key tests.

- [ ] **FR7** -- `frame/reshape`: `melt`, `pivot`, `transpose`; `describe`;
  `agg-median`, `agg-std`, `agg-var` in `frame/group`; `sample` and `distinct`
  in `frame/filter`.

- [ ] **FR8** -- `frame/interop`: Arrow C Data Interface export and import for
  primitive + utf8 columns; release-callback bookkeeping; an integration test
  that exports a frame, re-imports the same pointers, and asserts round-trip
  equality. Document the PyArrow / R / DuckDB / Polars call patterns in the
  README.

- [ ] **FR9** -- `date32` and `timestamp` types in `frame/type` and
  `frame/column`; CSV inference for ISO-8601 dates and `YYYY-MM-DD HH:MM:SS`
  timestamps; `column-cast` for numeric <-> numeric.

- [ ] **FR10** -- Tests pass on all CI targets (Linux, macOS, WASM where
  applicable); README in `turmeric-spices`; `docs/guides/frame-guide.md` (see
  below); `frame-v0.1.0` tag.

---

## Design notes

### Memory layout

Each primitive column owns up to two aligned buffers:

- **validity bitmap** (`:ptr<void>`, optional -- absent if `null-count = 0`):
  one bit per row, little-endian byte order, 1 = valid.
- **values buffer** (`:ptr<void>`): `length * sizeof(type)` bytes for fixed-width
  types; for utf8, this is the data buffer plus a third **offsets buffer** of
  `(length + 1) * 4` bytes (int32 byte offsets into the data buffer).

All buffers are 64-byte aligned (Arrow spec; matches AVX-512 vector width and
typical cache-line size). Allocation is via `aligned_alloc(64, n)` with `n`
rounded up to a multiple of 64.

Bool columns use Arrow's 1-bit packing (not 1 byte per value), matching the
validity bitmap layout. This is the only departure from "obvious" C layouts;
the trade is wire compatibility and 8x memory savings on bool-heavy frames.

### Why immutable values

`with-col`, `filter`, `arrange`, etc. return new frame handles. Columns can be
shared between frames (a `select` does not copy bytes; it builds a new schema +
column-list pointing at the same buffers). This gives:

- Predictable semantics: passing a frame to a function never mutates the
  caller's value.
- Cheap derived frames: `head`, `slice`, `select`, and `rename` are all
  metadata-only.
- A path to thread safety (a frame value is safe to share across threads since
  nothing mutates the buffers after construction).

The cost is that streaming row-by-row construction goes through a column
*builder* rather than mutating an existing column. The builder API
(`column-builder`, `builder-append-*`, `builder-finish`) covers that case.

### Reference counting on shared columns

Columns are reference-counted (a small `:int` header before the buffers). When
the last frame referencing a column drops it, the buffers are freed. The Arrow
C Data Interface release-callback semantics map cleanly onto this: an exported
column increments the refcount and the release callback decrements it.

This is the only place inline-C reference counting appears in v0.1.0; if the
Turmeric stdlib later grows a general `arc`-style refcell, `frame/buffer`
should migrate to it.

### Type inference for CSV

`read-csv` scans the first `opts.infer-rows` rows (default 100) and picks the
narrowest type that fits each column:

1. If every cell parses as int32, use int32.
2. Else if every cell parses as int64, use int64.
3. Else if every cell parses as float64, use float64.
4. Else if every cell parses as `YYYY-MM-DD`, use date32.
5. Else if every cell parses as ISO-8601 timestamp, use timestamp.
6. Else if every cell is "true" / "false" (case-insensitive), use bool.
7. Else use utf8.

Empty cells, the user's `null-str`, and (case-insensitive) `NA` / `NaN` /
`NULL` are treated as null and do not narrow the type.

If a later row violates the inferred type, the reader returns
`err "row N: expected int32, got '...'"`. Use `read-csv-typed` to skip
inference and fail fast.

### Why not lazy evaluation

Polars / Spark / pandas-via-Modin all support a lazy / query-plan API. v0.1.0
of `tur-frame` is **eager-only**: every operation runs immediately and returns
a materialized frame. Reasons:

- The eager API is what users coming from R `data.frame` and Racket
  `data-frame` expect.
- A query planner is a substantial body of code and is best added once we
  have real workloads to optimize against.
- Eager + immutable + cheap metadata-only ops covers most interactive use.

Lazy evaluation is tracked as a post-v0 spice (`tur-frame-lazy`).

---

## Risks and open questions

1. **Null representation in `column-get`.** Returning `option<any>` requires a
   boxed `any` type that the Turmeric stdlib does not currently have.
   Resolution sketch: typed `*-at` accessors are the documented fast path;
   `column-get` returns a tagged union `(cons :int :int)` of (type-tag, value)
   that callers destructure. Revisit if/when stdlib grows an `any`.

2. **Builder lifetime.** A builder is conceptually mutable; we need to make
   sure no two threads append to the same builder. The v0.1.0 docs say
   "single-owner, not thread-safe" and we rely on convention. A future
   `frame/buffer` refactor could move builders behind a stdlib mutex.

3. **Date/time without a stdlib `datetime` type.** v0.1.0 stores date32 as
   `:int` days-since-epoch and timestamp as `:int` microseconds-since-epoch.
   `read-csv` parses common formats; arithmetic and formatting are the
   caller's job. A future `tur-datetime` spice or stdlib addition could give
   us nicer accessors.

4. **String memory pressure.** Utf8 columns hold all string bytes in one
   contiguous buffer; deleting rows does not reclaim string bytes. For frames
   that aggressively filter wide utf8 data, document `compact-strings` as a
   future operation that re-packs the values buffer.

---

## Shared work

### turmeric-spices README

Add row once v0.1.0 is tagged:

| Spice | Description | Tier | Depends on |
|-------|-------------|------|------------|
| `tur-frame` | In-memory dataframe (Arrow-compatible columnar) | 1 -- pure Turmeric | (none) |

### Guide

Deliver `docs/guides/frame-guide.md` alongside the `v0.1.0` tag. Sections:

1. Building a frame from columns and from CSV
2. Selecting, filtering, mutating
3. Sorting and de-duplicating
4. Group-by and aggregation
5. Joining two frames
6. Reshaping with `melt` and `pivot`
7. Handing a frame to Python / R / DuckDB via the Arrow C Data Interface

### Integration notes

- Pair with `tur-sqlite` or `tur-postgres` to round-trip query results
  through a frame (each driver gains a small `frame-from-rows` helper in
  its own spice; no dep added to `tur-frame`).
- Pair with `tur-plot` to draw scatter / histogram / line plots directly
  from frame columns -- a small `frame->plot` adapter in the guide shows
  the pattern (no API addition needed in either spice).
- Pair with `tur-json` for `read-jsonl` / `write-jsonl` interchange in
  user code; a future `tur-frame-json` sub-spice could absorb that.

---

## Future spices

Out of scope for v0.1.0; called out so the public API does not block them.

| Spice | Purpose | Why separate |
|-------|---------|--------------|
| `tur-frame-parquet` | Read/write Parquet files | Pulls in a Parquet C lib (~MB-scale dep) |
| `tur-frame-arrow-ipc` | Read/write Arrow IPC / Feather v2 | Implementable in pure Turmeric, but adds surface area |
| `tur-frame-lazy` | Lazy query plan + optimizer | Substantial new code; needs eager API to settle first |
| `tur-frame-stream` | Streaming readers (chunked CSV, line-delimited JSON) | Different memory model (chunked, not materialized) |
| `tur-frame-compute` | Vectorized expression kernels (SIMD where available) | Optimization layer, separate from semantics |
