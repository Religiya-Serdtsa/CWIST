#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/big_dumb_reply.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/siphash/siphash.h>
#include <cwist/sys/sys_info.h>
#include <cwist/core/macros.h>
#include <ttak/mem/epoch.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

/**
 * @file big_dumb_reply.c
 * @brief Opportunistic response cache that stores stable GET replies in memory or on disk.
 *
 * Concurrency model: bucket chains are published with a CAS push and entry
 * nodes are never freed until destroy (retired entries are tombstoned), so
 * readers walk without locks.  The response blob lives behind an atomic
 * pointer per entry; writers swap it with one exchange and retire the
 * predecessor through libttak EBR, which guarantees a swapped-out blob
 * outlives every reader that entered its epoch before the swap.  Only the
 * janitor (TTL/byte-trim/disk spill, every 64 learns) and the disk-fallback
 * path take the context mutex.
 */

#define BDR_BUCKETS 1024
#define BDR_GC_SWEEP 8
#define BDR_JANITOR_PERIOD 64
#define BDR_DEFAULT_MAX_BYTES CWIST_MIB(32)
#define BDR_DEFAULT_ENTRY_TTL 300
#define BDR_DEFAULT_REVALIDATE_HITS 100000

/**
 * @brief Static SipHash key used to bucket request and response fingerprints.
 */
static const uint8_t BDR_KEY[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                     0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};

/* --- Blob lifetime -------------------------------------------------------- */

static void bdr_blob_free_cb(void *ptr) {
    bdr_blob_t *blob = (bdr_blob_t *)ptr;
    if (!blob) return;
    /* Read the fields before releasing: for inline blobs mem == blob, so the
     * free below clobbers the very bytes the branch would inspect (glibc
     * writes its tcache links into freed chunks). */
    void *mem = blob->mem;
    void (*free_fn)(void *) = blob->free_fn;
    free_fn(mem);
    if (mem != ptr) cwist_free(blob);
}

/* Retire a swapped-out blob; the epoch guarantees no reader still serves it. */
static void bdr_blob_retire(bdr_blob_t *blob) {
    if (blob) ttak_epoch_retire(blob, bdr_blob_free_cb);
}

/* Learned blob: one allocation carries header and bytes (single copy from
 * the caller-owned serialization, whose lifetime the cache cannot borrow). */
static bdr_blob_t *bdr_blob_learn(const void *data, size_t len) {
    bdr_blob_t *blob = cwist_alloc(sizeof(bdr_blob_t) + len);
    if (!blob) return NULL;
    blob->mem = blob;
    blob->free_fn = cwist_free;
    blob->len = len;
    blob->data = (const unsigned char *)(blob + 1);
    memcpy((void *)blob->data, data, len);
    return blob;
}

/* Callback-provided blob: header only, the buffer pointer is swung as-is. */
static bdr_blob_t *bdr_blob_wrap(void *mem, size_t len, void (*free_fn)(void *)) {
    bdr_blob_t *blob = cwist_alloc(sizeof(bdr_blob_t));
    if (!blob) return NULL;
    blob->mem = mem;
    blob->free_fn = free_fn ? free_fn : cwist_free;
    blob->len = len;
    blob->data = (const unsigned char *)mem;
    return blob;
}

static void bdr_bytes_add(cwist_bdr_t *bdr, size_t len) {
    atomic_fetch_add_explicit(&bdr->current_bytes, len, memory_order_relaxed);
}

static void bdr_bytes_sub(cwist_bdr_t *bdr, size_t len) {
    size_t cur = atomic_load_explicit(&bdr->current_bytes, memory_order_relaxed);
    while (cur > 0) {
        size_t next = cur >= len ? cur - len : 0;
        if (atomic_compare_exchange_weak_explicit(&bdr->current_bytes, &cur, next,
                                                  memory_order_relaxed, memory_order_relaxed))
            return;
    }
}

/* Swap a new blob into an entry and retire whatever was published before. */
static void bdr_entry_publish(cwist_bdr_t *bdr, bdr_entry_t *entry, bdr_blob_t *blob) {
    bdr_blob_t *old = atomic_exchange_explicit(&entry->blob, blob, memory_order_acq_rel);
    if (blob) bdr_bytes_add(bdr, blob->len);
    if (old) {
        bdr_bytes_sub(bdr, old->len);
        bdr_blob_retire(old);
    }
}

/* --- Lookup helpers ------------------------------------------------------- */

static uint64_t bdr_hash(const char *method, const char *path) {
    uint64_t h = siphash24((const void*)path, strlen(path), BDR_KEY);
    h ^= (uint64_t)(method[0]);
    return h;
}

static uint64_t bdr_hash_data(const void *data, size_t len) {
    return siphash24(data, len, BDR_KEY);
}

static bool bdr_entry_should_decay(const cwist_bdr_t *bdr, const bdr_entry_t *entry, time_t now) {
    if (!bdr || !entry) return false;
    int64_t created = atomic_load_explicit(&entry->created_at, memory_order_relaxed);
    if (bdr->max_entry_age_sec > 0 && created > 0) {
        if (now - (time_t)created > bdr->max_entry_age_sec) return true;
    }
    if (atomic_load_explicit(&entry->is_stable, memory_order_relaxed) &&
        bdr->revalidate_hits > 0 &&
        atomic_load_explicit(&entry->hits, memory_order_relaxed) >= bdr->revalidate_hits) {
        return true;
    }
    return false;
}

/* Walk a bucket chain; entries are tombstoned rather than freed, so the
 * walk is safe without locks as long as loads are atomic. */
static bdr_entry_t *bdr_find(cwist_bdr_t *bdr, uint64_t req_h) {
    size_t idx = req_h % bdr->bucket_count;
    bdr_entry_t *curr = atomic_load_explicit(&bdr->buckets[idx], memory_order_acquire);
    while (curr) {
        if (curr->request_hash == req_h &&
            !atomic_load_explicit(&curr->retired, memory_order_acquire)) {
            return curr;
        }
        curr = atomic_load_explicit(&curr->next, memory_order_acquire);
    }
    return NULL;
}

/* Insert a fresh candidate entry at the bucket head, lock-free.  Returns
 * the entry that ended up reachable (ours, or the winner of a CAS race). */
static bdr_entry_t *bdr_find_or_insert(cwist_bdr_t *bdr, uint64_t req_h) {
    bdr_entry_t *found = bdr_find(bdr, req_h);
    if (found) return found;

    bdr_entry_t *entry = cwist_alloc(sizeof(bdr_entry_t));
    if (!entry) return NULL;
    entry->request_hash = req_h;
    atomic_init(&entry->response_hash, 0);
    atomic_init(&entry->is_stable, false);
    atomic_init(&entry->retired, false);
    atomic_init(&entry->blob, NULL);
    atomic_init(&entry->hits, 0);
    atomic_init(&entry->created_at, (int64_t)time(NULL));
    entry->revalidate = NULL;
    entry->revalidate_arg = NULL;
    entry->retire_next = NULL;

    size_t idx = req_h % bdr->bucket_count;
    bdr_entry_t *head = atomic_load_explicit(&bdr->buckets[idx], memory_order_acquire);
    do {
        /* A concurrent insert of the same key wins; ours becomes garbage. */
        bdr_entry_t *race = bdr_find(bdr, req_h);
        if (race) {
            cwist_free(entry);
            return race;
        }
        atomic_store_explicit(&entry->next, head, memory_order_relaxed);
    } while (!atomic_compare_exchange_weak_explicit(&bdr->buckets[idx], &head, entry,
                                                    memory_order_release, memory_order_acquire));
    return entry;
}

/* --- Janitor (mutex-serialized, runs every BDR_JANITOR_PERIOD learns) ----- */

static void bdr_retire_entry(cwist_bdr_t *bdr, size_t idx, bdr_entry_t *prev, bdr_entry_t *entry) {
    atomic_store_explicit(&entry->retired, true, memory_order_release);
    if (prev) {
        atomic_store_explicit(&prev->next, atomic_load_explicit(&entry->next, memory_order_acquire),
                              memory_order_release);
    } else {
        atomic_store_explicit(&bdr->buckets[idx], atomic_load_explicit(&entry->next, memory_order_acquire),
                              memory_order_release);
    }
    bdr_entry_publish(bdr, entry, NULL);
    entry->retire_next = bdr->retired_entries;
    bdr->retired_entries = entry;
}

static void bdr_sweep(cwist_bdr_t *bdr, size_t steps) {
    if (!bdr || bdr->bucket_count == 0 || steps == 0) return;
    time_t now = time(NULL);
    for (size_t i = 0; i < steps; ++i) {
        size_t idx = bdr->gc_cursor % bdr->bucket_count;
        bdr->gc_cursor = (bdr->gc_cursor + 1) % bdr->bucket_count;
        bdr_entry_t *prev = NULL;
        bdr_entry_t *curr = atomic_load_explicit(&bdr->buckets[idx], memory_order_acquire);
        while (curr) {
            bdr_entry_t *next = atomic_load_explicit(&curr->next, memory_order_acquire);
            if (!atomic_load_explicit(&curr->retired, memory_order_relaxed) &&
                bdr_entry_should_decay(bdr, curr, now)) {
                bdr_retire_entry(bdr, idx, prev, curr);
            } else {
                prev = curr;
            }
            curr = next;
        }
    }
}

static bool bdr_trim_oldest(cwist_bdr_t *bdr, time_t now) {
    size_t victim_idx = SIZE_MAX;
    bdr_entry_t *victim = NULL;
    bdr_entry_t *victim_prev = NULL;
    int64_t oldest = (int64_t)now;

    for (size_t i = 0; i < bdr->bucket_count; ++i) {
        bdr_entry_t *prev = NULL;
        bdr_entry_t *curr = atomic_load_explicit(&bdr->buckets[i], memory_order_acquire);
        while (curr) {
            if (!atomic_load_explicit(&curr->retired, memory_order_relaxed) &&
                atomic_load_explicit(&curr->blob, memory_order_relaxed)) {
                int64_t created = atomic_load_explicit(&curr->created_at, memory_order_relaxed);
                if (!victim || created < oldest) {
                    victim = curr;
                    victim_prev = prev;
                    victim_idx = i;
                    oldest = created;
                }
            }
            prev = curr;
            curr = atomic_load_explicit(&curr->next, memory_order_acquire);
        }
    }

    if (!victim) return false;
    bdr_retire_entry(bdr, victim_idx, victim_prev, victim);
    return true;
}

static void bdr_check_ram(cwist_bdr_t *bdr) {
    if (atomic_load_explicit(&bdr->is_disk_mode, memory_order_relaxed)) return;

    if (cwist_is_ram_critical(CWIST_MIB(64))) {
        printf("[BDR] Low RAM. Switching to Disk Cache.\n");
        if (sqlite3_open("cwist_bdr_fallback.db", &bdr->disk_db) == SQLITE_OK) {
             char *err = NULL;
             sqlite3_exec(bdr->disk_db, "CREATE TABLE IF NOT EXISTS bdr (hash INTEGER PRIMARY KEY, blob BLOB);", NULL, NULL, &err);
             if (err) sqlite3_free(err);

             sqlite3_exec(bdr->disk_db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
             for (size_t i = 0; i < bdr->bucket_count; i++) {
                bdr_entry_t *curr = atomic_load_explicit(&bdr->buckets[i], memory_order_acquire);
                while (curr) {
                    bdr_entry_t *next = atomic_load_explicit(&curr->next, memory_order_acquire);
                    if (!atomic_load_explicit(&curr->retired, memory_order_relaxed) &&
                        atomic_load_explicit(&curr->is_stable, memory_order_relaxed)) {
                        bdr_blob_t *blob = atomic_load_explicit(&curr->blob, memory_order_acquire);
                        if (blob) {
                            sqlite3_stmt *stmt;
                            sqlite3_prepare_v2(bdr->disk_db, "INSERT INTO bdr (hash, blob) VALUES (?, ?);", -1, &stmt, NULL);
                            sqlite3_bind_int64(stmt, 1, curr->request_hash);
                            sqlite3_bind_blob(stmt, 2, blob->data, (int)blob->len, SQLITE_STATIC);
                            sqlite3_step(stmt);
                            sqlite3_finalize(stmt);
                        }
                    }
                    if (!atomic_load_explicit(&curr->retired, memory_order_relaxed)) {
                        atomic_store_explicit(&curr->retired, true, memory_order_release);
                        bdr_entry_publish(bdr, curr, NULL);
                        curr->retire_next = bdr->retired_entries;
                        bdr->retired_entries = curr;
                    }
                    curr = next;
                }
                atomic_store_explicit(&bdr->buckets[i], NULL, memory_order_release);
             }
             sqlite3_exec(bdr->disk_db, "COMMIT;", NULL, NULL, NULL);
             atomic_store_explicit(&bdr->is_disk_mode, true, memory_order_release);
        }
    }
}

static void bdr_janitor_tick(cwist_bdr_t *bdr) {
    uint64_t n = atomic_fetch_add_explicit(&bdr->put_count, 1, memory_order_relaxed) + 1;
    if (n % BDR_JANITOR_PERIOD != 0) return;
    if (pthread_mutex_trylock(&bdr->lock) != 0) return; /* another tick holds it */
    bdr_check_ram(bdr);
    bdr_sweep(bdr, BDR_GC_SWEEP);
    if (bdr->max_bytes > 0) {
        time_t now = time(NULL);
        while (atomic_load_explicit(&bdr->current_bytes, memory_order_relaxed) > bdr->max_bytes) {
            if (!bdr_trim_oldest(bdr, now)) break;
        }
    }
    pthread_mutex_unlock(&bdr->lock);
}

/* --- Lifecycle ------------------------------------------------------------ */

cwist_bdr_t *cwist_bdr_create(void) {
    cwist_bdr_t *bdr = cwist_alloc(sizeof(cwist_bdr_t));
    if (!bdr) return NULL;
    if (pthread_mutex_init(&bdr->lock, NULL) != 0) {
        cwist_free(bdr);
        return NULL;
    }
    bdr->bucket_count = BDR_BUCKETS;
    bdr->buckets = cwist_alloc_array(BDR_BUCKETS, sizeof(bdr_entry_t *));
    if (!bdr->buckets) {
        pthread_mutex_destroy(&bdr->lock);
        cwist_free(bdr);
        return NULL;
    }
    bdr->latency_threshold_ms = 10;
    atomic_init(&bdr->current_bytes, 0);
    bdr->max_bytes = BDR_DEFAULT_MAX_BYTES;
    bdr->max_entry_age_sec = BDR_DEFAULT_ENTRY_TTL;
    bdr->revalidate_hits = BDR_DEFAULT_REVALIDATE_HITS;
    bdr->gc_cursor = 0;
    atomic_init(&bdr->put_count, 0);
    bdr->retired_entries = NULL;
    bdr->disk_db = NULL;
    atomic_init(&bdr->is_disk_mode, false);
    return bdr;
}

void cwist_bdr_destroy(cwist_bdr_t *bdr) {
    if (!bdr) return;
    pthread_mutex_lock(&bdr->lock);
    for (size_t i = 0; i < bdr->bucket_count; i++) {
        bdr_entry_t *curr = bdr->buckets[i];
        while (curr) {
            bdr_entry_t *next = curr->next;
            if (curr == NULL) break;
            if(curr->blob != NULL) bdr_blob_free_cb(atomic_load_explicit(&curr->blob, memory_order_relaxed));
            cwist_free(curr);
            curr = next;
        }
    }
    while (bdr->retired_entries) {
        bdr_entry_t *next = bdr->retired_entries->retire_next;
        bdr_blob_free_cb(atomic_load_explicit(&bdr->retired_entries->blob, memory_order_relaxed));
        cwist_free(bdr->retired_entries);
        bdr->retired_entries = next;
    }
    cwist_free(bdr->buckets);
    if (bdr->disk_db) {
        sqlite3_close(bdr->disk_db);
        remove("cwist_bdr_fallback.db");
    }
    pthread_mutex_unlock(&bdr->lock);
    pthread_mutex_destroy(&bdr->lock);
    cwist_free(bdr);
}

/* --- Read path ------------------------------------------------------------ */

const void *cwist_bdr_get_pinned(cwist_bdr_t *bdr, const char *method, const char *path,
                                 size_t *out_len, bdr_blob_t **out_pin) {
    if (!bdr || !method || !path || !out_pin) return NULL;
    if (strcmp(method, "GET") != 0) return NULL;
    if (atomic_load_explicit(&bdr->is_disk_mode, memory_order_acquire)) return NULL;

    uint64_t req_h = bdr_hash(method, path);

    ttak_epoch_enter();
    bdr_entry_t *entry = bdr_find(bdr, req_h);
    if (!entry) {
        ttak_epoch_exit();
        return NULL;
    }
    atomic_fetch_add_explicit(&entry->hits, 1, memory_order_relaxed);

    /* Hit-time revalidation: a true report swaps in the callback's buffer
     * with one pointer exchange; the predecessor retires across an epoch. */
    cwist_bdr_revalidate_fn hook = entry->revalidate;
    if (hook) {
        void *fresh = NULL;
        size_t fresh_len = 0;
        void (*fresh_free)(void *) = NULL;
        if (hook(entry->revalidate_arg, &fresh, &fresh_len, &fresh_free) && fresh && fresh_len > 0) {
            bdr_blob_t *nb = bdr_blob_wrap(fresh, fresh_len, fresh_free);
            if (nb) {
                bdr_entry_publish(bdr, entry, nb);
            } else if (fresh_free) {
                fresh_free(fresh);
            }
        }
    }

    bdr_blob_t *blob = atomic_load_explicit(&entry->blob, memory_order_acquire);
    if (!blob || !atomic_load_explicit(&entry->is_stable, memory_order_acquire)) {
        ttak_epoch_exit();
        return NULL;
    }
    if (out_len) *out_len = blob->len;
    *out_pin = blob;
    return blob->data;
}

void cwist_bdr_unpin(bdr_blob_t *pin) {
    (void)pin; /* The pin is the epoch itself. */
    ttak_epoch_exit();
}

const void *cwist_bdr_get(cwist_bdr_t *bdr, const char *method, const char *path, size_t *out_len) {
    bdr_blob_t *pin = NULL;
    const void *data = cwist_bdr_get_pinned(bdr, method, path, out_len, &pin);
    if (pin) cwist_bdr_unpin(pin);
    return data;
}

void *cwist_bdr_copy_get(cwist_bdr_t *bdr, const char *method, const char *path, size_t *out_len) {
    bdr_blob_t *pin = NULL;
    size_t len = 0;
    const void *data = cwist_bdr_get_pinned(bdr, method, path, &len, &pin);
    void *copy = NULL;
    if (data && len > 0) {
        copy = cwist_alloc(len);
        if (copy) {
            memcpy(copy, data, len);
            if (out_len) *out_len = len;
        }
    }
    if (pin) cwist_bdr_unpin(pin);
    return copy;
}

/* --- Learn path (lock-free) ------------------------------------------------ */

static void cwist_bdr_put_disk(cwist_bdr_t *bdr, uint64_t req_h, const void *data, size_t len) {
    pthread_mutex_lock(&bdr->lock);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(bdr->disk_db, "INSERT OR REPLACE INTO bdr (hash, blob) VALUES (?, ?);", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)req_h);
    sqlite3_bind_blob(stmt, 2, data, (int)len, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&bdr->lock);
}

void cwist_bdr_put(cwist_bdr_t *bdr, const char *method, const char *path, const void *data, size_t len) {
    if (!bdr || !method || !path || !data || len == 0) return;
    if (strcmp(method, "GET") != 0) return;

    uint64_t req_h = bdr_hash(method, path);
    uint64_t res_h = bdr_hash_data(data, len);

    if (atomic_load_explicit(&bdr->is_disk_mode, memory_order_acquire)) {
        cwist_bdr_put_disk(bdr, req_h, data, len);
        return;
    }

    bdr_entry_t *entry = bdr_find_or_insert(bdr, req_h);
    if (!entry) return;

    if (atomic_load_explicit(&entry->is_stable, memory_order_acquire)) {
        /* Content changed under a stable entry: demote to candidate. */
        if (atomic_load_explicit(&entry->response_hash, memory_order_relaxed) != res_h) {
            bdr_entry_publish(bdr, entry, NULL);
            atomic_store_explicit(&entry->response_hash, res_h, memory_order_release);
            atomic_store_explicit(&entry->is_stable, false, memory_order_release);
            atomic_store_explicit(&entry->hits, 0, memory_order_relaxed);
            atomic_store_explicit(&entry->created_at, (int64_t)time(NULL), memory_order_relaxed);
        }
    } else {
        uint64_t expect = res_h;
        if (atomic_compare_exchange_strong_explicit(&entry->response_hash, &expect, res_h,
                                                    memory_order_acq_rel, memory_order_relaxed)) {
            /* Candidate reproduced the same bytes: stabilize.  A concurrent
             * stabilizer publishes an identical blob; the loser's duplicate
             * comes back from the exchange and is retired unused. */
            bdr_blob_t *blob = bdr_blob_learn(data, len);
            if (blob) {
                bdr_entry_publish(bdr, entry, blob);
                atomic_store_explicit(&entry->is_stable, true, memory_order_release);
                atomic_store_explicit(&entry->hits, 0, memory_order_relaxed);
                atomic_store_explicit(&entry->created_at, (int64_t)time(NULL), memory_order_relaxed);
            }
        } else {
            /* Different bytes: the entry now tracks the new candidate hash. */
            atomic_store_explicit(&entry->response_hash, res_h, memory_order_release);
        }
    }

    bdr_janitor_tick(bdr);
}

void cwist_bdr_put_fixed(cwist_bdr_t *bdr, const char *method, const char *path, const void *data, size_t len) {
    if (!bdr || !method || !path || !data || len == 0) return;
    if (strcmp(method, "GET") != 0) return;

    uint64_t req_h = bdr_hash(method, path);

    if (atomic_load_explicit(&bdr->is_disk_mode, memory_order_acquire)) {
        cwist_bdr_put_disk(bdr, req_h, data, len);
        return;
    }

    bdr_entry_t *entry = bdr_find_or_insert(bdr, req_h);
    if (!entry) return;

    bdr_blob_t *blob = bdr_blob_learn(data, len);
    if (!blob) return;
    atomic_store_explicit(&entry->response_hash, bdr_hash_data(data, len), memory_order_release);
    bdr_entry_publish(bdr, entry, blob);
    atomic_store_explicit(&entry->is_stable, true, memory_order_release);
    atomic_store_explicit(&entry->hits, 0, memory_order_relaxed);
    atomic_store_explicit(&entry->created_at, (int64_t)time(NULL), memory_order_relaxed);

    bdr_janitor_tick(bdr);
}

void cwist_bdr_put_revalidatable(cwist_bdr_t *bdr, const char *method, const char *path,
                                 const void *data, size_t len,
                                 cwist_bdr_revalidate_fn fn, void *arg) {
    if (!bdr || !method || !path || !data || len == 0 || !fn) return;
    if (strcmp(method, "GET") != 0) return;

    uint64_t req_h = bdr_hash(method, path);

    if (atomic_load_explicit(&bdr->is_disk_mode, memory_order_acquire)) {
        cwist_bdr_put_disk(bdr, req_h, data, len);
        return;
    }

    bdr_entry_t *entry = bdr_find_or_insert(bdr, req_h);
    if (!entry) return;

    /* Publish the hook before the blob so a reader never observes a
     * revalidatable blob without its hook. */
    entry->revalidate_arg = arg;
    atomic_store_explicit((_Atomic(cwist_bdr_revalidate_fn) *)&entry->revalidate, fn,
                          memory_order_release);

    bdr_blob_t *blob = bdr_blob_learn(data, len);
    if (!blob) return;
    atomic_store_explicit(&entry->response_hash, bdr_hash_data(data, len), memory_order_release);
    bdr_entry_publish(bdr, entry, blob);
    atomic_store_explicit(&entry->is_stable, true, memory_order_release);
    atomic_store_explicit(&entry->hits, 0, memory_order_relaxed);
    atomic_store_explicit(&entry->created_at, (int64_t)time(NULL), memory_order_relaxed);

    bdr_janitor_tick(bdr);
}

void cwist_bdr_set_limits(cwist_bdr_t *bdr, size_t max_bytes, time_t max_entry_age_sec, uint64_t revalidate_hits) {
    if (!bdr) return;
    pthread_mutex_lock(&bdr->lock);
    if (max_bytes > 0) bdr->max_bytes = max_bytes;
    if (max_entry_age_sec > 0) bdr->max_entry_age_sec = max_entry_age_sec;
    if (revalidate_hits > 0) bdr->revalidate_hits = revalidate_hits;
    pthread_mutex_unlock(&bdr->lock);
}
