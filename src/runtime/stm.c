/* stm.c - Software Transactional Memory (TL2)
 *
 * Transactional Locking II: optimistic, lock-free reads validated against a
 * global version clock; commit publishes the write set under striped bucket
 * locks with a per-TVar lock bit so concurrent readers never observe a
 * half-applied commit. See stm.h for the full protocol description.
 *
 * Version stamps and the global clock are accessed with the __atomic builtins
 * (GCC/Clang); the low bit of a TVar version is the commit lock flag, so
 * committed stamps are always even and advance by 2.
 */

#include "stm.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>

#include "platform.h"

/* Global STM state */
static STM_State global_stm_state;
static pthread_once_t stm_once = PTHREAD_ONCE_INIT;

/* Thread-local current transaction */
static TUR_THREAD_LOCAL STM_Transaction *current_tx = NULL;

/* ============================================================================
 * Initialization
 * ============================================================================ */

static void stm_do_init(void) {
    __atomic_store_n(&global_stm_state.version_clock, (uint64_t)0, __ATOMIC_RELEASE);
    pthread_mutex_init(&global_stm_state.retry_lock, NULL);
    pthread_cond_init(&global_stm_state.retry_cond, NULL);
    for (int i = 0; i < STM_NUM_LOCK_BUCKETS; i++) {
        pthread_mutex_init(&global_stm_state.lock_buckets[i].lock, NULL);
        global_stm_state.lock_buckets[i].commit_seq = 0;
    }
}

STM_State *tur_stm_state(void) {
    pthread_once(&stm_once, stm_do_init);
    return &global_stm_state;
}

STM_Transaction *tur_stm_current_tx(void) {
    return current_tx;
}

void tur_stm_set_current_tx(STM_Transaction *tx) {
    current_tx = tx;
}

void tur_stm_init(void) {
    pthread_once(&stm_once, stm_do_init);
}

/* ============================================================================
 * TVar operations
 * ============================================================================ */

/* Hash a TVar pointer to a lock bucket index. Shift past the malloc alignment
 * bits so striping isn't degenerate. */
static unsigned tvar_to_bucket(TVar *tv) {
    uintptr_t addr = (uintptr_t)tv;
    return (unsigned)((addr >> 4) & (STM_NUM_LOCK_BUCKETS - 1));
}

TVar *tur_tvar_new(TypeInfo *type, void *initial_value) {
    tur_stm_init();
    TVar *tv = (TVar *)malloc(sizeof(TVar));
    if (!tv) return NULL;
    tv->type = type;
    tv->value = initial_value;
    /* Version 0 == even (unlocked), committed at the dawn of time. */
    __atomic_store_n(&tv->version, (uint64_t)0, __ATOMIC_RELEASE);
    return tv;
}

void tur_tvar_free(TVar *tv) {
    if (!tv) return;
    /* Serialize against any concurrent committer publishing to this stripe so
     * the free can't race a commit that still holds the bucket lock. */
    STM_State *st = tur_stm_state();
    STM_LockBucket *bucket = &st->lock_buckets[tvar_to_bucket(tv)];
    pthread_mutex_lock(&bucket->lock);
    pthread_mutex_unlock(&bucket->lock);
    free(tv);
}

void *tur_tvar_read(STM_Transaction *tx, TVar *tv) {
    if (!tx || !tv) return NULL;

    /* Reads to our own buffered writes return the new value. */
    for (int i = 0; i < tx->write_count; i++) {
        if (tx->write_set[i] == tv) {
            return tx->new_values[i];
        }
    }

    /* TL2 lock-free read: snapshot version, load value, re-check version. */
    uint64_t v1 = __atomic_load_n(&tv->version, __ATOMIC_ACQUIRE);
    if ((v1 & 1u) || v1 > tx->read_stamp) {
        tx->aborted = true;
        return NULL;
    }
    void *val = __atomic_load_n(&tv->value, __ATOMIC_ACQUIRE);
    uint64_t v2 = __atomic_load_n(&tv->version, __ATOMIC_ACQUIRE);
    if (v1 != v2) {
        tx->aborted = true;
        return NULL;
    }

    /* Record the read (once) in the read set. */
    if (tx->read_count < STM_MAX_READ_SET) {
        bool seen = false;
        for (int i = 0; i < tx->read_count; i++) {
            if (tx->read_set[i] == tv) { seen = true; break; }
        }
        if (!seen) {
            tx->read_set[tx->read_count] = tv;
            tx->read_versions[tx->read_count] = v1;
            tx->read_count++;
        }
    }

    return val;
}

void tur_tvar_write(STM_Transaction *tx, TVar *tv, void *value) {
    if (!tx || !tv) return;

    /* Check if already in write set - update */
    for (int i = 0; i < tx->write_count; i++) {
        if (tx->write_set[i] == tv) {
            tx->new_values[i] = value;
            return;
        }
    }

    /* A read-then-write keeps its read-set entry: the recorded version is the
     * dependency the commit must still validate, or a concurrent committer's
     * update to this same TVar would be silently lost. (A write with no prior
     * read is a blind write and never enters the read set.) */

    /* Add to write set */
    if (tx->write_count < STM_MAX_WRITE_SET) {
        tx->write_set[tx->write_count] = tv;
        tx->new_values[tx->write_count] = value;
        tx->write_count++;
    }
}

void *tur_tvar_modify(STM_Transaction *tx, TVar *tv, void *(*fn)(void *, void *), void *fn_env) {
    if (!tx || !tv || !fn) return NULL;

    void *old_value = tur_tvar_read(tx, tv);
    /* Apply the function and write its result back (read-modify-write). */
    void *new_value = fn(old_value, fn_env);
    tur_tvar_write(tx, tv, new_value);
    return old_value;
}

void *tur_tvar_swap(STM_Transaction *tx, TVar *tv, void *new_value) {
    if (!tx || !tv) return NULL;

    void *old_value = tur_tvar_read(tx, tv);
    tur_tvar_write(tx, tv, new_value);
    return old_value;
}

bool tur_tvar_cas(STM_Transaction *tx, TVar *tv, void *old_value, void *new_value) {
    if (!tx || !tv) return false;

    void *current = tur_tvar_read(tx, tv);
    /* Simple pointer comparison */
    if (current == old_value) {
        tur_tvar_write(tx, tv, new_value);
        return true;
    }
    return false;
}

/* ============================================================================
 * Lock ordering
 * ============================================================================ */

/* Compare TVars by address for lock ordering */
int tur_stm_lock_cmp(const void *a, const void *b) {
    const TVar *tv_a = *(const TVar **)a;
    const TVar *tv_b = *(const TVar **)b;
    if (tv_a < tv_b) return -1;
    if (tv_a > tv_b) return 1;
    return 0;
}

/* ============================================================================
 * Transaction validation and commit (TL2)
 * ============================================================================ */

bool tur_stm_validate(STM_Transaction *tx) {
    if (!tx) return false;

    for (int i = 0; i < tx->read_count; i++) {
        TVar *tv = tx->read_set[i];
        uint64_t cur = __atomic_load_n(&tv->version, __ATOMIC_ACQUIRE);
        if ((cur & 1u) || cur != tx->read_versions[i]) {
            return false;  /* locked by a committer or version changed */
        }
    }
    return true;
}

/* Collect the distinct bucket indices covering the write set, sorted ascending
 * (insertion sort over a small array). Returns the count. */
static int stm_collect_write_buckets(STM_Transaction *tx, unsigned *idxs) {
    int n = 0;
    for (int i = 0; i < tx->write_count; i++) {
        unsigned bi = tvar_to_bucket(tx->write_set[i]);
        bool seen = false;
        for (int j = 0; j < n; j++) {
            if (idxs[j] == bi) { seen = true; break; }
        }
        if (!seen) idxs[n++] = bi;
    }
    for (int i = 1; i < n; i++) {
        unsigned k = idxs[i];
        int j = i - 1;
        while (j >= 0 && idxs[j] > k) { idxs[j + 1] = idxs[j]; j--; }
        idxs[j + 1] = k;
    }
    return n;
}

bool tur_stm_commit(STM_Transaction *tx) {
    if (!tx) return false;

    STM_State *st = tur_stm_state();

    /* Lock the buckets covering the write set, in bucket-index order. */
    unsigned idxs[STM_MAX_WRITE_SET];
    int nidx = stm_collect_write_buckets(tx, idxs);
    for (int i = 0; i < nidx; i++) {
        pthread_mutex_lock(&st->lock_buckets[idxs[i]].lock);
    }

    /* Advance the global clock; wv is the new even version for our writes. */
    uint64_t wv = __atomic_add_fetch(&st->version_clock, (uint64_t)2, __ATOMIC_ACQ_REL);

    /* Re-validate the whole read set, including read-then-written TVars (we
     * hold their bucket locks, so the only way the version moved is a committer
     * that finished before us -- which is exactly the conflict we must catch). */
    for (int i = 0; i < tx->read_count; i++) {
        TVar *tv = tx->read_set[i];
        uint64_t cur = __atomic_load_n(&tv->version, __ATOMIC_ACQUIRE);
        if ((cur & 1u) || cur != tx->read_versions[i]) {
            for (int k = nidx - 1; k >= 0; k--) {
                pthread_mutex_unlock(&st->lock_buckets[idxs[k]].lock);
            }
            return false;  /* conflict; caller retries the closure */
        }
    }

    /* Publish writes: mark locked (odd), store value, store new version (even).
     * A concurrent lock-free reader sees the odd stamp and aborts. */
    for (int i = 0; i < tx->write_count; i++) {
        TVar *tv = tx->write_set[i];
        uint64_t locked = __atomic_load_n(&tv->version, __ATOMIC_RELAXED) | 1u;
        __atomic_store_n(&tv->version, locked, __ATOMIC_RELEASE);
        __atomic_store_n(&tv->value, tx->new_values[i], __ATOMIC_RELEASE);
        __atomic_store_n(&tv->version, wv, __ATOMIC_RELEASE);
    }

    tx->committed = true;

    /* Fire commit defers inside the locked region so observers that wake on the
     * commit see a fully-committed state. */
    tur_stm_fire_commit_defers(tx);

    /* Advance the touched buckets' commit_seq (drives the retry filter). */
    for (int i = 0; i < nidx; i++) {
        __atomic_add_fetch(&st->lock_buckets[idxs[i]].commit_seq, (uint64_t)1, __ATOMIC_ACQ_REL);
    }

    /* Release bucket locks in reverse order. */
    for (int i = nidx - 1; i >= 0; i--) {
        pthread_mutex_unlock(&st->lock_buckets[idxs[i]].lock);
    }

    /* Wake any parked retriers; the per-bucket filter sorts out who re-runs. */
    pthread_mutex_lock(&st->retry_lock);
    pthread_cond_broadcast(&st->retry_cond);
    pthread_mutex_unlock(&st->retry_lock);

    return true;
}

void tur_stm_abort(STM_Transaction *tx) {
    if (!tx) return;
    if (tx->committed) return;  /* Already committed */

    tx->aborted = true;

    /* Fire abort defers */
    tur_stm_fire_abort_defers(tx);
}

/* ============================================================================
 * Retry: global cond + per-bucket commit_seq filter
 * ============================================================================ */

void tur_stm_retry(STM_Transaction *tx) {
    if (!tx) return;
    /* The actual parking happens in tur_atomically once the body has populated
     * the read set; here we just request it. */
    tx->retry_requested = true;
}

/* Park the calling thread until a bucket covering the read set commits. */
static void stm_park(STM_State *st, STM_Transaction *tx) {
    unsigned idxs[STM_MAX_READ_SET];
    uint64_t seqs[STM_MAX_READ_SET];
    int n = 0;
    for (int i = 0; i < tx->read_count; i++) {
        unsigned bi = tvar_to_bucket(tx->read_set[i]);
        bool seen = false;
        for (int j = 0; j < n; j++) {
            if (idxs[j] == bi) { seen = true; break; }
        }
        if (!seen) {
            idxs[n] = bi;
            seqs[n] = __atomic_load_n(&st->lock_buckets[bi].commit_seq, __ATOMIC_ACQUIRE);
            n++;
        }
    }
    if (n == 0) return;  /* nothing read -> retry would deadlock; let caller spin */

    pthread_mutex_lock(&st->retry_lock);
    for (;;) {
        bool advanced = false;
        for (int i = 0; i < n; i++) {
            uint64_t cur = __atomic_load_n(&st->lock_buckets[idxs[i]].commit_seq, __ATOMIC_ACQUIRE);
            if (cur != seqs[i]) { advanced = true; break; }
        }
        if (advanced) break;
        pthread_cond_wait(&st->retry_cond, &st->retry_lock);
    }
    pthread_mutex_unlock(&st->retry_lock);
}

void tur_stm_check(bool condition) {
    if (!condition) {
        STM_Transaction *tx = tur_stm_current_tx();
        if (tx) {
            tur_stm_retry(tx);
        }
    }
}

/* ============================================================================
 * atomically - the main entry point
 * ============================================================================ */

void *tur_atomically(stm_fn_t fn, void *env) {
    if (!fn) return NULL;

    STM_State *st = tur_stm_state();
    STM_Transaction *tx = tur_stm_new_transaction();
    if (!tx) return NULL;
    STM_Transaction *prev_tx = tur_stm_current_tx();

    while (true) {
        /* Begin: reset the log and snapshot the global clock. */
        tx->read_count = 0;
        tx->write_count = 0;
        tx->retry_requested = false;
        tx->aborted = false;
        tx->committed = false;
        tx->read_stamp = __atomic_load_n(&st->version_clock, __ATOMIC_ACQUIRE);
        tur_stm_set_current_tx(tx);

        /* Execute the transaction body. */
        fn(env);

        /* retry / check-false: park on the read set, then re-run. */
        if (tx->retry_requested) {
            tur_stm_set_current_tx(prev_tx);
            stm_park(st, tx);
            continue;
        }

        /* Read conflict observed mid-body: restart immediately. */
        if (tx->aborted) {
            tur_stm_set_current_tx(prev_tx);
            continue;
        }

        /* Try to commit. */
        bool success = tur_stm_commit(tx);
        tur_stm_set_current_tx(prev_tx);

        if (success) {
            void *ret = NULL;
            if (tx->write_count > 0) {
                ret = tx->new_values[tx->write_count - 1];
            }
            tur_stm_free_transaction(tx);
            return ret;
        }

        /* Commit validation failed - retry from the top. */
    }
}

/* ============================================================================
 * Defer operations
 * ============================================================================ */

int tur_stm_defer_on_commit(STM_Transaction *tx, stm_defer_fn_t fn, void *env) {
    if (!tx || !fn) return -1;
    if (tx->defer_count >= STM_MAX_DEFERS) return -1;
    tx->defers[tx->defer_count].fn = fn;
    tx->defers[tx->defer_count].env = env;
    tx->defers[tx->defer_count].on_commit = true;
    tx->defer_count++;
    return 0;
}

int tur_stm_defer_on_abort(STM_Transaction *tx, stm_defer_fn_t fn, void *env) {
    if (!tx || !fn) return -1;
    if (tx->defer_count >= STM_MAX_DEFERS) return -1;
    tx->defers[tx->defer_count].fn = fn;
    tx->defers[tx->defer_count].env = env;
    tx->defers[tx->defer_count].on_commit = false;
    tx->defer_count++;
    return 0;
}

void tur_stm_fire_commit_defers(STM_Transaction *tx) {
    if (!tx) return;
    /* Fire in reverse order (LIFO) */
    for (int i = tx->defer_count - 1; i >= 0; i--) {
        if (tx->defers[i].on_commit && tx->defers[i].fn) {
            tx->defers[i].fn(tx->defers[i].env);
        }
    }
}

void tur_stm_fire_abort_defers(STM_Transaction *tx) {
    if (!tx) return;
    /* Fire in reverse order (LIFO) */
    for (int i = tx->defer_count - 1; i >= 0; i--) {
        if (!tx->defers[i].on_commit && tx->defers[i].fn) {
            tx->defers[i].fn(tx->defers[i].env);
        }
    }
}

/* ============================================================================
 * Transaction management
 * ============================================================================ */

STM_Transaction *tur_stm_new_transaction(void) {
    STM_Transaction *tx = (STM_Transaction *)calloc(1, sizeof(STM_Transaction));
    if (!tx) return NULL;
    return tx;
}

void tur_stm_free_transaction(STM_Transaction *tx) {
    if (!tx) return;
    free(tx);
}
