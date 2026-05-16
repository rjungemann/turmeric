# Default: configure (if needed) then debug build
default: build

# ---------------------------------------------------------------------------
# Build targets
# ---------------------------------------------------------------------------

build: debug

debug:
    cmake --build build -j --config Debug

release:
    cmake --build build -j --config Release

tsan:
    cmake --build build -j --config TSan

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

configure:
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5

reconfigure:
    rm -rf build
    just configure

# ---------------------------------------------------------------------------
# Test targets
# ---------------------------------------------------------------------------

test: build
    ctest --output-on-failure --progress --test-dir build

test-tsan: tsan
    TUR_TSAN=1 ctest --output-on-failure --test-dir build

# Run production smoke tests against live turmeric-lang.com + try.turmeric-lang.com.
# Requires: npm install run from the web/ directory (just web-deps).
smoke:
    cd web && npm run test:prod

# ---------------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------------

clean:
    cmake --build build --target clean
    rm -rf build tests/out
    find tests/fixtures -name 'actual.*' -delete
    find tests/cli -name 'actual.*' -delete

clean-test:
    rm -rf tests/out
    find tests/fixtures -name 'actual.*' -delete
    find tests/cli -name 'actual.*' -delete

# ---------------------------------------------------------------------------
# Utility
# ---------------------------------------------------------------------------

repl: build
    ./build/tur repl

run file:
    ./build/tur run {{file}}

build-file file:
    ./build/tur build {{file}}

emit-c file:
    ./build/tur emit-c {{file}}

# ---------------------------------------------------------------------------
# Games / Examples (CMake targets — require Raylib)
# ---------------------------------------------------------------------------

# Enable the examples subtree (Raylib) and build the snake target.
# On a cold build, prefer installing raylib via your package manager first
# (e.g. `brew install raylib`) so CPM does not need to compile it from source.
configure-examples:
    cmake -S . -B build -DTUR_EXAMPLES=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5

snake: configure-examples
    cmake --build build --target snake

minikanren: configure-examples
    cmake --build build --target minikanren

roguelike: configure-examples
    cmake --build build --target roguelike

games: snake minikanren roguelike

run-snake: snake
    ./build/examples/snake/snake

run-minikanren: minikanren
    ./build/examples/minikanren/minikanren

run-roguelike: roguelike
    ./build/examples/roguelike-raylib/roguelike

guestbook: configure-examples
    cmake --build build --target guestbook

run-guestbook: guestbook
    ./build/examples/guestbook/guestbook

# ---------------------------------------------------------------------------
# Script examples (interpreted via `tur run`)
# ---------------------------------------------------------------------------

run-cellular-automata: build
    ./build/tur run examples/cellular-automata.tur

run-scscm-basic: build
    ./build/tur run examples/scscm/basic.tur

run-scscm-live-coding: build
    ./build/tur run examples/scscm/live-coding.tur

run-scscm-pattern-demo: build
    ./build/tur run examples/scscm/pattern-demo.tur

run-signal-processing-basics: build
    ./build/tur run examples/signal-processing/01_basics.tur

run-signal-processing-signals: build
    ./build/tur run examples/signal-processing/02_signals.tur

run-signal-processing-dsp: build
    ./build/tur run examples/signal-processing/03_dsp.tur

run-tidal-basic: build
    ./build/tur run examples/tidal/basic.tur

run-tidal-drums: build
    ./build/tur run examples/tidal/drums.tur

run-tidal-generative: build
    ./build/tur run examples/tidal/generative.tur

run-tidal-livecoding: build
    ./build/tur run examples/tidal/livecoding.tur

run-tidal-melody: build
    ./build/tur run examples/tidal/melody.tur

# ---------------------------------------------------------------------------
# Full rebuild
# ---------------------------------------------------------------------------

rebuild: clean configure build

# ---------------------------------------------------------------------------
# Documentation
# ---------------------------------------------------------------------------

# Generate HTML API docs from stdlib ;;; docstrings.
# Also emits stdlib/docstrings.tur for the runtime (doc name) lookup,
# and web/public/doc-names.json for the web REPL search bar.
docs: guides
    python3 tools/gendocs.py stdlib/ --out docs/html/api/ --emit-tur stdlib/docstrings.tur --emit-json web/public/doc-names.json

# Render markdown guides to HTML pages (served at /docs/html/guides/).
guides:
    python3 tools/genguides.py docs/guides/ --out docs/html/guides/

# ---------------------------------------------------------------------------
# WebAssembly & Web REPL targets
# ---------------------------------------------------------------------------

# Configure with WASM support
configure-wasm:
    cmake -S . -B build-wasm -DTUR_WASM=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5

# Build WASM module (requires Emscripten).
# Runs `just docs` first so stdlib/docstrings.tur is up-to-date.
wasm: docs configure-wasm
    cmake --build build-wasm -j --target tur_wasm

# Set up web dependencies (Monaco Editor, etc.)
web-deps:
    cd web && npm install

# Build web bundle (requires Vite)
web: wasm web-deps
    cd web && npm run build

# Deploy to GitHub Pages
deploy-web: web
    # cd web/dist && git init && git add . && git commit -m "Deploy to GitHub Pages"
    # git push -f git@github.com:turmeric-lang/turmeric.git main:gh-pages
    cd web && npm run deploy

# Run web dev server
web-dev: web-deps
    cd web && npm run dev

# Clean WASM build
clean-wasm:
    rm -rf build-wasm
