# Snake Game — CMake + CPM + Raylib + Turmeric Test Project

A **proof-of-concept game** that validates Turmeric's integration with a real C library (Raylib), demonstrates interoperability via CPM dependency management, and showcases Turmeric's advanced features — particularly **algebraic effects** for clean game-loop architecture, **typeclasses** for ad-hoc polymorphism, **defer** for resource safety, and **pattern matching** for game logic.

---

## 1. Goals

| Goal | Priority | Success Criterion |
|---|---|---|
| Validate CMake + CPM + Raylib build pipeline | High | Project builds and runs on macOS/Linux |
| Demonstrate Turmeric ↔ C FFI | High | Raylib functions callable from Turmeric |
| Showcase algebraic effects | High | Game loop uses `perform`/`handle` for input/state |
| Showcase typeclasses | Medium | Game entities use `Eq`, `Show`, or custom classes |
| Showcase `defer` | Medium | Resources (textures, windows) auto-cleanup |
| Showcase pattern matching | Medium | Game state transitions use `match`/`cond` |
| Minimal C glue | Medium | ≤50 lines of C shim code |

---

## 2. Project Structure

```
fith/
├── CMakeLists.txt                    # Root: defines turmeric, build targets
├── cmake/
│   └── CPM.cmake                    # CPM download (if not system-installed)
├── external/
│   └── CPM.cmake                    # Alternative: vendored CPM
├── examples/
│   └── snake/
│       ├── CMakeLists.txt            # Game target + Raylib dependency
│       ├── src/
│       │   ├── main.tur              # Entry: game loop in Turmeric
│       │   ├── game.tur              # Core game logic + effects
│       │   ├── state.tur             # Game state types + typeclasses
│       │   └── rayLibShim.c          # C shim: Raylib ↔ Turmeric FFI
│       └── assets/
│           └── font.png             # Optional: custom font
├── stdlib/
│   └── raylib.tur                   # Raylib bindings (auto-generated)
└── build/
```

---

## 3. Build System

### 3.1 CMake Root (`CMakeLists.txt`)

```cmake
cmake_minimum_required(VERSION 3.20)
project(fith LANGUAGES C CXX)

# CPM setup
include(cmake/CPM.cmake)

# Build Turmeric compiler first
add_subdirectory(src)

# Games directory
add_subdirectory(games)
```

### 3.2 Snake Game (`examples/snake/CMakeLists.txt`)

```cmake
# CPM: fetch Raylib
CPMAddPackage(
  NAME raylib
  GITHUB_REPO raysan5/raylib
  VERSION 5.0
  OPTIONS "BUILD_EXAMPLES OFF" "BUILD_GAMES OFF"
)

# Turmeric-generated executable
add_custom_command(
  OUTPUT snake
  COMMAND ${CMAKE_SOURCE_DIR}/build/tur build ${CMAKE_CURRENT_SOURCE_DIR}/src/main.tur
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS ${CMAKE_SOURCE_DIR}/build/tur
  COMMENT "Compiling Snake with Turmeric"
)

# Link against Raylib
add_executable(snake ${CMAKE_CURRENT_SOURCE_DIR}/src/rayLibShim.c)
target_link_libraries(snake PRIVATE raylib)
add_dependencies(snake snake_compile)

# Copy assets
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/assets DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
```

---

## 4. Raylib C Shim (`src/rayLibShim.c`)

Minimal C wrapper exposing Raylib functions with C99-compatible signatures for Turmeric FFI:

```c
// raylibShim.c - thin wrapper so Turmeric can call Raylib via extern-c
#include <raylib.h>

// Window management
void tur_init_window(int width, int height, const char *title) {
    InitWindow(width, height, title);
}
void tur_close_window(void) {
    CloseWindow();
}
bool tur_window_should_close(void) {
    return WindowShouldClose();
}

// Input
bool tur_is_key_pressed(int key) {
    return IsKeyPressed(key);
}

// Drawing
void tur_begin_drawing(void) {
    BeginDrawing();
}
void tur_end_drawing(void) {
    EndDrawing();
}
void tur_clear_background(int r, int g, int b) {
    ClearBackground((Color){r, g, b, 255});
}
void tur_draw_rectangle(int x, int y, int w, int h, int r, int g, int b) {
    DrawRectangle(x, y, w, h, (Color){r, g, b, 255});
}
void tur_set_target_fps(int fps) {
    SetTargetFPS(fps);
}
```

---

## 5. Turmeric Raylib Bindings (`stdlib/raylib.tur`)

Auto-generated or manually written bindings:

```clojure
;; raylib.tur - FFI bindings
(module raylib)

(extern-c init-window [^int width ^int height ^cstr title] : void)
(extern-c close-window [] : void)
(extern-c window-should-close [] : bool)
(extern-c is-key-pressed [^int key] : bool)
(extern-c begin-drawing [] : void)
(extern-c end-drawing [] : void)
(extern-c clear-background [^int r ^int g ^int b] : void)
(extern-c draw-rectangle [^int x ^int y ^int w ^int h ^int r ^int g ^int b] : void)
(extern-c set-target-fps [^int fps] : void)

;; Key codes (subset)
(def KEY_UP    262)
(def KEY_DOWN  264)
(def KEY_LEFT  263)
(def KEY_RIGHT 265)
(def KEY_ESCAPE 256)

;; Color constants
(def RED   (color 255 0 0))
(def GREEN (color 0 255 0))
(def BLUE  (color 0 0 255))
(def BLACK (color 0 0 0))
(def WHITE (color 255 255 255))

(defstruct color [r : int, g : int, b : int])
```

---

## 6. Game Architecture with Effects

### 6.1 Effect Definitions

We define algebraic effects for the game domain:

```clojure
;; state.tur
(module state)

;; Effect: read current game input
(defeffect Read-Input [] : KeyEvent)

;; Effect: get current time (for frame-independent movement)
(defeffect Get-Time [] : float)

;; Effect: render a game object
(defeffect Render [obj : GameObject] : nil)

;; Effect: game over signal
(defeffect Game-Over [score : int] : nil)

(defstruct KeyEvent [key : int, pressed : bool])
```

### 6.2 Typeclasses for Game Entities

```clojure
;; Typeclass: things that can be drawn
(defclass Drawable [a]
  (draw [self : a] : nil))

;; Typeclass: things that can update their position
(defclass Updatable [a]
  (update [self : a ^float dt] : a))

;; Typeclass: things that can collide
(defclass Collidable [a]
  (collides? [self : a other : a] : bool)
  (bounds [self : a] : Rectangle))

(defstruct Rectangle [x : int, y : int, w : int, h : int])

;; Snake segment
(defstruct Segment [x : int, y : int])

;; Snake
(defstruct Snake
  [segments : (vec Segment)
   direction : Direction
   grow-pending : int])

;; Food
(defstruct Food [x : int, y : int])

;; Game state
(defstruct GameState
  [snake : Snake
   food : Food
   score : int
   grid-width : int
   grid-height : int
   cell-size : int])

;; Direction enum
(defenum Direction [UP DOWN LEFT RIGHT])
```

### 6.3 Instance Implementations

```clojure
;; Make Segment collidable
(definstance Collidable Segment
  (collides? [self other]
    (and (= self.x other.x)
         (= self.y other.y)))
  (bounds [self]
    (Rectangle self.x self.y 1 1))

;; Make Snake drawable
(definstance Drawable Snake
  (draw [self]
    (for [seg self.segments]
      (perform (Render (Rectangle seg.x seg.y 1 1 GREEN))))))

;; Make Snake updatable
(definstance Updatable Snake
  (update [self dt]
    (let [speed 10
          head (vec-get self.segments 0)
          new-head (match self.direction
                     UP    (Segment head.x (- head.y 1))
                     DOWN  (Segment head.x (+ head.y 1))
                     LEFT  (Segment (- head.x 1) head.y)
                     RIGHT (Segment (+ head.x 1) head.y))]
      (if self.grow-pending
        (Snake (vec-conj self.segments new-head) self.direction (- self.grow-pending 1))
        (Snake (vec-conj (vec-rest self.segments) new-head) self.direction 0)))))
```

---

## 7. Game Loop with Effects

### 7.1 Main Entry Point

```clojure
;; main.tur
(module main)
(import raylib)
(import state)

(defn init-game [] : GameState
  (GameState
    (Snake (vec (Segment 10 10)) RIGHT 0)
    (Food 15 15)
    0
    20  ;; grid width
    20  ;; grid height
    20)) ;; cell size in pixels

(defn direction-from-key [^int key] : (option Direction)
  (cond
    (= key raylib/KEY_UP)    (some Direction/UP)
    (= key raylib/KEY_DOWN)  (some Direction/DOWN)
    (= key raylib/KEY_LEFT)  (some Direction/LEFT)
    (= key raylib/KEY_RIGHT) (some Direction/RIGHT)
    :else none))

(defn handle-input [^GameState state] : GameState
  (if (raylib/is-key-pressed raylib/KEY_ESCAPE)
    (perform (Game-Over state.score))
    (let [up (raylib/is-key-pressed raylib/KEY_UP)
          down (raylib/is-key-pressed raylib/KEY_DOWN)
          left (raylib/is-key-pressed raylib/KEY_LEFT)
          right (raylib/is-key-pressed raylib/KEY_RIGHT)]
      (cond
        up    (update state { direction: Direction/UP })
        down  (update state { direction: Direction/DOWN })
        left  (update state { direction: Direction/LEFT })
        right (update state { direction: Direction/RIGHT })
        :else state))))
```

### 7.2 Game Loop with Effect Handlers

```clojure
(defn game-loop [^GameState state] : nil
  (handle
    (let [;; Handle input
          state (handle-input state)
          
          ;; Update game state
          dt (perform (Get-Time))
          state (update state dt)
          
          ;; Check collisions
          state (check-collisions state)
          
          ;; Render
          (perform (Render state.snake))
          (perform (Render state.food))
          
          ;; Recurse
          (game-loop state)]
      
      ;; Handler: Read-Input
      (Read-Input [] k)
        (let [event (KeyEvent 0 false)] ;; Simplified: poll keys directly
          (resume k event))
      
      ;; Handler: Get-Time
      (Get-Time [] k)
        (resume k 0.016) ;; ~60 FPS
      
      ;; Handler: Render
      (Render [obj] k)
        (raylib/begin-drawing)
        (raylib/clear-background 0 0 0)
        (draw obj) ;; Uses Drawable typeclass
        (raylib/end-drawing)
        (resume k nil)
      
      ;; Handler: Game-Over
      (Game-Over [score] k)
        (raylib/clear-background 0 0 0)
        (println (concat "Game Over! Score: " (show score)))
        (resume k nil))))

(defn -main []
  (raylib/init-window 800 600 "Turmeric Snake")
  (defer (raylib/close-window)) ;; Auto-cleanup on exit
  (raylib/set-target-fps 60)
  
  (let [initial-state (init-game)]
    (handle
      (game-loop initial-state)
      
      ;; Top-level handler catches Game-Over and exits
      (Game-Over [score] k)
        (println (concat "Final Score: " (itoa score))))))
```

### 7.3 Collision Detection

```clojure
;; state.tur (continued)

(defn check-collisions [^GameState state] : GameState
  (let [head (vec-get state.snake.segments 0)
        ;; Check wall collision
        wall-collision (or (< head.x 0)
                          (>= head.x state.grid-width)
                          (< head.y 0)
                          (>= head.y state.grid-height))
        ;; Check self collision
        tail (vec-rest state.snake.segments)
        self-collision (any? (fn [seg] (collides? head seg)) tail)
        ;; Check food collision
        food-collision (collides? head state.food)]
    
    (cond
      wall-collision (perform (Game-Over state.score))
      self-collision (perform (Game-Over state.score))
      food-collision (update state { snake: (grow-snake state.snake)
                                     food: (random-food state)
                                     score: (+ state.score 10) })
      :else state)))

(defn grow-snake [^Snake snake] : Snake
  (Snake snake.segments snake.direction (+ snake.grow-pending 1)))

(defn random-food [^GameState state] : Food
  ;; Simple: place food at random position not occupied by snake
  (let [rng-seed (perform (Get-Time))
        x (mod (cast (sin rng-seed) int) state.grid-width)
        y (mod (cast (cos rng-seed) int) state.grid-height)
        food (Food x y)]
    (if (collides? food state.snake)
      (random-food state) ;; Retry if collision
      food)))

;; Typeclass instances for Food and GameState
(definstance Drawable Food
  (draw [self]
    (perform (Render (Rectangle self.x self.y 1 1 RED)))))

(definstance Collidable Food
  (collides? [self other]
    (and (= self.x (.- other x))
         (= self.y (.- other y))))
  (bounds [self]
    (Rectangle self.x self.y 1 1)))
```

---

## 8. Feature Showcase Summary

| Turmeric Feature | Usage in Snake Game | Lines of Code |
|---|---|---|
| **Algebraic Effects** | Input, rendering, time, game-over as effects | ~80 |
| **Effect Handlers** | `handle` wraps game loop, dispatches effects | ~40 |
| **Typeclasses** | `Drawable`, `Updatable`, `Collidable` for polymorphism | ~50 |
| **Pattern Matching** | Direction handling, collision checks | ~20 |
| **`defer`** | Window cleanup on exit | 1 |
| **`extern-c` FFI** | Raylib interop | ~15 |
| **Structs/Enums** | GameState, Snake, Direction, etc. | ~30 |
| **Vectors** | Snake segments, game grid | ~10 |

---

## 9. Implementation Phases

### Phase 1: Build Infrastructure (1–2 days)
- [ ] Create `examples/snake/` directory structure
- [ ] Add CPM.cmake to project
- [ ] Create root CMakeLists.txt with Turmeric + games
- [ ] Create snake CMakeLists.txt with Raylib dependency
- [ ] Verify Raylib builds via CPM

### Phase 2: C Shim (1 day)
- [ ] Implement `rayLibShim.c` with essential Raylib functions
- [ ] Verify shim compiles and links
- [ ] Write minimal C test program to validate shim

### Phase 3: Turmeric Bindings (1 day)
- [ ] Create `stdlib/raylib.tur` with extern-c declarations
- [ ] Write simple Turmeric program that opens a window
- [ ] Verify Turmeric → C shim → Raylib call chain works

### Phase 4: Core Game (2–3 days)
- [ ] Implement `state.tur` with structs, typeclasses
- [ ] Implement `game.tur` with game logic
- [ ] Implement `main.tur` with game loop
- [ ] Test basic movement and rendering

### Phase 5: Effects (2–3 days)
- [ ] Define effects: Read-Input, Get-Time, Render, Game-Over
- [ ] Convert game loop to use `perform`/`handle`
- [ ] Implement effect handlers
- [ ] Test effect-driven game flow

### Phase 6: Polish (1–2 days)
- [ ] Add collision detection
- [ ] Add score display
- [ ] Add food spawning
- [ ] Add game over screen
- [ ] Add restart capability

---

## 10. Success Criteria

1. **Build**: `cmake --build build` succeeds on macOS and Linux
2. **Run**: `./examples/snake/snake` opens a window with a playable snake
3. **Controls**: Arrow keys change direction, ESC quits
4. **Rendering**: Snake is green, food is red, background is black
5. **Gameplay**: Snake grows on food, dies on wall/self collision
6. **Effects**: All game logic flows through effect system
7. **Cleanup**: Window closes cleanly, no resource leaks (valgrind clean)

---

## 11. Stretch Goals

| Stretch Goal | Description | Complexity |
|---|---|---|
| Sound effects | Use Raylib audio via effects | Medium |
| High score persistence | Save to file using IO effects | Medium |
| Pause menu | Nested effect handlers | High |
| Multiplayer (2 snakes) | Extend state, add input effects | Medium |
| Serialization | Save/load game state | High |
| WASM port | Emscripten + Raylib WASM | High |
| Custom shaders | Showcase FFI with function pointers | High |

---

## 12. Debugging Strategy

1. **CMake verbose**: `cmake --build build --verbose` for build issues
2. **Raylib test**: Write minimal C program using Raylib to isolate issues
3. **Turmeric test**: Write minimal Turmeric program (no Raylib) to verify compiler
4. **FFI test**: Write Turmeric calling simple C function before Raylib
5. **Effect test**: Write pure Turmeric effect demo before integrating with Raylib

---

## 13. References

- [Raylib](https://www.raylib.com/) — Simple and easy-to-use library to enjoy videogames programming
- [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) — C++ Package Manager
- [Turmeric Plan](turmeric-plan.md) — Language design and implementation
- [Effects Plan](archive/effects-plan.md) — Algebraic effects design for Turmeric
- [OCaml 5 Effects](https://ocaml.org/manual/5.4/effects.html) — Reference for effect semantics
