/*
 * rayLibShim.c — Raylib ↔ Turmeric FFI bridge
 *
 * This file does two jobs:
 *
 *  1. Wraps Raylib functions with C signatures that the Turmeric compiler can
 *     call via `extern-c`.  All integer parameters use `long long` (== int64_t)
 *     so they match the `int64_t` the compiler emits for every `:int` type.
 *
 *  2. Owns all mutable game state (snake segments, food position, score) so
 *     that the Turmeric layer can stay purely functional while still driving
 *     the game.  Turmeric calls small getter/setter/step functions here and
 *     uses algebraic effects for game events.
 *
 * Max shim size: ≤200 lines.
 */

#include <raylib.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

/* =========================================================================
 * Window & drawing
 * ========================================================================= */

void tur_init_window(long long w, long long h, const char *title) {
    InitWindow((int)w, (int)h, title);
}

void tur_close_window(void) {
    CloseWindow();
}

long long tur_window_should_close(void) {
    return WindowShouldClose() ? 1 : 0;
}

void tur_begin_drawing(void) {
    BeginDrawing();
}

void tur_end_drawing(void) {
    EndDrawing();
}

void tur_clear_bg(long long r, long long g, long long b) {
    ClearBackground((Color){(unsigned char)r,
                            (unsigned char)g,
                            (unsigned char)b, 255});
}

void tur_draw_text(const char *text,
                   long long x, long long y,
                   long long size,
                   long long r, long long g, long long b) {
    DrawText(text, (int)x, (int)y, (int)size,
             (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, 255});
}

void tur_set_fps(long long fps) {
    SetTargetFPS((int)fps);
}

/* =========================================================================
 * Input
 * ========================================================================= */

long long tur_is_key_down(long long key) {
    return IsKeyDown((int)key) ? 1 : 0;
}

/* Returns elapsed frame time in whole milliseconds (integer). */
long long tur_get_frame_ms(void) {
    return (long long)(GetFrameTime() * 1000.0f);
}

/* =========================================================================
 * Snake game state
 *
 * Direction encoding:  0 = UP, 1 = RIGHT, 2 = DOWN, 3 = LEFT
 * ========================================================================= */

typedef struct { int x; int y; } Seg;
#define SNAKE_MAX 400

static Seg  _segs[SNAKE_MAX];
static int  _len;
static int  _dir;
static int  _grow;
static int  _food_x;
static int  _food_y;
static int  _score;
static int  _grid_w;
static int  _grid_h;
static int  _rng_seeded;

/* Initialise (or restart) the snake game. */
void tur_snake_init(long long grid_w, long long grid_h) {
    _grid_w = (int)grid_w;
    _grid_h = (int)grid_h;
    _len  = 3;
    _dir  = 1;  /* RIGHT */
    _grow = 0;
    _score = 0;
    _segs[0] = (Seg){ _grid_w / 2,     _grid_h / 2 };
    _segs[1] = (Seg){ _grid_w / 2 - 1, _grid_h / 2 };
    _segs[2] = (Seg){ _grid_w / 2 - 2, _grid_h / 2 };
    if (!_rng_seeded) {
        srand((unsigned int)time(NULL));
        _rng_seeded = 1;
    }
    _food_x = rand() % _grid_w;
    _food_y = rand() % _grid_h;
}

/* Set direction (blocks 180° reversal). */
void tur_snake_set_dir(long long dir) {
    int d = (int)dir;
    if ((_dir == 0 && d == 2) || (_dir == 2 && d == 0)) return;
    if ((_dir == 1 && d == 3) || (_dir == 3 && d == 1)) return;
    _dir = d;
}

/*
 * Advance the snake one step.
 * Returns 1 if still alive, 0 if wall or self collision.
 * On food collision: grows the snake, updates score, spawns new food.
 */
long long tur_snake_step(void) {
    int hx = _segs[0].x, hy = _segs[0].y;
    int nx = hx, ny = hy;
    switch (_dir) {
        case 0: ny--; break;
        case 1: nx++; break;
        case 2: ny++; break;
        case 3: nx--; break;
    }
    /* Wall collision */
    if (nx < 0 || nx >= _grid_w || ny < 0 || ny >= _grid_h) return 0;
    /* Self collision (skip the tail tip if not growing) */
    int check_len = _grow > 0 ? _len : _len - 1;
    for (int i = 0; i < check_len; i++) {
        if (_segs[i].x == nx && _segs[i].y == ny) return 0;
    }
    /* Shift segments */
    int keep = _grow > 0 ? _len : _len - 1;
    if (keep >= SNAKE_MAX) keep = SNAKE_MAX - 1;
    for (int i = keep; i > 0; i--) _segs[i] = _segs[i - 1];
    _segs[0].x = nx;
    _segs[0].y = ny;
    _len = keep + 1;
    if (_grow > 0) _grow--;
    /* Food collision */
    if (nx == _food_x && ny == _food_y) {
        _grow += 3;
        _score += 10;
        _food_x = rand() % _grid_w;
        _food_y = rand() % _grid_h;
    }
    return 1;
}

long long tur_snake_score(void) { return (long long)_score; }

/* =========================================================================
 * Render helpers (called from Turmeric's Drawable instance / defn)
 * ========================================================================= */

void tur_draw_snake(long long cell, long long r, long long g, long long b) {
    Color c = {(unsigned char)r, (unsigned char)g, (unsigned char)b, 255};
    for (int i = 0; i < _len; i++) {
        DrawRectangle(_segs[i].x * (int)cell, _segs[i].y * (int)cell,
                      (int)cell - 1, (int)cell - 1, c);
    }
}

void tur_draw_food(long long cell, long long r, long long g, long long b) {
    Color c = {(unsigned char)r, (unsigned char)g, (unsigned char)b, 255};
    DrawRectangle(_food_x * (int)cell, _food_y * (int)cell,
                  (int)cell - 1, (int)cell - 1, c);
}

/* =========================================================================
 * Text helpers (int → string, composite score text)
 * ========================================================================= */

const char *tur_score_text(void) {
    static char buf[64];
    snprintf(buf, sizeof(buf), "Score: %d", _score);
    return buf;
}

const char *tur_game_over_text(void) {
    static char buf[64];
    snprintf(buf, sizeof(buf), "GAME OVER   Score: %d", _score);
    return buf;
}
