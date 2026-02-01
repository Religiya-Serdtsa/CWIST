#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/big_dumb_reply.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/siphash/siphash.h>
#include <cwist/sys/sys_info.h>
#include <cwist/core/macros.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#define BDR_BUCKETS 1024
#define BDR_GC_SWEEP 8
#define BDR_DEFAULT_MAX_BYTES CWIST_MIB(32)
#define BDR_DEFAULT_ENTRY_TTL 300
#define BDR_DEFAULT_REVALIDATE_HITS 100000

// SipHash key for BDR (Hardcoded or random at startup)
static const uint8_t BDR_KEY[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};

static void bdr_release_blob(cwist_bdr_t *bdr, bdr_entry_t *entry) {
    if (!entry || !entry->response_blob) return;
    if (bdr) {
        if (bdr->current_bytes >= entry->len) {
            bdr->current_bytes -= entry->len;
        } else {
            bdr->current_bytes = 0;
        }
    }
    cwist_free(entry->response_blob);
    entry->response_blob = NULL;
    entry->len = 0;
}

static void bdr_remove_entry(cwist_bdr_t *bdr, size_t idx, bdr_entry_t *prev, bdr_entry_t *entry) {
    if (!bdr || !entry || idx >= bdr->bucket_count) return;
    bdr_release_blob(bdr, entry);
    if (prev) {
        prev->next = entry->next;
    } else {
        bdr->buckets[idx] = entry->next;
    }
    cwist_free(entry);
}

static bool bdr_entry_should_decay(const cwist_bdr_t *bdr, const bdr_entry_t *entry, time_t now) {
    if (!bdr || !entry) return false;
    if (bdr->max_entry_age_sec > 0 && entry->created_at > 0) {
        if (now - entry->created_at > bdr->max_entry_age_sec) {
            return true;
        }
    }
    if (entry->is_stable && bdr->revalidate_hits > 0 && entry->hits >= bdr->revalidate_hits) {
        return true;
    }
    return false;
}

static bool bdr_trim_oldest(cwist_bdr_t *bdr, time_t now) {
    if (!bdr) return false;
    size_t victim_idx = SIZE_MAX;
    bdr_entry_t *victim = NULL;
    bdr_entry_t *victim_prev = NULL;
    time_t oldest = now;

    for (size_t i = 0; i < bdr->bucket_count; ++i) {
        bdr_entry_t *prev = NULL;
        bdr_entry_t *curr = bdr->buckets[i];
        while (curr) {
            if (curr->response_blob && (!victim || curr->created_at < oldest)) {
                victim = curr;
                victim_prev = prev;
                victim_idx = i;
                oldest = curr->created_at;
            }
            prev = curr;
            curr = curr->next;
        }
    }

    if (!victim || victim_idx == SIZE_MAX) return false;
    bdr_remove_entry(bdr, victim_idx, victim_prev, victim);
    return true;
}

static void bdr_sweep(cwist_bdr_t *bdr, size_t steps) {
    if (!bdr || bdr->bucket_count == 0 || steps == 0) return;
    time_t now = time(NULL);
    for (size_t i = 0; i < steps; ++i) {
        size_t idx = bdr->gc_cursor % bdr->bucket_count;
        bdr->gc_cursor = (bdr->gc_cursor + 1) % bdr->bucket_count;
        bdr_entry_t *prev = NULL;
        bdr_entry_t *curr = bdr->buckets[idx];
        while (curr) {
            if (bdr_entry_should_decay(bdr, curr, now)) {
                bdr_entry_t *victim = curr;
                curr = curr->next;
                bdr_remove_entry(bdr, idx, prev, victim);
                continue;
            }
            prev = curr;
            curr = curr->next;
        }
    }
}

static void bdr_guardrails(cwist_bdr_t *bdr) {
    if (!bdr) return;
    bdr_sweep(bdr, BDR_GC_SWEEP);

    if (bdr->max_bytes == 0) return;
    time_t now = time(NULL);
    while (bdr->current_bytes > bdr->max_bytes) {
        if (!bdr_trim_oldest(bdr, now)) break;
    }
}

cwist_bdr_t *cwist_bdr_create(void) {
    cwist_bdr_t *bdr = cwist_alloc(sizeof(cwist_bdr_t));
    if (!bdr) return NULL;
    bdr->bucket_count = BDR_BUCKETS;
    bdr->buckets = cwist_alloc_array(BDR_BUCKETS, sizeof(bdr_entry_t *));
    bdr->latency_threshold_ms = 10; 
    bdr->current_bytes = 0;
    bdr->max_bytes = BDR_DEFAULT_MAX_BYTES;
    bdr->max_entry_age_sec = BDR_DEFAULT_ENTRY_TTL;
    bdr->revalidate_hits = BDR_DEFAULT_REVALIDATE_HITS;
    bdr->gc_cursor = 0;
    bdr->disk_db = NULL;
    bdr->is_disk_mode = false;
    return bdr;
}

void cwist_bdr_destroy(cwist_bdr_t *bdr) {
    if (!bdr) return;
    for (size_t i = 0; i < bdr->bucket_count; i++) {
        bdr_entry_t *curr = bdr->buckets[i];
        while (curr) {
            bdr_entry_t *next = curr->next;
            bdr_release_blob(bdr, curr);
            cwist_free(curr);
            curr = next;
        }
    }
    cwist_free(bdr->buckets);
    if (bdr->disk_db) {
        sqlite3_close(bdr->disk_db);
        remove("cwist_bdr_fallback.db"); // Cleanup temp db
    }
    cwist_free(bdr);
}

static uint64_t bdr_hash(const char *method, const char *path) {
    uint64_t h = siphash24((const void*)path, strlen(path), BDR_KEY);
    h ^= (uint64_t)(method[0]); 
    return h;
}

static void bdr_check_ram(cwist_bdr_t *bdr) {
    if (bdr->is_disk_mode) return;
    
    // Threshold: 64MB free (conservative)
    if (cwist_is_ram_critical(CWIST_MIB(64))) {
        printf("[BDR] Low RAM. Switching to Disk Cache.\n");
        // Open Disk DB
        if (sqlite3_open("cwist_bdr_fallback.db", &bdr->disk_db) == SQLITE_OK) {
             char *err = NULL;
             sqlite3_exec(bdr->disk_db, "CREATE TABLE IF NOT EXISTS bdr (hash INTEGER PRIMARY KEY, blob BLOB);", NULL, NULL, &err);
             if (err) sqlite3_free(err);
             
             // Move existing memory items to disk
             sqlite3_exec(bdr->disk_db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
             for (size_t i = 0; i < bdr->bucket_count; i++) {
                bdr_entry_t *curr = bdr->buckets[i];
                while (curr) {
                    if (curr->is_stable) { // Only move stable items
                        sqlite3_stmt *stmt;
                        sqlite3_prepare_v2(bdr->disk_db, "INSERT INTO bdr (hash, blob) VALUES (?, ?);", -1, &stmt, NULL);
                        sqlite3_bind_int64(stmt, 1, curr->request_hash);
                        sqlite3_bind_blob(stmt, 2, curr->response_blob, curr->len, SQLITE_STATIC);
                        sqlite3_step(stmt);
                        sqlite3_finalize(stmt);
                    }
                    
                    // Free memory
                    bdr_entry_t *next = curr->next;
                    bdr_release_blob(bdr, curr);
                    cwist_free(curr);
                    curr = next;
                }
                bdr->buckets[i] = NULL;
             }
             sqlite3_exec(bdr->disk_db, "COMMIT;", NULL, NULL, NULL);
             bdr->is_disk_mode = true;
        }
    }
}

static uint64_t bdr_hash_data(const void *data, size_t len) {

    return siphash24(data, len, BDR_KEY);

}



const void *cwist_bdr_get(cwist_bdr_t *bdr, const char *method, const char *path, size_t *out_len) {

    if (!bdr || !method || !path) return NULL;

    if (strcmp(method, "GET") != 0) return NULL;



    uint64_t h = bdr_hash(method, path);



    // Disk Mode

    if (bdr->is_disk_mode) {

        // Disk mode logic remains same (Fail-safe: return NULL or implement read)

        // For now, we stick to "Write-only" on disk for safety as per previous step.

        return NULL; 

    }



    // Memory Mode

    size_t idx = h % bdr->bucket_count;

    bdr_entry_t *prev = NULL;
    bdr_entry_t *curr = bdr->buckets[idx];

    while (curr) {

        if (curr->request_hash == h) {

            curr->hits++;

            if (bdr_entry_should_decay(bdr, curr, time(NULL))) {
                bdr_remove_entry(bdr, idx, prev, curr);
                return NULL;
            }

            if (curr->is_stable && curr->response_blob) {

                if (out_len) *out_len = curr->len;

                return curr->response_blob;

            }

            return NULL; // Found but not stable yet

        }

        prev = curr;
        curr = curr->next;

    }

    return NULL;

}



void cwist_bdr_put(cwist_bdr_t *bdr, const char *method, const char *path, const void *data, size_t len) {

    if (!bdr || !method || !path || !data || len == 0) return;

    if (strcmp(method, "GET") != 0) return;



    // Check RAM health before adding

    bdr_check_ram(bdr);



    uint64_t req_h = bdr_hash(method, path);

    uint64_t res_h = bdr_hash_data(data, len);



    if (bdr->is_disk_mode) {

         // In disk mode, we assume stability check is too expensive or we just dump.

         // Actually, to respect "Stability Check" even on disk, we need read-modify-write.

         // But disk is fallback. Let's just update.

         // Or simpler: Disk mode = Emergency. Just save it.

         sqlite3_stmt *stmt;

         sqlite3_prepare_v2(bdr->disk_db, "INSERT OR REPLACE INTO bdr (hash, blob) VALUES (?, ?);", -1, &stmt, NULL);

         sqlite3_bind_int64(stmt, 1, req_h);

         sqlite3_bind_blob(stmt, 2, data, len, SQLITE_STATIC);

         sqlite3_step(stmt);

         sqlite3_finalize(stmt);

         bdr_guardrails(bdr);

         return;

    }



    size_t idx = req_h % bdr->bucket_count;

    

    // Check exist

    bdr_entry_t *curr = bdr->buckets[idx];

    while (curr) {

        if (curr->request_hash == req_h) {

            // Entry exists. Check stability.

            if (curr->is_stable) {

                // Already stable. 

                // Optional: Re-verify occasionally? For now, assume "Big Dumb" means permanent.

                // If we want to detect changes:

                if (curr->response_hash != res_h) {

                    // Changed! Invalidated.

                    // Downgrade to unstable? Or update immediately?

                    // "Only cache if totally matching".

                    // If it changed, it's not dumb-cacheable.

                    // Evict it.

                    curr->is_stable = false;

                    curr->hits = 0;

                    bdr_release_blob(bdr, curr);

                    curr->response_hash = res_h; // New candidate

                    curr->created_at = time(NULL);

                }

            } else {

                // Was unstable/candidate. Check if matches candidate.

                if (curr->response_hash == res_h) {

                    // Match! Stabilize.

                    void *blob = cwist_alloc(len);

                    if (blob) {

                        memcpy(blob, data, len);

                        bdr_release_blob(bdr, curr);

                        curr->response_blob = blob;

                        curr->len = len;

                        curr->is_stable = true;

                        curr->hits = 0;

                        curr->created_at = time(NULL);

                        bdr->current_bytes += len;

                        bdr_guardrails(bdr);

                        // printf("[BDR] Stabilized: %s\n", path);

                    }

                } else {

                    // Mismatch. Keep unstable, update candidate.

                    curr->response_hash = res_h;

                }

            }

            return;

        }

        curr = curr->next;

    }

    

    // New Entry (Candidate)

    bdr_entry_t *entry = cwist_alloc(sizeof(bdr_entry_t));

    if (!entry) return;



    entry->request_hash = req_h;

    entry->response_hash = res_h;

    entry->is_stable = false; // Start as candidate

    entry->response_blob = NULL;

    entry->len = 0;

    entry->hits = 0;

    entry->created_at = time(NULL);

    

    entry->next = bdr->buckets[idx];

    bdr->buckets[idx] = entry;

    bdr_guardrails(bdr);

}

void cwist_bdr_set_limits(cwist_bdr_t *bdr, size_t max_bytes, time_t max_entry_age_sec, uint64_t revalidate_hits) {

    if (!bdr) return;

    if (max_bytes > 0) {

        bdr->max_bytes = max_bytes;

    }

    if (max_entry_age_sec > 0) {

        bdr->max_entry_age_sec = max_entry_age_sec;

    }

    if (revalidate_hits > 0) {

        bdr->revalidate_hits = revalidate_hits;

    }

    bdr_guardrails(bdr);

}
