# libfive Solid-Modeling DSL for Turmeric

> **Status:** Draft Plan
> **Last Updated:** 2026-05-19
> **Type:** DSL Design + Tutorial + Example Project

---

## Overview

Design a domain-specific language (DSL) embedded in Turmeric for constructive solid geometry (CSG) using [libfive](https://libfive.com/) as the rendering backend. The DSL mirrors the libfive Scheme API -- shape constructors, boolean set operations, spatial transforms, blending -- but integrates naturally with Turmeric's syntax, type system, and macro facilities.

The DSL compiles a tree of shape descriptors into libfive's f-rep (functional representation) evaluator via Turmeric's C FFI, then exports geometry as STL mesh files or renders live previews through the libfive Studio GUI.

---

## Motivation

- **Functional geometry**: F-rep modeling fits naturally in a functional language; shapes are first-class values composed with ordinary functions
- **Metaprogramming**: Turmeric macros can generate shape families, parametric parts, and repetition patterns that would be tedious by hand in Scheme
- **Type safety**: Encode shape invariants (e.g. non-negative radius) via Turmeric contracts
- **Toolchain integration**: Drive export pipelines (STL -> slicer -> printer) from the same script that defines the geometry
- **Familiar syntax**: Turmeric developers get solid modeling without learning Scheme or a separate GUI tool

---

## Background: libfive f-reps

libfive represents shapes as **signed distance fields**: a function `f(x, y, z) -> float` where `f < 0` is inside the shape, `f = 0` is the surface, and `f > 0` is outside. CSG operations on f-reps are simple arithmetic:

| Operation | f-rep formula |
|-----------|---------------|
| Union | `min(f_a, f_b)` |
| Intersection | `max(f_a, f_b)` |
| Difference | `max(f_a, -f_b)` |
| Offset | `f(x,y,z) - r` |
| Shell | `abs(f) - thickness` |
| Smooth blend | `f_a + f_b - sqrt(f_a^2 + f_b^2) + m` |

This means shapes compose without approximation -- no polygon soup, no seams.

The libfive **Scheme DSL** already exposes this API at a high level. The Turmeric DSL targets the same semantic layer, translating to libfive C API calls instead of running through Guile.

---

## DSL Design

### Core Principles

1. **Shape = value**: every constructor returns an opaque `:shape` handle (a pointer to a `libfive_tree`)
2. **Pure construction**: no mutation -- transforms and booleans return new shape values
3. **1:1 Scheme mapping**: each Scheme primitive has a direct Turmeric counterpart so the libfive docs remain useful
4. **Macro extension**: Turmeric macros provide loops, patterns, and parametric families on top of the primitives
5. **C FFI backend**: the runtime calls libfive C API functions via `extern-c`

### Type Model

| Turmeric alias | Underlying type | Description |
|----------------|-----------------|-------------|
| `:shape` | `:int` (opaque handle) | A libfive tree / f-rep node |
| `:vec2` | `[float float]` | 2-D point or size |
| `:vec3` | `[float float float]` | 3-D point, size, or axis |
| `:bounds` | `[vec3 vec3]` | Axis-aligned bounding box `[lo hi]` |
| `:resolution` | `:float` | Mesh resolution in voxels-per-unit |

In practice `:shape` is represented as an `:int` (the C pointer cast to integer) since Turmeric's FFI uses integer-width slots.

---

## Shape Constructors

### Primitives

```turmeric
;; Sphere of radius r centered at the origin
(lf-sphere 1.5)

;; Sphere of radius r centered at [cx cy cz]
(lf-sphere 1.5 [0.0 0.0 2.0])

;; Axis-aligned box from lo to hi
(lf-box [-1.0 -1.0 -1.0] [1.0 1.0 1.0])

;; Box with exact distance field (no sharp-edge mitering)
(lf-box-exact [-1.0 -1.0 0.0] [1.0 1.0 2.0])

;; Cylinder of radius r, height h, base center at origin
(lf-cylinder 0.5 3.0)

;; Cylinder with explicit base center
(lf-cylinder 0.5 3.0 [0.0 0.0 0.0])

;; Cone: radius r at base, tapering to a point, height h
(lf-cone 0.8 2.0)

;; Torus: major radius R, minor radius r
(lf-torus 2.0 0.5)

;; Half-space: all points below z = level
(lf-half-space-z 0.0)

;; Infinite extrusion of a 2-D circle along Z
(lf-circle 1.0)

;; Rectangular prism (box alias with width/depth/height)
(lf-rect [-1.0 -1.0] [1.0 1.0])   ; 2-D rectangle in XY

;; Rounded box (box offset inward then out by radius r)
(lf-rounded-box [-1.0 -1.0 -1.0] [1.0 1.0 1.0] 0.2)
```

### Boolean Operations

```turmeric
;; Union -- smallest enclosing shape
(lf-union a b)
(lf-union a b c d)   ; variadic via macro

;; Intersection -- volume inside all shapes
(lf-intersection a b)
(lf-intersection a b c)

;; Difference -- subtract b (and more) from a
(lf-difference a b)
(lf-difference a b c)   ; removes b and c from a

;; Symmetric difference (XOR)
(lf-xor a b)
```

### Spatial Transforms

All transforms are pure -- they return a new `:shape`.

```turmeric
;; Translate by [dx dy dz]
(lf-move a [1.0 0.0 0.0])

;; Uniform scale from origin
(lf-scale a 2.0)

;; Non-uniform scale [sx sy sz]
(lf-scale-xyz a [1.0 2.0 0.5])

;; Scale about an arbitrary center point
(lf-scale-xyz-about a [1.0 1.0 1.0] [0.0 0.0 0.0])

;; Rotate about Z axis by angle (radians), around origin
(lf-rotate-z a 0.785)   ; 45 degrees = pi/4

;; Rotate about Z axis around center [cx cy cz]
(lf-rotate-z a 0.785 [0.0 0.0 1.0])

;; Rotate about X or Y axis
(lf-rotate-x a 1.571)   ; 90 degrees
(lf-rotate-y a 1.571)

;; Reflect across an axis plane
(lf-reflect-x a)    ; mirror in YZ plane
(lf-reflect-y a)
(lf-reflect-z a)

;; Reflect with explicit plane offset
(lf-reflect-x a 1.0)   ; mirror across x = 1.0

;; Symmetric copy -- reflect and union with original
(lf-symmetric-x a)
(lf-symmetric-y a)
(lf-symmetric-z a)
```

### Blending and Offsets

```turmeric
;; Offset: expand shape outward by amount (shrink if negative)
(lf-offset a 0.1)

;; Shell: keep only a surface layer of thickness t
(lf-shell a 0.05)

;; Smooth union (m controls blend radius)
(lf-blend-union a b 0.3)

;; Smooth intersection
(lf-blend-intersection a b 0.3)

;; Smooth difference
(lf-blend-difference a b 0.3)

;; Morphological erosion/dilation
(lf-erode  a 0.1)
(lf-dilate a 0.1)
```

### 2-D to 3-D Operations

```turmeric
;; Extrude a 2-D shape along Z from z-lo to z-hi
(lf-extrude-z shape2d 0.0 5.0)

;; Revolve a 2-D XZ cross-section around Z axis
(lf-revolve-z shape2d)

;; Revolve with a cylindrical offset from axis
(lf-revolve-z shape2d 1.0)
```

### Repetition

```turmeric
;; Tile a shape with period [px py pz] (0 = no repeat on that axis)
(lf-array a [3.0 3.0 0.0])

;; Polar repetition: n copies around Z axis
(lf-array-polar a 6)
```

---

## Output and Rendering

```turmeric
;; Render to STL file within bounds at given resolution
(lf-save-stl shape "output.stl" [[-5.0 -5.0 -5.0] [5.0 5.0 5.0]] 10.0)

;; Render 2-D shape to SVG
(lf-save-svg shape2d "output.svg" [[-5.0 -5.0] [5.0 5.0]] 10.0)

;; Evaluate the shape's distance at a point (useful for debugging)
(lf-eval shape [0.0 0.0 0.0])   ; returns :float

;; Launch live preview in libfive Studio (blocks until closed)
(lf-show shape [[-5.0 -5.0 -5.0] [5.0 5.0 5.0]])
```

---

## FFI Backend

The DSL wraps the libfive C API (`libfive.h`). Core bindings:

```turmeric
;;; lf-sphere -- create a sphere f-rep node.
(extern-c libfive_tree_sphere [r :float cx :float cy :float cz :float] :int)

;;; lf-box-mitered -- create an axis-aligned box with mitered edges.
(extern-c libfive_tree_box_mitered
  [xmin :float ymin :float zmin :float
   xmax :float ymax :float zmax :float] :int)

;;; lf-cylinder -- create a cylinder along Z.
(extern-c libfive_tree_cylinder_z
  [r :float height :float bx :float by :float bz :float] :int)

;;; lf-union -- CSG union of two trees.
(extern-c libfive_tree_union [a :int b :int] :int)

;;; lf-intersection -- CSG intersection.
(extern-c libfive_tree_intersection [a :int b :int] :int)

;;; lf-difference -- CSG difference a - b.
(extern-c libfive_tree_difference [a :int b :int] :int)

;;; lf-move -- translate a shape.
(extern-c libfive_tree_move [shape :int dx :float dy :float dz :float] :int)

;;; lf-save-stl -- mesh and export to STL.
(extern-c libfive_tree_save_mesh
  [shape :int
   xmin :float ymin :float zmin :float
   xmax :float ymax :float zmax :float
   res :float filename :cstr] :int)

;;; lf-free-tree -- release a libfive tree node.
(extern-c libfive_tree_free [shape :int] :void)
```

Thin Turmeric wrappers unpack `:vec3` tuples and delegate to the `extern-c` bindings:

```turmeric
(defn lf-sphere [r :float] :int
  (libfive_tree_sphere r 0.0 0.0 0.0))

(defn lf-sphere-at [r :float center :vec3] :int
  (let [[cx cy cz] center]
    (libfive_tree_sphere r cx cy cz)))

(defn lf-move [shape :int delta :vec3] :int
  (let [[dx dy dz] delta]
    (libfive_tree_move shape dx dy dz)))

(defn lf-save-stl [shape :int path :cstr bounds :bounds res :float] :void
  (let [[[x0 y0 z0] [x1 y1 z1]] bounds]
    (libfive_tree_save_mesh shape x0 y0 z0 x1 y1 z1 res path)))
```

---

## Macro Layer

Turmeric macros add expressive patterns that have no direct Scheme equivalent.

### Variadic Booleans

```turmeric
(defmacro lf-union* [& shapes]
  (reduce (fn [acc s] `(lf-union ~acc ~s)) shapes))

(defmacro lf-intersection* [& shapes]
  (reduce (fn [acc s] `(lf-intersection ~acc ~s)) shapes))

(defmacro lf-difference* [base & cutters]
  (reduce (fn [acc s] `(lf-difference ~acc ~s)) base cutters))
```

Usage:

```turmeric
(lf-union* (lf-sphere 1.0)
           (lf-cylinder 0.3 3.0)
           (lf-box [-0.2 -0.2 -0.2] [0.2 0.2 0.2]))
```

### Linear Array

```turmeric
(defmacro lf-linear-array [shape axis count spacing]
  `(lf-union*
    ,@(map (fn [i]
              `(lf-move ~shape ,(map (fn [a] (* i spacing a)) axis)))
            (range count))))
```

Usage:

```turmeric
;; 5 spheres along X, 3 units apart
(lf-linear-array (lf-sphere 0.4) [1 0 0] 5 3.0)
```

### With-Bounds Helper

```turmeric
(defmacro with-bounds [name lo hi & body]
  `(let [~name [~lo ~hi]]
     ,@body))

;; Usage
(with-bounds b [-5.0 -5.0 -5.0] [5.0 5.0 5.0]
  (lf-save-stl my-shape "out.stl" b 20.0))
```

---

## Tutorial: From Zero to Printed Part

### Part 1 -- Hello Sphere

The simplest possible model: a sphere exported as STL.

```turmeric
(defn main [] :int
  (let [shape (lf-sphere 1.0)
        bounds [[-2.0 -2.0 -2.0] [2.0 2.0 2.0]]]
    (lf-save-stl shape "sphere.stl" bounds 20.0)
    (println "Wrote sphere.stl")
    0))
```

Run it:

```
$ turi run main.tur
Wrote sphere.stl
```

Open `sphere.stl` in any slicer or mesh viewer.

---

### Part 2 -- Basic CSG

Carve a channel through a box:

```turmeric
(defn channel-box [] :int
  (let [body    (lf-box [-2.0 -2.0 -0.5] [2.0 2.0 0.5])
        channel (lf-cylinder 0.3 3.0)
        rotated (lf-rotate-x channel 1.5708)   ; 90 deg -> lies along Y
        result  (lf-difference body rotated)
        bounds  [[-3.0 -3.0 -1.0] [3.0 3.0 1.0]]]
    (lf-save-stl result "channel-box.stl" bounds 30.0)
    0))
```

Key ideas:
- `lf-rotate-x` by pi/2 reorients the cylinder from Z to Y
- `lf-difference` subtracts the cylinder volume from the box

---

### Part 3 -- Parametric Model

A mounting bracket parameterized by size and hole radius:

```turmeric
(defn bracket [width :float height :float thickness :float hole-r :float] :int
  (let [body  (lf-box [0.0 0.0 0.0] [width height thickness])
        hole1 (lf-move (lf-cylinder hole-r (* thickness 2.0))
                       [(* width 0.25) (* height 0.5) (- thickness)])
        hole2 (lf-move (lf-cylinder hole-r (* thickness 2.0))
                       [(* width 0.75) (* height 0.5) (- thickness)])]
    (lf-difference* body hole1 hole2)))

(defn main [] :int
  (let [part   (bracket 60.0 30.0 5.0 3.5)
        bounds [[-5.0 -5.0 -5.0] [65.0 35.0 10.0]]]
    (lf-save-stl part "bracket.stl" bounds 40.0)
    0))
```

Swap any argument to immediately regenerate the geometry -- no GUI required.

---

### Part 4 -- Smooth Blending

A blob character head using smooth unions:

```turmeric
(defn blob-head [] :int
  (let [m     0.4      ; blend radius
        skull (lf-sphere 2.0)
        nose  (lf-move (lf-sphere 0.6) [0.0 2.1 0.3])
        eye-l (lf-move (lf-sphere 0.4) [-0.8 1.9 0.9])
        eye-r (lf-move (lf-sphere 0.4) [ 0.8 1.9 0.9])]
    (lf-blend-union
      (lf-blend-union skull nose m)
      (lf-blend-union eye-l eye-r m)
      m)))

(defn main [] :int
  (let [shape  (blob-head)
        bounds [[-3.0 -1.0 -3.0] [3.0 4.0 4.0]]]
    (lf-save-stl shape "blob-head.stl" bounds 30.0)
    0))
```

Adjust `m` to control how smoothly the parts merge.

---

### Part 5 -- Repetition and Arrays

Honeycomb panel using polar arrays:

```turmeric
(defn hex-cell [r :float h :float] :int
  (let [prism (lf-extrude-z
                (lf-intersection*
                  (lf-rotate-z (lf-rect [-r -r] [r r]) 0.0)
                  (lf-rotate-z (lf-rect [-r -r] [r r]) 1.047)  ; 60 deg
                  (lf-rotate-z (lf-rect [-r -r] [r r]) 2.094)) ; 120 deg
                0.0 h)]
    prism))

(defn honeycomb [rows :int cols :int cell-r :float] :int
  (let [step-x (* cell-r 1.732)      ; sqrt(3) * r
        step-y (* cell-r 1.5)
        plate  (lf-box [0.0 0.0 0.0]
                       [(* cols step-x) (* rows step-y) 3.0])
        cell   (hex-cell (* cell-r 0.85) 4.0)
        cells  (lf-array cell [step-x step-y 0.0])]
    (lf-difference plate cells)))

(defn main [] :int
  (let [panel  (honeycomb 6 8 5.0)
        bounds [[-2.0 -2.0 -2.0] [80.0 50.0 6.0]]]
    (lf-save-stl panel "honeycomb.stl" bounds 25.0)
    0))
```

---

### Part 6 -- Shell and Wall Thickness

Convert a solid shape into a thin-walled shell (useful for hollow prints):

```turmeric
(defn hollow-vase [r-base :float r-top :float h :float wall :float] :int
  (let [outer (lf-blend-union
                (lf-cylinder r-base (* h 0.4))
                (lf-move (lf-cylinder r-top (* h 0.7)) [0.0 0.0 (* h 0.3)])
                (* r-base 0.5))
        inner (lf-offset outer (- wall))
        vase  (lf-difference outer inner)
        ;; Open the top
        cap   (lf-move (lf-half-space-z 0.0) [0.0 0.0 (* h 0.95)])]
    (lf-difference vase cap)))

(defn main [] :int
  (let [shape  (hollow-vase 3.0 2.0 10.0 0.3)
        bounds [[-4.0 -4.0 -1.0] [4.0 4.0 12.0]]]
    (lf-save-stl shape "vase.stl" bounds 40.0)
    0))
```

---

## Example Project: Customizable Gear

A complete self-contained project generating an involute gear approximation from parameters.

### File Layout

```
examples/libfive-gear/
  main.tur       -- entry point, reads CLI args
  gear.tur       -- gear geometry functions
  README.md      -- build and usage notes
```

### `gear.tur`

```turmeric
;; gear.tur -- parametric spur gear via libfive DSL

(defn tooth [pitch-r :float tooth-h :float width :float] :int
  ;; Approximate an involute tooth with a tapered box
  (let [base  (lf-box [(- 0.0 (/ pitch-r 10.0)) 0.0 0.0]
                      [(/ pitch-r 10.0) (+ pitch-r tooth-h) width])
        tip   (lf-offset base (- (/ tooth-h 4.0)))]
    (lf-blend-union base tip 0.05)))

(defn gear-ring [n :int pitch-r :float tooth-h :float width :float] :int
  ;; n teeth distributed evenly around Z axis
  (let [angle-step (/ 6.2832 n)  ; 2*pi / n
        one-tooth  (tooth pitch-r tooth-h width)]
    (reduce
      (fn [acc i]
        (lf-union acc (lf-rotate-z one-tooth (* i angle-step))))
      (lf-rotate-z one-tooth 0.0)
      (range 1 n))))

(defn gear [n :int module :float width :float bore-r :float] :int
  ;; module = pitch diameter / tooth count (standard gear parameter)
  (let [pitch-r (/ (* n module) 2.0)
        tooth-h (* module 2.25)       ; addendum + dedendum
        disk    (lf-cylinder pitch-r width)
        teeth   (gear-ring n pitch-r tooth-h width)
        bore    (lf-cylinder bore-r (* width 3.0))
        full    (lf-union disk teeth)]
    (lf-difference full bore)))
```

### `main.tur`

```turmeric
;; main.tur -- generate gear STL from command-line arguments

(import gear)

(defn usage [] :void
  (println "Usage: turi run main.tur <teeth> <module> <width> <bore> <out.stl>"))

(defn main [args :vec] :int
  (if (< (vec-len args) 5)
    (do (usage) 1)
    (let [n      (parse-int  (vec-get args 0))
          module (parse-float (vec-get args 1))
          width  (parse-float (vec-get args 2))
          bore-r (parse-float (vec-get args 3))
          path   (vec-get args 4)
          shape  (gear n module width bore-r)
          r      (+ (/ (* n module) 2.0) (* module 3.0))
          bounds [(- [0.0 0.0 0.0] [r r 1.0])
                  (+ [0.0 0.0 0.0] [r r (+ width 1.0)])]]
      (lf-save-stl shape path bounds 50.0)
      (println (str "Wrote " path))
      0)))
```

### Usage

```
$ turi run main.tur 24 2.0 8.0 3.0 gear-24t.stl
Wrote gear-24t.stl

$ turi run main.tur 12 2.0 8.0 3.0 pinion-12t.stl
Wrote pinion-12t.stl
```

The two STL files can be imported into a slicer side by side; the tooth pitch automatically matches because both use the same `module` value.

---

## Building and Linking

### CMake Integration

```cmake
find_package(libfive REQUIRED)

add_executable(gear_gen main.c)        # generated by turi compile
target_link_libraries(gear_gen PRIVATE five)
```

Or with the Turmeric build system:

```just
# justfile entry
libfive-gear:
    turi compile examples/libfive-gear/main.tur -o gear_gen \
        --link five --link-dir /usr/local/lib
    ./gear_gen 24 2.0 8.0 3.0 gear-24t.stl
```

### Header Shim

Create `src/libfive_shim.h` with the subset of the libfive C API used by the DSL wrappers:

```c
#pragma once
#include <libfive.h>

// Shape constructors
libfive_tree libfive_tree_sphere(float r, float cx, float cy, float cz);
libfive_tree libfive_tree_box_mitered(
    float xmin, float ymin, float zmin,
    float xmax, float ymax, float zmax);
libfive_tree libfive_tree_cylinder_z(
    float r, float height, float bx, float by, float bz);

// CSG
libfive_tree libfive_tree_union       (libfive_tree a, libfive_tree b);
libfive_tree libfive_tree_intersection(libfive_tree a, libfive_tree b);
libfive_tree libfive_tree_difference  (libfive_tree a, libfive_tree b);

// Transforms
libfive_tree libfive_tree_move(libfive_tree t, float dx, float dy, float dz);

// Export
int libfive_tree_save_mesh(
    libfive_tree t,
    float xmin, float ymin, float zmin,
    float xmax, float ymax, float zmax,
    float res, const char* filename);

void libfive_tree_free(libfive_tree t);
```

---

## Scheme DSL Correspondence Table

For developers migrating from the libfive Scheme REPL, here is a quick reference:

| Scheme | Turmeric DSL | Notes |
|--------|-------------|-------|
| `(sphere r)` | `(lf-sphere r)` | |
| `(sphere r center)` | `(lf-sphere-at r center)` | `center` is a `vec3` |
| `(box-mitered lo hi)` | `(lf-box lo hi)` | |
| `(box-exact lo hi)` | `(lf-box-exact lo hi)` | |
| `(cylinder-z r h base)` | `(lf-cylinder r h)` / `(lf-cylinder-at r h base)` | |
| `(cone-z r h base)` | `(lf-cone r h)` | |
| `(torus-z R r center)` | `(lf-torus R r)` | |
| `(union a b)` | `(lf-union a b)` | Variadic: `(lf-union* ...)` |
| `(intersection a b)` | `(lf-intersection a b)` | |
| `(difference a b)` | `(lf-difference a b)` | |
| `(move s off)` | `(lf-move s off)` | `off` is a `vec3` |
| `(scale-xyz s v)` | `(lf-scale-xyz s v)` | |
| `(rotate-z s angle)` | `(lf-rotate-z s angle)` | angle in radians |
| `(reflect-x s)` | `(lf-reflect-x s)` | |
| `(symmetric-x s)` | `(lf-symmetric-x s)` | reflect + union |
| `(offset s r)` | `(lf-offset s r)` | |
| `(shell s t)` | `(lf-shell s t)` | |
| `(blend a b m)` | `(lf-blend-union a b m)` | |
| `(extrude-z s lo hi)` | `(lf-extrude-z s lo hi)` | |
| `(revolve-z s)` | `(lf-revolve-z s)` | |
| `(array s off)` | `(lf-array s off)` | `off` is a `vec3` |
| `(array-polar s n)` | `(lf-array-polar s n)` | |
| `(save-stl s file res)` | `(lf-save-stl s file bounds res)` | bounds required in Turmeric |
| `(ao-render s)` | `(lf-show s bounds)` | blocks on Studio preview |

---

## Advanced Topics

### Functional Shape Combinators

Because shapes are values, higher-order functions compose naturally:

```turmeric
;; Apply a transform to each item in a list and union them
(defn place-all [shape positions :list] :int
  (reduce (fn [acc pos] (lf-union acc (lf-move shape pos)))
          (lf-move shape (list-head positions))
          (list-tail positions)))

;; Radial symmetry around an arbitrary axis
(defn radial-n [shape :int n :int] :int
  (let [angle (/ 6.2832 n)]
    (reduce (fn [acc i] (lf-union acc (lf-rotate-z shape (* i angle))))
            shape
            (range 1 n))))
```

### Contracts for Safety

Use Turmeric contracts to catch parameter errors before hitting the C layer:

```turmeric
(defn lf-sphere [r :float] :int
  (require! (> r 0.0) "sphere radius must be positive")
  (libfive_tree_sphere r 0.0 0.0 0.0))

(defn lf-save-stl [shape :int path :cstr bounds :bounds res :float] :void
  (require! (> res 0.0) "resolution must be positive")
  (let [[[x0 y0 z0] [x1 y1 z1]] bounds]
    (require! (and (< x0 x1) (< y0 y1) (< z0 z1)) "bounds lo must be < hi")
    (libfive_tree_save_mesh shape x0 y0 z0 x1 y1 z1 res path)))
```

### Lazy Shape Trees

Wrap expensive model computations in Turmeric's lazy thunks so shapes are only evaluated when needed:

```turmeric
(def expensive-shape
  (lazy (gear 64 1.0 12.0 4.0)))

;; shape is not built until first force
(when needs-gear
  (lf-save-stl (force expensive-shape) "gear.stl" bounds 60.0))
```

### Exporting Multiple Variants

```turmeric
(defn export-gear-family [] :void
  (for [teeth [12 18 24 36 48]]
    (let [shape (gear teeth 2.0 8.0 3.0)
          path  (str "gear_" teeth "t.stl")
          r     (+ (* teeth 1.0) 6.0)
          bounds [(- [r r 1.0]) [r r 9.0]]]
      (lf-save-stl shape path bounds 50.0)
      (println (str "Wrote " path)))))
```

---

## File Structure

```
stdlib/
  libfive.tur             -- shape constructors, transforms, booleans, export

src/
  libfive_shim.c          -- thin C shim over libfive C API (if needed)
  libfive_shim.h          -- C declarations used by extern-c bindings

examples/
  libfive-gear/
    main.tur              -- CLI entry point
    gear.tur              -- gear geometry module
    README.md
  libfive-vase/
    main.tur              -- hollow vase example
  libfive-honeycomb/
    main.tur              -- panel with hex cutouts
```

---

## Future Enhancements

1. **Live preview integration**: pipe shapes into libfive Studio socket for hot-reload editing
2. **2-D SVG export**: flatten designs to 2-D and export for laser cutting
3. **libfive expressions**: expose the raw math tree so users can define novel f-rep primitives in Turmeric
4. **Mesh simplification**: post-process STL with OpenMesh to reduce polygon count before slicing
5. **OpenSCAD interop**: emit an OpenSCAD `.scad` file as an alternative backend for users already in that ecosystem
6. **Web WASM demo**: compile a small libfive subset to WASM and render in the Turmeric web REPL

---

## References

- [libfive website](https://libfive.com/)
- [libfive Scheme API reference](https://libfive.com/stdlib/)
- [libfive C API header](https://github.com/libfive/libfive/blob/master/libfive/include/libfive.h)
- [Implicit surface modeling](https://iquilezles.org/articles/distfunctions/) -- Inigo Quilez's f-rep distance function cookbook
- [SDF toolbox](https://mercury.sexy/hg_sdf/) -- hg_sdf reference for advanced primitives
