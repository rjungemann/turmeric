---
title: Stdlib Effect Rows on I/O-Touching Modules
category: Planning
description: Annotate I/O-touching stdlib modules (fs, process, env, net, random, log, time, httpd, csv, json) with coarse effect tags (#{FS}, #{Net}, #{Proc}, #{Rand}). One-pass annotation — no inference, no masking — that gives the type system capability discipline without changing semantics.
---

# Stdlib Effect Rows on I/O-Touching Modules -- Plan

> **Status:** COMPLETED. Capability tags (`#{IO FS Net Proc Rand}`) ship in
> `stdlib/effects.tur` via the new `^capability` `defeffect` marker; the
> I/O-touching modules (`fs`, `process`, `env`, `random`, `net`, `async_socket`,
> `httpd`, `log`, `csv`) carry their canonical tags. `path.tur` and `json.tur`
> turned out to be pure (string / in-memory only) and are intentionally left
> untagged. See
> [docs/guides/effects-system-guide.md](../guides/effects-system-guide.md#capability-effect-tags-capability).
>
> **Type:** stdlib API hardening -- effect annotation
> **Prerequisites:** none (uses existing `#{...}` effect-set syntax).

## Motivation

`#{Unsafe}` is already in use (e.g. `stdlib/reactor.tur`) and the
language has effect-set syntax. The I/O-touching stdlib (`fs`,
`process`, `env`, `net`, `random`, `log`, `time`) silently performs side
effects with no signal in the signature. Annotating them with coarse
effect tags is a one-pass change that gives the type system capability
discipline without changing semantics.

Code that doesn't opt into effect checking continues to work unchanged.
Code that does opt in (via a `requires-effect-checking` directive at
module top) gets the discipline.

## Design

Standardise on five new effect tags:

| Tag        | Used by                                   |
|------------|-------------------------------------------|
| `#{IO}`    | umbrella, implied by the others           |
| `#{FS}`    | `fs.tur`, `path.tur`, `tmpfile`           |
| `#{Net}`   | `net.tur`, `async_socket.tur`, `httpd.tur` |
| `#{Proc}`  | `process.tur`, `env.tur`                  |
| `#{Rand}`  | `random.tur`                              |

Annotate existing signatures; no call-site changes required for code
that doesn't opt in.

```turmeric
(defn file-read [path : cstr] #{FS} : Result<Bytes>)
(defn http-get  [url  : cstr] #{Net} : Result<Response>)
(defn spawn     [cmd  : cstr args : List<cstr>] #{Proc} : ChildHandle)
(defn rand-u64  []           #{Rand} : int)
```

## Phasing

1. **E1** -- Land tag definitions in `stdlib/effects.tur`; document in
   `docs/guides/effects-guide.md`.
2. **E2** -- Annotate primary I/O modules: `fs`, `path`, `process`,
   `env`, `random`, `net`.
3. **E3** -- Annotate downstream consumers: `httpd`, `log`, `csv` (via
   fs), `json` (via fs/net helpers).

Each phase can ship as one PR per module batch.

## Out of scope

- **No effect inference**; this pass is purely annotation.
- **No effect masking / handlers**; that is `tur/capability`'s job and
  is unchanged.
- **No mandatory checking**; effect discipline stays opt-in via
  `requires-effect-checking`. A follow-up plan can flip the default
  once stdlib is fully annotated and downstream code has migrated.

## Risks

- **Annotation drift.** A signature added later without `#{FS}` silently
  weakens the discipline for callers that opted in. Add a lint pass
  that flags `fs.tur` / `net.tur` / etc. exports without their canonical
  tag.
- **Tag bikeshed.** Naming is hard. Land the table in E1 with explicit
  "these names are stable after this PR" framing, so E2/E3 churn is
  purely annotation, not renaming.

## Acceptance

- `stdlib/effects.tur` exports all five tags.
- Every signature in the targeted modules carries its canonical tag.
- A fixture that opts into checking and tries to call `file-read` from
  a `#{}` context fails to compile with a clear effect-row diagnostic.
- `bash tests/run.sh` passes with zero `FAIL` lines.
- `tur run docs` regenerated; effect tags appear in the API reference.

## Cross-references

- Independent of [[stdlib-opaque-handle-types-plan]] / linearity /
  session-types work; can land in any order relative to them.
- Split out from the original umbrella `stdlib-advanced-typing-plan`.
