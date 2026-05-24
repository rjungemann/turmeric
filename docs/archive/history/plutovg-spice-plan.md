# Spice Plan: tur-plutovg

> **Status:** Complete -- shipped as `plutovg-v0.1.0` in `turmeric-spices`
> **Last Updated:** 2026-05-24
> **Type:** Spice Design

---

## Overview

One new Tier-2 spice (cmake C dependency) for the `turmeric-spices` monorepo:

| Spice | Tag | C dep | Purpose |
|-------|-----|-------|---------|
| `tur-plutovg` | `plutovg-v0.1.0` | plutovg 1.3 | 2D vector graphics rendering |

PlutoVG is a lightweight standalone C library for 2D vector graphics: paths, fills,
strokes, gradients, text, and image compositing. It is the rendering engine behind
LunaSVG and PlutoSVG, and a natural companion to `tur-png` for off-screen rasterization.

---

## Conventions

Follows the same layout used by `tur-sqlite`, `tur-png`, etc.:

```
spices/plutovg/
  build.tur               -- defpackage manifest
  src/plutovg/
    surface.tur           -- module "plutovg/surface"
    canvas.tur            -- module "plutovg/canvas"
    path.tur              -- module "plutovg/path"
    paint.tur             -- module "plutovg/paint"
    font.tur              -- module "plutovg/font"
  tests/plutovg/
    surface_test.tur
    canvas_test.tur
    path_test.tur
    paint_test.tur
    font_test.tur
```

`build.tur` lists `:cmake-deps` with a pinned `:ref`. Inline-C blocks in the source
files call directly into the plutovg C API. All opaque C pointers are represented as
`:int` (pointer-sized integer) handles in Turmeric.

---

## C dependency

| Field | Value |
|-------|-------|
| Library | plutovg |
| URL | `https://github.com/sammycage/plutovg` |
| Pinned ref | `v1.3.3` |
| Header | `#include <plutovg.h>` |
| cmake option | `PLUTOVG_BUILD_EXAMPLES OFF` |
| Link | `plutovg` |
| License | MIT |
| C standard | C11 |
| Extra deps | libm (linked automatically on most platforms), platform threads |

---

## Modules and exports

```
plutovg/surface   -- pixel buffer creation, export (PNG/JPEG), raw data access
plutovg/canvas    -- drawing context: state stack, path recording, fill/stroke/clip
plutovg/path      -- standalone path object: construction, shapes, transforms
plutovg/paint     -- solid colors, linear/radial gradients, texture paints
plutovg/font      -- font loading, cache, metrics, text extent queries
```

---

## API sketch

### plutovg/surface

```turmeric
;; Pixel buffer backed by premultiplied ARGB data.
(surface-create width height)          ;; => result<surface :int>
(surface-create-from-data data width height stride)
                                       ;; => result<surface :int>  (wraps existing pixel data)
(surface-destroy s)
(surface-width s)                      ;; => :int
(surface-height s)                     ;; => :int
(surface-stride s)                     ;; => :int  (bytes per row)
(surface-data s)                       ;; => :cstr  (raw ARGB bytes, premultiplied)

(surface-write-png s "out.png")        ;; => result<:void>
(surface-write-jpeg s "out.jpg" quality)
                                       ;; => result<:void>  (quality 0-100)
```

### plutovg/canvas

```turmeric
;; Drawing context.  All state is pushed/popped with save/restore.
(canvas-create surface)                ;; => result<canvas :int>
(canvas-destroy c)
(canvas-save c)                        ;; push state
(canvas-restore c)                     ;; pop state

;; Transformation
(canvas-translate c tx ty)
(canvas-scale c sx sy)
(canvas-rotate c radians)
(canvas-transform c a b cc d e f)     ;; multiply by raw 6-element matrix
(canvas-reset-transform c)

;; Path recording on the canvas (shares grammar with plutovg/path)
(canvas-move-to c x y)
(canvas-line-to c x y)
(canvas-quad-to c x1 y1 x y)
(canvas-cubic-to c x1 y1 x2 y2 x y)
(canvas-arc-to c rx ry rotation large-arc sweep x y)
(canvas-close-path c)
(canvas-add-path c path)              ;; append a plutovg/path object

;; Shape helpers (move + path + close in one call)
(canvas-rect c x y w h)
(canvas-round-rect c x y w h rx ry)
(canvas-ellipse c cx cy rx ry)
(canvas-circle c cx cy r)
(canvas-arc c cx cy r start-angle sweep-angle)

;; Rendering
(canvas-fill c)                        ;; fill current path, clear path
(canvas-stroke c)                      ;; stroke current path, clear path
(canvas-clip c)                        ;; clip to current path, clear path
(canvas-fill-preserve c)               ;; fill but keep path
(canvas-stroke-preserve c)
(canvas-clip-preserve c)

;; Draw a pre-built path directly
(canvas-fill-path c path)
(canvas-stroke-path c path)
(canvas-clip-path c path)

;; Draw image (surface) into canvas
(canvas-draw-image c src-surface x y w h)

;; Stroke properties
(canvas-set-line-width c w)
(canvas-set-line-cap c cap)            ;; ":butt" ":round" ":square"
(canvas-set-line-join c join)          ;; ":miter" ":bevel" ":round"
(canvas-set-miter-limit c limit)
(canvas-set-dash-offset c offset)
(canvas-set-dash-array c dashes)       ;; cons list of :float lengths

;; Fill rule
(canvas-set-fill-rule c rule)          ;; ":non-zero" ":even-odd"

;; Paint (applies to fill, stroke, or both)
(canvas-set-source c paint)            ;; set fill and stroke paint
(canvas-set-source-color c r g b a)    ;; shorthand solid color (0.0-1.0)

;; Blend mode and opacity
(canvas-set-operator c op)             ;; ":src-over" ":src" ":dst-over" ":dst-in" ":xor" etc.
(canvas-set-opacity c alpha)           ;; 0.0-1.0, global opacity

;; Text (requires font set via canvas-set-font-face)
(canvas-set-font-face c font-face)
(canvas-set-font-size c size)
(canvas-show-text c text encoding x y)  ;; encoding: ":utf8" ":utf16" ":utf32" ":latin1"
(canvas-text-extents c text encoding)   ;; => result<rect :int>
```

### plutovg/path

```turmeric
;; Standalone path object for pre-built and reusable paths.
(path-create)                          ;; => result<path :int>
(path-destroy p)
(path-move-to p x y)
(path-line-to p x y)
(path-quad-to p x1 y1 x y)
(path-cubic-to p x1 y1 x2 y2 x y)
(path-arc-to p rx ry rotation large-arc sweep x y)
(path-close p)

;; Shape helpers
(path-add-rect p x y w h)
(path-add-round-rect p x y w h rx ry)
(path-add-ellipse p cx cy rx ry)
(path-add-circle p cx cy r)
(path-add-arc p cx cy r start-angle sweep-angle)

;; Utilities
(path-clone p)                         ;; => result<path :int>
(path-clone-flatten p tolerance)       ;; => result<path :int>  (all curves -> lines)
(path-clone-dashed p offset dashes)    ;; => result<path :int>  (dashes = cons list)
(path-extents p)                       ;; => result<rect :int>  (bounding box)
(path-length p)                        ;; => :float

;; Parse SVG path data ("M 10 20 L 30 40 Z")
(path-parse p svg-str)                 ;; => result<:void>
```

### plutovg/paint

```turmeric
;; Solid color paint.
(paint-create-color r g b a)           ;; => result<paint :int>  (components 0.0-1.0)
(paint-create-color-hex hex-str)       ;; => result<paint :int>  ("#rrggbbaa" or "#rrggbb")

;; Linear gradient: from (x1,y1) to (x2,y2).
(paint-create-linear-gradient x1 y1 x2 y2)
                                       ;; => result<paint :int>
;; Radial gradient: center (cx,cy), focus (fx,fy), radius r.
(paint-create-radial-gradient cx cy fx fy r)
                                       ;; => result<paint :int>

;; Gradient stop  (offset 0.0-1.0, color components 0.0-1.0)
(paint-add-color-stop paint offset r g b a)

;; Gradient spread method
(paint-set-spread-method paint method) ;; ":pad" ":reflect" ":repeat"

;; Gradient coordinate transform
(paint-set-matrix paint a b cc d e f)

;; Texture (surface-backed) paint
(paint-create-texture surface)         ;; => result<paint :int>

(paint-destroy paint)
```

### plutovg/font

```turmeric
;; Load a font face from a file or in-memory data.
(font-face-load-from-file path index)  ;; => result<font-face :int>
(font-face-load-from-data data len index)
                                       ;; => result<font-face :int>
(font-face-destroy ff)

;; Per-glyph metrics (values in font units; scale by font-size / units-per-em)
(font-face-units-per-em ff)            ;; => :float
(font-face-ascent ff size)             ;; => :float
(font-face-descent ff size)            ;; => :float
(font-face-line-gap ff size)           ;; => :float

;; Text extents at a given font size (UTF-8 string)
(font-face-text-extents ff size text)  ;; => result<rect :int>

;; Font face cache -- load and reuse faces by family name
(font-cache-create)                    ;; => result<font-cache :int>
(font-cache-destroy fc)
(font-cache-add-file fc path)          ;; => result<:void>
(font-cache-load-system fc)            ;; => result<:void>  (platform font directories)
(font-cache-get fc family style size)  ;; => result<font-face :int>
                                       ;;   style: ":regular" ":bold" ":italic" ":bold-italic"
```

---

## Implementation phases

- [x] **PV0** -- `build.tur`; CPM plutovg at `v1.3.3`; `surface-create`, `surface-destroy`,
  `surface-width`, `surface-height`, `surface-stride`, `surface-data`;
  `canvas-create`, `canvas-destroy`; solid-color fill of a rect (smoke test).

- [x] **PV1** -- Canvas path recording: `canvas-move-to`, `canvas-line-to`, `canvas-quad-to`,
  `canvas-cubic-to`, `canvas-close-path`; shape helpers (`canvas-rect`,
  `canvas-ellipse`, `canvas-circle`); `canvas-fill`, `canvas-stroke`, `canvas-clip`.

- [x] **PV2** -- Standalone path object (`plutovg/path`): full construction API,
  `path-clone`, `path-extents`, `path-length`, `path-clone-flatten`, `path-parse`;
  `canvas-add-path`, `canvas-fill-path`, `canvas-stroke-path`.

- [x] **PV3** -- Paint module (`plutovg/paint`): `paint-create-color`, `paint-create-color-hex`,
  linear/radial gradients, `paint-add-color-stop`, `paint-set-spread-method`,
  texture paints; `canvas-set-source`, `canvas-set-source-color`.

- [x] **PV4** -- Canvas state and transforms: `canvas-save`, `canvas-restore`,
  `canvas-translate`, `canvas-scale`, `canvas-rotate`, `canvas-transform`,
  `canvas-reset-transform`; stroke properties (`canvas-set-line-width/cap/join/miter-limit`);
  dash (`canvas-set-dash-offset`, `canvas-set-dash-array`); `canvas-set-fill-rule`;
  `canvas-set-operator`; `canvas-set-opacity`.

- [x] **PV5** -- Font module (`plutovg/font`): `font-face-load-from-file`,
  `font-face-load-from-data`, metrics, `font-face-text-extents`;
  `canvas-set-font-face`, `canvas-set-font-size`, `canvas-show-text`;
  font cache (`font-cache-create`, `font-cache-add-file`, `font-cache-load-system`,
  `font-cache-get`).

- [x] **PV6** -- Surface export: `surface-write-png`, `surface-write-jpeg`;
  `canvas-draw-image`; arc helpers (`canvas-arc`, `canvas-round-rect`,
  `canvas-arc-to`, `path-clone-dashed`).

- [x] **PV7** -- Tests (off-screen render, compare pixel output);
  README section in `turmeric-spices`; `plutovg-v0.1.0` tag.

---

## Shared work

### turmeric-spices README

Add a row once v0.1.0 is tagged:

| Spice | Description | Tier | C dep |
|-------|-------------|------|-------|
| `tur-plutovg` | 2D vector graphics rendering via plutovg | 2 -- cmake-dep | plutovg 1.3 |

### Guide

Deliver `docs/guides/plutovg-guide.md` alongside the `v0.1.0` tag. It should cover:

1. Creating a surface and canvas
2. Drawing basic shapes with solid and gradient fills
3. Loading a font and rendering text
4. Exporting the result to PNG
5. Integrating with `tur-png` for pixel-level manipulation after rasterization

### Integration notes

- `tur-png` is a natural complement: use `tur-plutovg` to rasterize a scene, then
  pass `surface-data` to `tur-png`'s `png-write-raw` if fine-grained pixel access
  is needed.
- `tur-raylib` users may prefer raylib's built-in drawing API for real-time rendering;
  `tur-plutovg` targets off-screen / static image generation use cases.
