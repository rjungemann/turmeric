# MiniKanren-Style Tutorial

A practical introduction to relational programming in Turmeric using a small, runnable example project in `examples/minikanren`.

> **Note:** This guide uses a miniKanren-style approach (relations + bidirectional queries) implemented directly in Turmeric while full stdlib miniKanren support is still being built.

---

## What you will build

The example models a tiny family-graph relation and demonstrates:

- `parento` relation (`parent`, `child`)
- `grandparento` relation composed from `parento`
- Bidirectional queries:
  - known child, unknown parent
  - known parent, unknown child
  - both variables unknown (enumeration)

Code location:

- `examples/minikanren/src/main.tur`

---

## Build and run

From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j --target minikanren
./build/examples/minikanren/minikanren
```

---

## Step 1: Define domain facts

The example encodes people as integer IDs and converts IDs to names with `person-name`.

`parento` acts like a relation predicate:

- returns `1` when a `(parent, child)` pair is true
- returns `0` otherwise

This mirrors the "goal succeeds/fails" shape used in miniKanren.

---

## Step 2: Compose relations

`grandparento` is defined in terms of `parento`:

- iterate over possible middle person (`mid`)
- succeed when `parento(grand, mid)` and `parento(mid, child)` both hold

This demonstrates relational composition (building larger relations from smaller ones).

---

## Step 3: Run directional and bidirectional queries

The example includes:

- `query-parents-of child`
- `query-children-of parent`
- `query-grandparents-of child`
- `print-all-parent-facts` (enumerates all matching pairs)

Although the implementation is explicit loops, the usage pattern matches miniKanren thinking: ask the same relation in different directions.

---

## Example output (abridged)

```text
miniKanren-style relational example
----------------------------------
Parents:
bart
homer
marge
Children:
homer
bart
lisa
Grandparents:
bart
abe
mona
```

---

## Next steps toward full miniKanren

After this tutorial, extend the example toward core miniKanren operators:

- represent terms (`LVAR`, `SYM`, `PAIR`, `NIL`)
- add unification (`=`, `==`)
- add `fresh`, `conde`, and `run` combinators
- reify results into user-friendly outputs

For the full roadmap, see:

- `docs/minikanren-plan.md`
