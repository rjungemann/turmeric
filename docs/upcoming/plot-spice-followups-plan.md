# Follow-up Plan: tur-plot polish and release-readiness

> **Status:** Draft Plan
> **Last Updated:** 2026-05-25
> **Type:** Spice Follow-up / Release Readiness
> **Depends on:** [../plot-spice-plan.md](../plot-spice-plan.md) (the main `tur-plot` implementation is already largely present in `../turmeric-spices/spices/plot`)

---

## Overview

`tur-plot` is no longer a greenfield spice design. The core modules, renderer
constructors, smoke tests, README, and guide already exist in
`../turmeric-spices/spices/plot/`. What remains is a narrower follow-on slice:

1. Finish the only notable planned runtime feature that is still missing:
   adaptive sampling for sampled line renderers.
2. Upgrade test coverage from "non-empty PNG smoke tests" to stronger image
   sanity checks.
3. Close the release/readiness loop so the implementation can honestly match
   the intended `plot-v0.1.0` milestone.

This plan intentionally carries forward only the items that remain open from
`docs/plot-spice-plan.md`. It does not redesign the public API or revisit the
already-implemented renderer surface.

---

## Remaining gaps

- [ ] Adaptive sampling is still missing from the sampled line renderers. The
  current implementation samples uniformly for `function`, `parametric`,
  `polar`, and `inverse`.
- [ ] Tests currently assert successful rendering and non-empty PNG output, but
  do not perform pixel-level `surface-data` sanity checks.
- [ ] The `turmeric-spices` worktree does not currently have a `plot-v0.1.0`
  tag.

---

## Scope

### In scope

- Adaptive sampling in the line-rendering path.
- Regression tests for adaptive-sampling behavior and NaN-gap behavior.
- Pixel-level image sanity checks for the existing renderer suites.
- Final release checklist items needed for a truthful `plot-v0.1.0`.

### Out of scope

- New renderer families or public modules.
- API redesign of `plot/style`, `plot/core`, or any renderer constructor.
- Reworking already-shipped smoke-test coverage except where needed to add
  stronger assertions.

---

## Follow-up phases

- [ ] **PF0** -- Adaptive sampler in `plot/core`: add a reusable subdivision
  helper for sampled curves that bisects intervals when adjacent segments turn
  sharply, with a fixed recursion cap and explicit NaN handling.

- [ ] **PF1** -- Wire adaptive sampling into `plot/line` renderers:
  `function`, `parametric`, `polar`, and `inverse` should use the adaptive
  path; `lines` should retain NaN-gap behavior; docs should describe the actual
  sampling behavior instead of the aspirational one.

- [ ] **PF2** -- Image-level tests: extend `spices/plot/tests/plot/*.tur` so
  renderer suites still write PNGs but also validate pixels or surface data at
  a few targeted coordinates, enough to catch "blank image" or badly-mapped
  output regressions.

- [ ] **PF3** -- Release-readiness pass: confirm the `turmeric-spices` README
  row, spice README, and guide all reflect the shipped API, then create the
  `plot-v0.1.0` tag once PF0--PF2 are complete.

---

## Design notes

### Adaptive sampling target

The original `plot` plan called for a simple adaptive pass, not a fully general
curve-flattening engine. The follow-up should keep that bar: start from the
existing uniform sampler, compare the angle between neighboring segments, and
subdivide only when curvature is high enough to justify extra points.

The important property is not mathematical optimality; it is that obvious
high-curvature regions stop looking under-sampled without forcing every caller
to crank sample counts globally.

### Testing target

The current smoke tests already prove that each renderer path can produce a PNG.
The missing layer is stronger confidence that the image is not merely
"non-empty", but is rendering roughly the expected content in the expected part
of the canvas.

These checks should stay lightweight. A few targeted assertions per suite are
enough: e.g. verify a background pixel remains background-colored, verify a
known plotted region is not background-colored, and verify categorical / contour
cases produce variation across the image.

### Release boundary

This follow-up plan treats `plot-v0.1.0` as a release-readiness boundary, not
as a new feature phase. The tag should come only after the implementation and
tests match what the main plan now claims is shipped.
