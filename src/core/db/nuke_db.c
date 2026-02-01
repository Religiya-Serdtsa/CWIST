#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <cwist/core/db/nuke_db.h>
#include <cwist/core/macros.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/sys/sys_info.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>

static cwist_nuke_db_t g_nuke = {0};
static pthread_t g_sync_thread = 0;
static volatile bool g_running = false;
static pthread_mutex_t g_nuke_lock = PTHREAD_MUTEX_INITIALIZER;
static sigset_t g_sigset;

static uint64_t nuke_estimate_required_ram(const char *disk_path) {
    struct stat st;
    uint64_t db_size = 0;
    if (disk_path && stat(disk_path, &st) == 0 && st.st_size > 0) {
        db_size = (uint64_t)st.st_size;
    }
    // Need room for the DB plus SQLite cache/overhead. Double the DB and add base buffer.
    uint64_t required = (db_size * 2) + CWIST_MIB(32);
    if (required < CWIST_MIB(64)) {
        required = CWIST_MIB(64);
    }
    return required;
}

static bool nuke_disk_has_tables(sqlite3 *db) {
    if (!db) return true;
    sqlite3_stmt *stmt = NULL;
    bool has_tables = true;
    if (sqlite3_prepare_v2(db,
                           "SELECT count(*) FROM sqlite_master WHERE type='table';",
                           -1,
                           &stmt,
                           NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            has_tables = sqlite3_column_int(stmt, 0) > 0;
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    return has_tables;
}

static bool nuke_integrity_ok(sqlite3 *db) {
    if (!db) return false;
    sqlite3_stmt *stmt = NULL;
    bool healthy = false;
    if (sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *state = sqlite3_column_text(stmt, 0);
            healthy = (state && strcmp((const char *)state, "ok") == 0);
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    return healthy;
}

// Helper: Backup source_db to dest_db
static int nuke_backup(sqlite3 *dest, sqlite3 *source) {
    if (!dest || !source) return -1;
    sqlite3_backup *backup = sqlite3_backup_init(dest, "main", source, "main");
    if (!backup) {
        fprintf(stderr, "[NukeDB] sqlite3_backup_init failed: %s\n", sqlite3_errmsg(dest));
        return -1;
    }

    int rc = sqlite3_backup_step(backup, -1); // Copy all pages
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "[NukeDB] Backup step failed: %d (%s)\n", rc, sqlite3_errmsg(dest));
    }

    sqlite3_backup_finish(backup);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

static void nuke_switch_to_disk(void) {
    if (g_nuke.is_disk_mode) return;

    printf("[NukeDB] CRITICAL: Low RAM detected (%zu bytes). Switching to Disk DB.\n", (size_t)cwist_get_available_ram());
    
    pthread_mutex_lock(&g_nuke_lock);
    
    // 1. Flush Memory -> Disk
    nuke_backup(g_nuke.disk_db, g_nuke.mem_db);
    
    // 2. Set mode flag
    sqlite3_db_release_memory(g_nuke.mem_db); 
    
    g_nuke.is_disk_mode = true;
    
    pthread_mutex_unlock(&g_nuke_lock);
}

// Internal commit hook to trigger immediate sync
static int nuke_commit_hook(void *arg) {
    CWIST_UNUSED(arg);
    if (g_running && !g_nuke.is_disk_mode && g_sync_thread != 0) {
        // Send signal to wake up sync thread
        pthread_kill(g_sync_thread, SIGUSR2);
    }
    return 0;
}

int cwist_nuke_sync(void) {
    pthread_mutex_lock(&g_nuke_lock);
    
    if (g_nuke.is_disk_mode) {
        if (g_nuke.disk_db) {
            sqlite3_wal_checkpoint_v2(g_nuke.disk_db, "main", SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
            pthread_mutex_unlock(&g_nuke_lock);
            return 0;
        }
        pthread_mutex_unlock(&g_nuke_lock);
        return -1;
    }

    // Safety: If initial load failed, don't overwrite disk with empty memory DB
    if (!g_nuke.load_successful) {
        pthread_mutex_unlock(&g_nuke_lock);
        return -1;
    }
    
    if (!g_nuke.mem_db || !g_nuke.disk_db) {
        pthread_mutex_unlock(&g_nuke_lock);
        return -1;
    }
    
    int rc = nuke_backup(g_nuke.disk_db, g_nuke.mem_db);
    pthread_mutex_unlock(&g_nuke_lock);
    return rc;
}

// Internal idempotent cleanup
static void nuke_cleanup_internal(void) {
    pthread_mutex_lock(&g_nuke_lock);
    
    // Close Memory DB
    if (g_nuke.mem_db) {
        sqlite3_close(g_nuke.mem_db);
        g_nuke.mem_db = NULL;
    }

    // Close Disk DB
    if (g_nuke.disk_db) {
        sqlite3_close(g_nuke.disk_db);
        g_nuke.disk_db = NULL;
    }

    if (g_nuke.disk_path) {
        cwist_free(g_nuke.disk_path);
        g_nuke.disk_path = NULL;
    }

    g_nuke.auto_sync = false;
    g_nuke.sync_interval_ms = 0;
    g_nuke.is_disk_mode = false;
    g_nuke.load_successful = false;

    pthread_mutex_unlock(&g_nuke_lock);
}

void cwist_nuke_close(void) {
    if (!g_running) return;
    g_running = false;

    // Wake up the sync thread if it's sleeping/waiting
    if (g_sync_thread != 0) {
        pthread_kill(g_sync_thread, SIGUSR1);
        pthread_join(g_sync_thread, NULL);
        g_sync_thread = 0;
    }

    cwist_nuke_sync();
    nuke_cleanup_internal();
}

static void *sync_thread_func(void *arg) {
    CWIST_UNUSED(arg);
    struct timespec timeout;
    int signum;

    while (g_running) {
        timeout.tv_sec = g_nuke.sync_interval_ms / 1000;
        timeout.tv_nsec = (g_nuke.sync_interval_ms % 1000) * 1000000;

        // Wait for signals (INT, TERM, USR1, USR2) or Timeout
        signum = sigtimedwait(&g_sigset, NULL, &timeout);

        if (signum > 0) {
            if (signum == SIGUSR1) {
                // Requested to stop via cwist_nuke_close
                break;
            }

            if (signum == SIGUSR2) {
                // Immediate sync requested via commit hook
                cwist_nuke_sync();
                continue;
            }
            
            // Handle termination signals (SIGINT, SIGTERM)
            printf("\n[NukeDB] Intercepted Signal %d. Saving data...\n", signum);
            
            cwist_nuke_sync();
            nuke_cleanup_internal();
            g_running = false;

            // Unblock the signal first
            sigset_t s;
            sigemptyset(&s);
            sigaddset(&s, signum);
            pthread_sigmask(SIG_UNBLOCK, &s, NULL);
            
            // Restore default handler and re-raise
            signal(signum, SIG_DFL);
            raise(signum);
            return NULL;
        } else { // Timeout (EAGAIN) or Interruption
            if (errno == EAGAIN && g_running && g_nuke.auto_sync) {
                if (cwist_is_ram_critical(CWIST_MIB(128))) {
                    nuke_switch_to_disk();
                } else {
                    cwist_nuke_sync();
                }
            }
        }
    }
    return NULL;
}

int cwist_nuke_init(const char *disk_path, int sync_interval_ms) {
    if (g_running) return CWIST_NUKE_ERR_GENERIC; // Already running
    if (!disk_path) return CWIST_NUKE_ERR_GENERIC;

    uint64_t required_ram = nuke_estimate_required_ram(disk_path);
    uint64_t available_ram = cwist_get_available_ram();
    if (available_ram > 0 && available_ram < required_ram) {
        fprintf(stderr,
                "[NukeDB] Insufficient RAM for in-memory mode (have %zu bytes, need ~%zu bytes).\n",
                (size_t)available_ram,
                (size_t)required_ram);
        return CWIST_NUKE_ERR_LOW_MEMORY;
    }

    g_nuke.disk_path = cwist_strdup(disk_path);
    if (!g_nuke.disk_path) {
        return CWIST_NUKE_ERR_GENERIC;
    }
    g_nuke.sync_interval_ms = sync_interval_ms > 0 ? sync_interval_ms : 1000;
    g_nuke.auto_sync = (sync_interval_ms > 0);
    g_nuke.is_disk_mode = false;
    g_nuke.load_successful = false;

    // 1. Open Disk DB
    int rc = sqlite3_open(disk_path, &g_nuke.disk_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[NukeDB] Failed to open disk DB: %s\n", sqlite3_errmsg(g_nuke.disk_db));
        nuke_cleanup_internal();
        return CWIST_NUKE_ERR_GENERIC;
    }

    if (!nuke_integrity_ok(g_nuke.disk_db)) {
        fprintf(stderr, "[NukeDB] Integrity check failed for '%s'. Aborting in-memory mode.\n", disk_path);
        nuke_cleanup_internal();
        return CWIST_NUKE_ERR_GENERIC;
    }

    sqlite3_exec(g_nuke.disk_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(g_nuke.disk_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

    // 2. Open Memory DB
    rc = sqlite3_open(":memory:", &g_nuke.mem_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[NukeDB] Failed to open memory DB: %s\n", sqlite3_errmsg(g_nuke.mem_db));
        nuke_cleanup_internal();
        return CWIST_NUKE_ERR_GENERIC;
    }

    // 3. Load Disk -> Memory
    pthread_mutex_lock(&g_nuke_lock);
    
    // Use sqlite3_backup as it is safer than raw deserialize for WAL/mode transitions
    int backup_rc = nuke_backup(g_nuke.mem_db, g_nuke.disk_db);
    if (backup_rc == 0) {
        g_nuke.load_successful = true;
    } else if (!nuke_disk_has_tables(g_nuke.disk_db)) {
        g_nuke.load_successful = true;
    } else {
        fprintf(stderr, "[NukeDB] Initial disk->memory load failed. Falling back to disk-only mode.\n");
        g_nuke.is_disk_mode = true;
        sqlite3_close(g_nuke.mem_db);
        g_nuke.mem_db = NULL;
    }
    pthread_mutex_unlock(&g_nuke_lock);

    // Register commit hook for immediate sync. Only needed while in-memory mode is active.
    if (g_nuke.mem_db && !g_nuke.is_disk_mode) {
        sqlite3_commit_hook(g_nuke.mem_db, nuke_commit_hook, NULL);
    }

    g_running = true;

    // 4. Setup Signal Interception
    sigemptyset(&g_sigset);
    sigaddset(&g_sigset, SIGINT);
    sigaddset(&g_sigset, SIGTERM);
    sigaddset(&g_sigset, SIGUSR1);
    sigaddset(&g_sigset, SIGUSR2);
    
    pthread_sigmask(SIG_BLOCK, &g_sigset, NULL);

    // 5. Start Sync/Signal Thread
    if (pthread_create(&g_sync_thread, NULL, sync_thread_func, NULL) != 0) {
        g_running = false;
        pthread_sigmask(SIG_UNBLOCK, &g_sigset, NULL);
        nuke_cleanup_internal();
        return CWIST_NUKE_ERR_GENERIC;
    }

    // 6. Register atexit for ultimate safety
    atexit(cwist_nuke_close);

    return CWIST_NUKE_OK;
}

sqlite3 *cwist_nuke_get_db(void) {
    if (g_nuke.is_disk_mode) return g_nuke.disk_db;
    return g_nuke.mem_db;
}

void cwist_nuke_signal_handler(int signum) {
    CWIST_UNUSED(signum);
}
