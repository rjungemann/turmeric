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

run file:
    ./build/tur run {{file}}

build-file file:
    ./build/tur build {{file}}

emit-c file:
    ./build/tur emit-c {{file}}

# ---------------------------------------------------------------------------
# Games
# ---------------------------------------------------------------------------

# Enable the examples subtree (Raylib) and build the snake target.
# On a cold build, prefer installing raylib via your package manager first
# (e.g. `brew install raylib`) so CPM does not need to compile it from source.
configure-examples:
    cmake -S . -B build -DTUR_EXAMPLES=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5

games: configure-examples
    cmake --build build --target snake

run-snake: games
    ./build/examples/snake/snake

# ---------------------------------------------------------------------------
# Full rebuild
# ---------------------------------------------------------------------------

rebuild: clean configure build

# ---------------------------------------------------------------------------
# WebAssembly & Web REPL targets
# ---------------------------------------------------------------------------

# Configure with WASM support
configure-wasm:
    cmake -S . -B build-wasm -DTUR_WASM=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5

# Build WASM module (requires Emscripten)
wasm: configure-wasm
    cmake --build build-wasm -j --target tur_wasm

# Set up web dependencies (Monaco Editor, etc.)
web-deps:
    cd web && npm install

# Build web bundle (requires Vite)
web: wasm web-deps
    cd web && npm run build

# Deploy to GitHub Pages
deploy-web: web
    cd web/dist && git init && git add . && git commit -m "Deploy to GitHub Pages"
    git push -f git@github.com:turmeric-lang/turmeric.git main:gh-pages

# Run web dev server
web-dev: web-deps
    cd web && npm run dev

# Clean WASM build
clean-wasm:
    rm -rf build-wasm
