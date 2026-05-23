# Spice Plan: tur-plot

> **Status:** Draft Plan
> **Last Updated:** 2026-05-22
> **Type:** Spice Design

---

## Overview

One new spice for the `turmeric-spices` monorepo:

| Spice | Tag | Depends on | Purpose |
|-------|-----|------------|---------|
| `tur-plot` | `plot-v0.1.0` | `tur-plutovg` | 2D data visualization |

`tur-plot` is a pure-Turmeric spice modeled on the 2D subset of Racket's `plot`
library. It renders to an off-screen `tur-plutovg` surface and exports PNG (or hands
the surface to the caller). All sampling, coordinate transforms, tick calculation, and
legend layout are implemented in Turmeric; plutovg provides the drawing primitives.

`tur-plot` has no cmake C dependency of its own. It declares `tur-plutovg` as a spice
dependency in `build.tur`. Inline-C is limited to thin wrappers for `<math.h>`
functions (`sin`, `cos`, `log`, `pow`, `floor`, `fmod`) that the stdlib does not
expose.

---

## Conventions

Standard spice layout:

```
spices/plot/
  build.tur
  src/plot/
    core.tur        -- "plot/core"    plot function, output, coord transform
    style.tur       -- "plot/style"   color/line/point style types and defaults
    tick.tur        -- "plot/tick"    tick generation and formatting
    decor.tur       -- "plot/decor"   axes, grid, labels, legend
    line.tur        -- "plot/line"    function, lines, parametric, polar, inverse, density
    point.tur       -- "plot/point"   points, error-bars, vector-field, arrows
    interval.tur    -- "plot/interval" function-interval, lines-interval, parametric-interval
    area.tur        -- "plot/area"    rectangles, histograms
    contour.tur     -- "plot/contour" contours, isoline, contour-intervals, color-field
  tests/plot/
    core_test.tur
    line_test.tur
    point_test.tur
    interval_test.tur
    area_test.tur
    contour_test.tur
```

---

## Architecture

```
caller
  |
  v
plot/core      -- collects renderers, computes unified bounds, sets up viewport
  |
  +-- plot/tick    -- compute tick positions and label strings
  +-- plot/decor   -- draw axes, grid lines, legend, title, axis labels
  +-- renderer.render(canvas bounds)   -- each renderer draws itself
        |
        v
      tur-plutovg  -- canvas-*, path-*, paint-*, font-*, surface-*
        |
        v
      plutovg surface  --> PNG file or returned as :int handle
```

Renderers are pure data values (structs). `plot` iterates the renderer list twice:
once to union bounds, once to draw. Each renderer carries a `render` function pointer
(`:int` function value) called with `(canvas bounds style-ctx)`.

---

## Style types

All style structs live in `plot/style`. Every field that accepts `0` as a sentinel
falls back to the context default.

```turmeric
;;; color-idx -- color index mapped to a built-in palette (0-7, wraps).
;;;   0=blue 1=red 2=green 3=orange 4=purple 5=brown 6=pink 7=grey
;;; Pass -1 for "auto" (next color in palette rotation).

(defstruct line-style
  color  :int    ;; color-idx, -1 = auto
  width  :float  ;; pixels, 0.0 = default (1.5)
  dash   :int    ;; 0=solid 1=dash 2=dot 3=dash-dot
  alpha  :float) ;; 0.0 = default (1.0)

(defstruct point-style
  color       :int    ;; outline color-idx, -1 = auto
  fill-color  :int    ;; fill color-idx, -1 = auto (= color)
  sym         :int    ;; 0=circle 1=square 2=triangle 3=cross 4=plus 5=diamond
  size        :float  ;; diameter in pixels, 0.0 = default (6.0)
  alpha       :float)

(defstruct fill-style
  color        :int    ;; fill color-idx, -1 = auto
  alpha        :float  ;; 0.0 = default (0.5)
  line-color   :int    ;; border color-idx, -1 = auto (= color, darker)
  line-width   :float) ;; border width, 0.0 = default (1.0)

(defstruct plot-opts
  width       :int    ;; output pixels, 0 = default (600)
  height      :int    ;; output pixels, 0 = default (450)
  title       :cstr   ;; 0 (nil ptr) = no title
  x-label     :cstr
  y-label     :cstr
  x-min       :float  ;; NaN = auto
  x-max       :float
  y-min       :float
  y-max       :float
  legend-pos  :int)   ;; 0=none 1=top-right 2=top-left 3=bottom-right 4=bottom-left
```

Convenience constructor:

```turmeric
(default-plot-opts)    ;; => plot-opts with all defaults filled in
```

---

## Modules and exports

### plot/style

```turmeric
;; Default style constructors (all fields at their defaults).
(default-line-style)          ;; => line-style
(default-point-style)         ;; => point-style
(default-fill-style)          ;; => fill-style

;; Color palette access.
(palette-color idx)            ;; => (cons r (cons g (cons b 0)))  each 0-255
(palette-size)                 ;; => :int  (8)

;; Color interpolation -- returns a cons list of n color-idx values between
;; start and end (linearly in RGB).
(color-seq start-idx end-idx n)     ;; => list<:int>

;; Named line-dash constants.
(dash-solid)     ;; => :int  0
(dash-dash)      ;; => :int  1
(dash-dot)       ;; => :int  2
(dash-dash-dot)  ;; => :int  3

;; Named point-sym constants.
(sym-circle)    ;; => :int  0
(sym-square)    ;; => :int  1
(sym-triangle)  ;; => :int  2
(sym-cross)     ;; => :int  3
(sym-plus)      ;; => :int  4
(sym-diamond)   ;; => :int  5
```

---

### plot/core

```turmeric
;; Render a list of renderers to a new plutovg surface.
;; renderers -- cons list of renderer :int values
;; opts      -- plot-opts struct
;; Returns a tur-plutovg surface handle (caller owns it; call surface-destroy).
(plot renderers opts)              ;; => result<surface :int>

;; Convenience: render and write directly to a PNG file.
(plot-write-png renderers opts path)    ;; => result<:void>

;; Low-level: render into an existing plutovg canvas with an explicit viewport
;; (pixel rect: x y w h) and data bounds.
(plot-into-canvas canvas renderers opts px py pw ph)
                                        ;; => result<:void>
```

Internal helpers (not exported, documented with single-line `;;;`):

```turmeric
;;; bounds-union -- union two axis-bounds structs, ignoring NaN sentinels.
;;; viewport->data -- map pixel (px py) to data (x y) given bounds and viewport.
;;; data->viewport -- map data (x y) to pixel (px py).
;;; auto-bounds -- compute tight data bounds from a list of renderer bound hints.
;;; draw-background -- fill plot area with background color and draw border.
```

---

### plot/tick

```turmeric
;; Compute a list of tick positions (as :float) within [lo, hi].
;; Returns a cons list of :float values.
(linear-ticks lo hi count)       ;; => list<:float>   (nice round values)
(log-ticks lo hi)                ;; => list<:float>   (powers of 10)

;; Format a tick value as a label string.
(format-tick v precision)        ;; => :cstr

;; Compute tick positions and formatted labels together.
;; Returns a cons list of (cons :float :cstr) pairs.
(linear-tick-labels lo hi count)  ;; => list<(cons :float :cstr)>
(log-tick-labels lo hi)           ;; => list<(cons :float :cstr)>
```

---

### plot/decor

These are renderer values (same interface as data renderers) and can be mixed
freely in the renderer list passed to `plot`.

```turmeric
;; Axis lines through y=0 and x=0.
(x-axis)                          ;; => renderer :int
(y-axis)                          ;; => renderer :int
(axes)                            ;; => renderer :int  (both)

;; Faint vertical lines at x-axis ticks, horizontal at y-axis ticks.
(x-tick-lines)                    ;; => renderer :int
(y-tick-lines)                    ;; => renderer :int
(tick-grid)                       ;; => renderer :int  (both)

;; Horizontal / vertical reference lines at a fixed data value.
(hrule y)                         ;; => renderer :int
(hrule-styled y style)            ;; => renderer :int  (style = line-style)
(vrule x)                         ;; => renderer :int
(vrule-styled x style)            ;; => renderer :int

;; Text label anchored to a data point.
;; anchor: 0=center 1=top 2=bottom 3=left 4=right 5=top-left etc. (8 positions)
(point-label x y text anchor)     ;; => renderer :int
(point-label-styled x y text anchor style)
                                  ;; => renderer :int  (style = line-style for color/size)
```

---

### plot/line

All line renderers accept a `line-style` as their last argument. Pass
`(default-line-style)` to use defaults.

```turmeric
;; Sample f over [x-min, x-max] with `samples` points and draw as a polyline.
;; f signature: (fn [x :float] :float)
(function f x-min x-max samples style label)
                                  ;; => renderer :int

;; Draw a polyline through a cons list of (cons :float :float) pairs.
(lines vs style label)            ;; => renderer :int

;; Parametric curve: f maps t -> (cons x y), sampled over [t-min, t-max].
(parametric f t-min t-max samples style label)
                                  ;; => renderer :int

;; Polar curve: f maps angle -> radius, sampled over [theta-min, theta-max].
(polar f theta-min theta-max samples style label)
                                  ;; => renderer :int

;; Inverse: f maps y -> x, sampled over [y-min, y-max].
(inverse f y-min y-max samples style label)
                                  ;; => renderer :int

;; Kernel density estimation from a cons list of :float values.
;; bandwidth <= 0.0 uses Silverman's rule.
(density vs bandwidth samples style label)
                                  ;; => renderer :int
```

---

### plot/point

```turmeric
;; Scatter plot.  vs = cons list of (cons :float :float) pairs.
(points vs style label)           ;; => renderer :int

;; Error bars.  bars = cons list of (x y half-height) as nested cons triples.
;; invert != 0 draws horizontal bars (x half-width).
(error-bars bars style invert label)
                                  ;; => renderer :int

;; Vector field.  f maps (x y) -> (cons dx dy), sampled on a samples x samples grid.
;; scale-mode: 0=auto (no overlap), 1=normalized (equal length), 2=raw.
(vector-field f x-min x-max y-min y-max samples scale-mode style label)
                                  ;; => renderer :int

;; Arrow segments.  arrows = cons list of (x1 y1 x2 y2) as nested cons.
(arrows arrow-list style label)   ;; => renderer :int
```

---

### plot/interval

Fill-style controls the shaded region; line-style controls the boundary lines.

```turmeric
;; Shade between f1(x) and f2(x).
(function-interval f1 f2 x-min x-max samples fill style label)
                                  ;; => renderer :int

;; Shade between two polylines (paired point sequences, same x values).
(lines-interval vs1 vs2 fill style label)
                                  ;; => renderer :int

;; Shade between two parametric curves over the same t range.
(parametric-interval f1 f2 t-min t-max samples fill style label)
                                  ;; => renderer :int

;; Shade between two polar curves over the same theta range.
(polar-interval f1 f2 theta-min theta-max samples fill style label)
                                  ;; => renderer :int
```

---

### plot/area

```turmeric
;; Draw filled rectangles.
;; rects = cons list of (x-lo x-hi y-lo y-hi) as nested cons.
(rectangles rects fill label)     ;; => renderer :int

;; Bar chart from pre-bucketed data.
;; bars = cons list of (x-lo x-hi height) as nested cons.
;; fill-styles = cons list of fill-style (one per bar; cycles if shorter).
(area-histogram bars fill-styles label)
                                  ;; => renderer :int

;; Categorical bar chart.
;; bars = cons list of (label-cstr height) pairs.
;; invert != 0 draws horizontal bars.
(discrete-histogram bars fill invert label)
                                  ;; => renderer :int

;; Stacked categorical bars.
;; groups = cons list of (label-cstr . cons list of :float heights).
;; fills = cons list of fill-style, one per stack segment.
;; labels = cons list of :cstr legend labels, one per segment.
(stacked-histogram groups fills labels invert)
                                  ;; => renderer :int
```

---

### plot/contour

```turmeric
;; Single isoline: draw the level set f(x,y)=level using marching squares.
;; f signature: (fn [x :float y :float] :float)
(isoline f x-min x-max y-min y-max samples level style label)
                                  ;; => renderer :int

;; Multiple isolines at automatically chosen levels (n evenly spaced levels).
;; styles = cons list of line-style (cycles); pass nil for uniform style.
(contours f x-min x-max y-min y-max samples n styles label)
                                  ;; => renderer :int

;; Filled regions between contour levels (n levels => n+1 bands).
;; fills = cons list of fill-style (cycles).
(contour-intervals f x-min x-max y-min y-max samples n fills label)
                                  ;; => renderer :int

;; Color each pixel by f(x,y) -> color-idx.  No legend entry.
;; f signature: (fn [x :float y :float] :int)
(color-field f x-min x-max y-min y-max samples alpha)
                                  ;; => renderer :int
```

---

## Implementation phases

- [ ] **PL0** -- `build.tur`; spice dependency on `tur-plutovg`; `plot/style` structs
  and defaults; `plot/tick` (`linear-ticks`, `format-tick`, `linear-tick-labels`);
  `plot/core` coordinate transform math (`data->viewport`, `viewport->data`,
  `auto-bounds`); `plot` and `plot-write-png` rendering a blank frame with axes,
  title, and tick labels (no data renderers yet).

- [ ] **PL1** -- `plot/decor`: `axes`, `x-axis`, `y-axis`, `tick-grid`, `hrule`,
  `vrule`, `point-label`; legend rendering in `plot/core` (collect labels from
  renderer list, draw swatch + text in chosen corner).

- [ ] **PL2** -- `plot/line`: `function`, `lines`; adaptive sampling that inserts
  extra points where the slope changes quickly; NaN gap handling in `lines`
  (skip segment when either endpoint is NaN).

- [ ] **PL3** -- `plot/point`: `points` (all six `sym` shapes); `error-bars`
  (vertical and horizontal); round-trip test: `points` + `error-bars` + `function`
  on one plot, write PNG.

- [ ] **PL4** -- `plot/line` remainder: `parametric`, `polar`, `inverse`;
  `color-seq` in `plot/style`; multi-renderer color cycling.

- [ ] **PL5** -- `plot/interval`: `function-interval`, `lines-interval`,
  `parametric-interval`, `polar-interval`; filled region clipping to plot bounds.

- [ ] **PL6** -- `plot/area`: `rectangles`, `area-histogram`, `discrete-histogram`,
  `stacked-histogram`; categorical x-axis labeling (tick labels from bar names
  instead of numbers).

- [ ] **PL7** -- `plot/contour`: marching-squares isoline extraction helper (shared
  by `isoline`, `contours`, `contour-intervals`); `isoline`, `contours`,
  `contour-intervals`; `color-field`; `log-ticks` / `log-tick-labels` in
  `plot/tick`.

- [ ] **PL8** -- `plot/line`: `density` (Silverman bandwidth, Gaussian kernel);
  `plot/point`: `vector-field`, `arrows`; `plot/decor`: `point-label-styled`,
  `hrule-styled`, `vrule-styled`.

- [ ] **PL9** -- Tests (each renderer type produces a non-empty PNG; pixel-level
  sanity checks via `surface-data`); README section in `turmeric-spices`;
  `plot-v0.1.0` tag.

---

## Design notes

### Renderer interface

Every renderer is a Turmeric struct carrying:
- **bound hints**: `(x-min x-max y-min y-max)` as `:float` (NaN = unconstrained)
- **label**: `:cstr` or nil (appears in legend)
- **render fn**: a function value `(fn [canvas :int bx-min :float bx-max :float by-min :float by-max :float pw :int ph :int] :void)`

`plot` calls `auto-bounds` to union all hint rects (honoring any overrides in
`plot-opts`), then calls each renderer's render fn with the resolved bounds and
pixel dimensions. Renderers call `data->viewport` to map their coordinates to
canvas pixels before issuing plutovg draw calls.

### Sampling

`function`, `parametric`, `polar`, and `inverse` use uniform sampling by default.
The sampler in `plot/core` also supports a simple adaptive pass: if the angle
between consecutive segments exceeds a threshold (~5 deg), it bisects the interval
up to a fixed recursion depth (4). This keeps smooth curves accurate without
requiring very high sample counts.

### Marching squares

`plot/contour` implements a basic marching squares algorithm for isoline
extraction. The grid is sampled once and the scalar field stored as a flat
`:cstr`-backed float array (allocated via inline-C `malloc`). Isoline segments
are collected into a cons list and then drawn as polylines via `canvas-add-path`.

### Categorical x-axis

When `discrete-histogram` or `stacked-histogram` is in the renderer list, `plot`
detects the categorical mode from a flag on the renderer struct and replaces the
numeric x-tick labels with the bar-name strings at positions 0.5, 1.5, 2.5, ...

### Color cycling

`plot` maintains a mutable color counter (`:int` ref via inline-C) that increments
each time a renderer requests color index -1 (auto). The counter resets at the
start of each `plot` call. This reproduces Racket's automatic color rotation across
multiple renderers.

---

## Shared work

### turmeric-spices README

Add row once v0.1.0 is tagged:

| Spice | Description | Tier | Depends on |
|-------|-------------|------|------------|
| `tur-plot` | 2D data visualization (functions, points, histograms, contours) | 1 -- pure Turmeric | `tur-plutovg` |

### Guide

Deliver `docs/guides/plot-guide.md` alongside the `v0.1.0` tag. Sections:

1. Basic function plot (`function`, `axes`, `plot-write-png`)
2. Scatter plot with error bars (`points`, `error-bars`)
3. Shaded interval (`function-interval`)
4. Histogram (`discrete-histogram`, `stacked-histogram`)
5. Contour plot (`contours`, `contour-intervals`)
6. Combining multiple renderers and legend

### Integration notes

- Pair with `tur-plutovg` directly when you need fine-grained control over the
  canvas after plotting (overlays, annotations, custom shapes).
- Pair with `tur-png` to read pixel data back from the surface for further
  processing or embedding in a larger image pipeline.
- `tur-raylib` users can blit the plot surface as a raylib texture for an in-window
  plot widget; `plot-into-canvas` lets you render into a sub-region of an existing
  canvas to compose multiple plots in one image.
