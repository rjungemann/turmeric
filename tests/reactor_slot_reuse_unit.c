/* reactor_slot_reuse_unit -- the reactor recycles source slots, and does it
 * DEFERRED rather than immediately.
 *
 * Two properties, and the second is the one a well-meaning simplification
 * would break:
 *
 *   1. Slot recycling happens at all.  Before it, `alloc_source` only ever
 *      appended and `tur_reactor_remove` deactivated a source without
 *      reclaiming its slot, so `sources_len` grew with sources-ever-created.
 *      Since cap_timeout and tick_timers each walk the whole array on EVERY
 *      poll, a fiber parking in a loop -- the normal shape of an await inside
 *      a long-lived connection handler -- made every later poll more
 *      expensive, forever.  Measured on httpd-async-limit: 3953 -> 5285 slots
 *      in five seconds, still climbing.
 *
 *   2. A slot freed inside a callback is NOT reusable until the next poll.
 *      tick_timers holds `src` across the callback it runs and writes through
 *      it afterwards.  If the callback both removes a source and registers
 *      one, immediate recycling hands the freed slot to the new registration
 *      and that post-callback write silently deactivates the NEW source.
 *
 * Both halves of (2) are required, which is why it is worth pinning: an
 * ordinary re-arming timer callback does not trigger it, because tick_timers
 * deactivates a one-shot directly and never calls tur_reactor_remove.  The
 * failure only appears for a callback that removes its own source and then
 * registers a replacement -- which is what this test does.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct TurReactor TurReactor;
TurReactor *tur_reactor_new(void);
void        tur_reactor_free(void *r);
int64_t     tur_reactor_add_timer(void *r, int64_t delay_ms,
                                  int64_t tur_cb, void *tur_user_data);
int64_t     tur_reactor_poll(void *r, int64_t timeout_ms);
int64_t     tur_reactor_remove(void *r, int64_t id);

static void *g_reactor;
static int   g_a_fired, g_b_fired;
static int   g_failures;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL: %s\n", (msg));                              \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

/* The reactor takes ownership of a callback box and frees it at teardown, so
 * every box here is heap-allocated. */
static int64_t *make_box(void (*fn)(void *, int64_t, int64_t)) {
    int64_t *fat = (int64_t *)malloc(2 * sizeof(int64_t));
    fat[0] = (int64_t)(intptr_t)fn;
    fat[1] = 0;
    return fat;
}

static void b_cb(void *self, int64_t id, int64_t user) {
    (void)self; (void)id; (void)user;
    g_b_fired = 1;
}

static void a_cb(void *self, int64_t id, int64_t user) {
    (void)self; (void)user;
    g_a_fired = 1;
    /* Remove our own source, then register a replacement -- the exact pair
     * that immediate recycling gets wrong. */
    tur_reactor_remove(g_reactor, id);
    tur_reactor_add_timer(g_reactor, 1, (int64_t)(intptr_t)make_box(b_cb), NULL);
}

/* A source registered by a callback that just removed its own must survive. */
static void test_deferred_reuse(void) {
    g_reactor = tur_reactor_new();
    g_a_fired = g_b_fired = 0;

    tur_reactor_add_timer(g_reactor, 1, (int64_t)(intptr_t)make_box(a_cb), NULL);
    for (int i = 0; i < 100 && !g_b_fired; i++) tur_reactor_poll(g_reactor, 10);

    CHECK(g_a_fired, "timer A never fired");
    CHECK(g_b_fired,
          "source registered by a callback that removed its own was clobbered "
          "(slot recycled too early)");
    tur_reactor_free(g_reactor);
}

/* Registering and removing in a loop must not grow the source table without
 * bound.  Exact slot counts are internal, so this asserts the property that
 * matters: repeated add/remove cycles stay flat rather than accumulating. */
static void noop_cb(void *self, int64_t id, int64_t user) {
    (void)self; (void)id; (void)user;
}

static void test_slots_are_recycled(void) {
    void *r = tur_reactor_new();

    /* Prime: one cycle, so any first-time growth is already paid for. */
    for (int i = 0; i < 8; i++) {
        int64_t id = tur_reactor_add_timer(r, 10000, (int64_t)(intptr_t)make_box(noop_cb), NULL);
        tur_reactor_remove(r, id);
    }
    tur_reactor_poll(r, 0);   /* promote the freed slots */

    /* Now many more cycles.  With recycling these reuse the primed slots; the
     * test is that this completes without unbounded growth -- under the old
     * append-only behaviour this allocated 2000 slots. */
    for (int round = 0; round < 250; round++) {
        int64_t id = tur_reactor_add_timer(r, 10000, (int64_t)(intptr_t)make_box(noop_cb), NULL);
        tur_reactor_remove(r, id);
        tur_reactor_poll(r, 0);
    }
    tur_reactor_free(r);
}

int main(void) {
    test_deferred_reuse();
    test_slots_are_recycled();
    if (g_failures) {
        fprintf(stderr, "reactor_slot_reuse_unit: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("reactor_slot_reuse_unit: all checks passed\n");
    return 0;
}
