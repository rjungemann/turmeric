/* stm.h - Software Transactional Memory (TL2)
 *
 * Haskell-style STM with optimistic reads and a fine-grained, striped commit
 * path.  The discipline is TL2 (Transactional Locking II):
 *
 *   - A global monotonic `version_clock` is the source of truth for read-set
 *     validation.  A transaction snapshots it at begin time (`read_stamp`).
 *   - Each TVar carries a `version` stamp.  The low bit is a "locked" flag set
 *     by a committer while it publishes new values; committed stamps are even.
 *   - Reads are lock-free: snapshot the version, load the value, re-check the
 *     version, and abort if the stamp moved, is locked, or is newer than the
 *     read snapshot.  An aborted read sets `tx->aborted` and the `atomically`
 *     retry loop restarts the closure.
 *   - Commit locks the buckets covering the write set (sorted by bucket index
 *     for a stable lock order), bumps the clock, re-validates the read set,
 *     publishes the writes under the per-TVar lock bit, then wakes retriers.
 *   - `retry()` parks on a single global condition variable and filters wakeups
 *     by the per-bucket `commit_seq`, so a retrier only re-runs when a bucket it
 *     actually read has committed.
 *
 * Lock granularity is the bucket (`STM_NUM_LOCK_BUCKETS` stripes), not the
 * individual TVar, so TVars carry no mutex/cond of their own.
 *
 * Limitations (see docs/guides/stm-guide.md): no nested transactions, no
 * in-transaction I/O, fixed-size read/write/defer sets, and boxed TVar payloads
 * must be treated as immutable after publication (writers swap the pointer).
 */

#ifndef TUR_STM_H
#define TUR_STM_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#include "platform.h"  /* TUR_THREAD_LOCAL */

/* Forward declaration for TypeInfo from types.h */
typedef struct TypeInfo TypeInfo;

/* Number of lock buckets for lock striping. Must be a power of two. */
#define STM_NUM_LOCK_BUCKETS 64

/* Maximum read set size per transaction */
#define STM_MAX_READ_SET  256

/* Maximum write set size per transaction */
#define STM_MAX_WRITE_SET 128

/* Maximum number of defers per transaction */
#define STM_MAX_DEFERS 32

/* A TVar (Transaction Variable) holds a value that can be read/written
 * atomically within STM transactions. The version stamp is accessed with
 * atomic loads/stores (see the TL2 notes above); its low bit is the commit
 * lock flag. */
typedef struct TVar {
    TypeInfo *type;           /* Type information for the stored value */
    void *value;              /* Current value (may be boxed) */
    uint64_t version;         /* Version stamp (atomic; low bit = locked) */
} TVar;

/* Defer callback type for transaction commit/abort */
typedef void (*stm_defer_fn_t)(void *env);

/* Defer entry - a defer registered within a transaction */
typedef struct STM_Defer {
    stm_defer_fn_t fn;
    void *env;
    bool on_commit;    /* true = fire on commit, false = fire on abort */
} STM_Defer;

/* A transaction context. Each thread has at most one active transaction. */
typedef struct STM_Transaction {
    /* Read set tracking */
    TVar *read_set[STM_MAX_READ_SET];
    uint64_t read_versions[STM_MAX_READ_SET];
    int read_count;

    /* Write set tracking */
    TVar *write_set[STM_MAX_WRITE_SET];
    void *new_values[STM_MAX_WRITE_SET];
    int write_count;

    /* Defer stack */
    STM_Defer defers[STM_MAX_DEFERS];
    int defer_count;

    /* TL2 begin-time snapshot of the global version clock */
    uint64_t read_stamp;

    /* State flags */
    bool retry_requested;    /* retry was called */
    bool aborted;            /* read-set conflict observed mid-body */
    bool committed;          /* transaction was committed */
} STM_Transaction;

/* Lock bucket for lock striping. The bucket mutex serializes committers whose
 * write sets touch this stripe; `commit_seq` advances on every such commit and
 * drives the retry-wakeup filter. */
typedef struct STM_LockBucket {
    pthread_mutex_t lock;     /* Bucket-level commit lock */
    uint64_t commit_seq;      /* Bumped on every commit touching this bucket */
} STM_LockBucket;

/* Global STM state */
typedef struct STM_State {
    uint64_t version_clock;                             /* monotonic; atomic */
    pthread_mutex_t retry_lock;                         /* guards retry_cond */
    pthread_cond_t  retry_cond;                         /* global retry wakeup */
    STM_LockBucket lock_buckets[STM_NUM_LOCK_BUCKETS];  /* lock stripes */
} STM_State;

/* Get the global STM state (initializes on first use) */
STM_State *tur_stm_state(void);

/* Get the current transaction for this thread */
STM_Transaction *tur_stm_current_tx(void);

/* Set the current transaction for this thread */
void tur_stm_set_current_tx(STM_Transaction *tx);

/* Initialize STM subsystem (idempotent; also runs lazily on first use) */
void tur_stm_init(void);

/* ==================== TVar operations ==================== */

/* Create a new TVar with an initial value */
TVar *tur_tvar_new(TypeInfo *type, void *initial_value);

/* Read a TVar within a transaction. Records the read in the transaction's read
 * set. Returns NULL and sets tx->aborted on a TL2 read conflict. */
void *tur_tvar_read(STM_Transaction *tx, TVar *tv);

/* Write to a TVar within a transaction. Records the write in the transaction's write set. */
void tur_tvar_write(STM_Transaction *tx, TVar *tv, void *value);

/* Modify a TVar: read, apply function, write (all within same transaction) */
void *tur_tvar_modify(STM_Transaction *tx, TVar *tv, void *(*fn)(void *, void *), void *fn_env);

/* Swap a TVar: read old value, write new value, return old (all within same transaction) */
void *tur_tvar_swap(STM_Transaction *tx, TVar *tv, void *new_value);

/* Compare-and-swap within a transaction. Returns true if swap succeeded. */
bool tur_tvar_cas(STM_Transaction *tx, TVar *tv, void *old_value, void *new_value);

/* Free a TVar (only when no longer needed) */
void tur_tvar_free(TVar *tv);

/* ==================== Transaction operations ==================== */

/* Create a new transaction */
STM_Transaction *tur_stm_new_transaction(void);

/* Free a transaction */
void tur_stm_free_transaction(STM_Transaction *tx);

/* Validate the transaction: check all read versions are still current */
bool tur_stm_validate(STM_Transaction *tx);

/* Commit the transaction: apply writes, advance versions, notify waiters */
bool tur_stm_commit(STM_Transaction *tx);

/* Abort the transaction: discard writes, fire abort defers */
void tur_stm_abort(STM_Transaction *tx);

/* Request retry: the atomically loop will park until a read TVar changes */
void tur_stm_retry(STM_Transaction *tx);

/* Check a condition: request retry if false */
void tur_stm_check(bool condition);

/* ==================== atomically ==================== */

/* Type for the closure passed to tur_atomically */
typedef void *(*stm_fn_t)(void *env);

/* Execute a closure as an atomic transaction with retry loop */
void *tur_atomically(stm_fn_t fn, void *env);

/* ==================== Lock ordering helpers ==================== */

/* Sort TVars by address for lock ordering */
int tur_stm_lock_cmp(const void *a, const void *b);

/* ==================== Defer operations ==================== */

/* Register a defer to fire on transaction commit */
int tur_stm_defer_on_commit(STM_Transaction *tx, stm_defer_fn_t fn, void *env);

/* Register a defer to fire on transaction abort */
int tur_stm_defer_on_abort(STM_Transaction *tx, stm_defer_fn_t fn, void *env);

/* Fire all commit defers for a transaction */
void tur_stm_fire_commit_defers(STM_Transaction *tx);

/* Fire all abort defers for a transaction */
void tur_stm_fire_abort_defers(STM_Transaction *tx);

#endif /* TUR_STM_H */
