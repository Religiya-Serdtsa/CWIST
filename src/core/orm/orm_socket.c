/**
 * @file orm_socket.c
 * @brief SQLite-to-socket bridge implementation.
 *
 * This file is the only place in the ORM subsystem that directly
 * links against SQLite3.  It exposes the database as a byte stream
 * over a local Unix socket pair so that cwist_orm_t remains fully
 * decoupled from sqlite3.h.
 */

#define _GNU_SOURCE
#include <cwist/core/orm/orm_socket.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <stdint.h>

#include <sqlite3.h>
#include <cjson/cJSON.h>

/* ------------------------------------------------------------------------- */
/* Protocol constants                                                        */
/* ------------------------------------------------------------------------- */

/** @brief Magic value placed at the start of every response. */
#define ORM_PROTO_MAGIC 0x43574F52   /* "CWOR" in ASCII hex trickery */

/* ------------------------------------------------------------------------- */
/* Worker context                                                            */
/* ------------------------------------------------------------------------- */

/**
 * @brief Bundle passed to the background worker thread.
 */
typedef struct {
    int      fd;       /**< Server-side socket descriptor. */
    char    *db_path;  /**< SQLite database path (heap-allocated). */
} cwist_orm_worker_ctx_t;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

/**
 * @brief Receive exactly @p n bytes from @p fd into @p buf.
 * @return 0 on success, -1 on EOF or error.
 */
static int recv_all(int fd, void *buf, size_t n)
{
    size_t total = 0;
    char *p = (char *)buf;
    while (total < n) {
        ssize_t r = recv(fd, p + total, n - total, MSG_WAITALL);
        if (r <= 0) return -1;
        total += (size_t)r;
    }
    return 0;
}

/**
 * @brief Send exactly @p n bytes from @p buf to @p fd.
 * @return 0 on success, -1 on error.
 */
static int send_all(int fd, const void *buf, size_t n)
{
    size_t total = 0;
    const char *p = (const char *)buf;
    while (total < n) {
        ssize_t w = send(fd, p + total, n - total, 0);
        if (w <= 0) return -1;
        total += (size_t)w;
    }
    return 0;
}

/**
 * @brief Free a heap-allocated worker context.
 */
static void free_worker_ctx(cwist_orm_worker_ctx_t *ctx)
{
    if (!ctx) return;
    free(ctx->db_path);
    free(ctx);
}

/* ------------------------------------------------------------------------- */
/* SQLite callback context                                                   */
/* ------------------------------------------------------------------------- */

/**
 * @brief Accumulator used by sqlite3_exec callback to build a JSON array.
 */
typedef struct {
    cJSON *rows;   /**< cJSON array receiving each row object. */
} query_accumulator_t;

/**
 * @brief sqlite3_exec callback that packs rows into a cJSON array.
 */
static int socket_query_callback(void *data, int argc, char **argv,
                                 char **azColName)
{
    query_accumulator_t *acc = (query_accumulator_t *)data;
    cJSON *row = cJSON_CreateObject();
    if (!row) return 1; /* abort */

    for (int i = 0; i < argc; i++) {
        if (argv[i]) {
            /* Attempt numeric conversion for cleaner JSON. */
            char *endptr = NULL;
            errno = 0;
            long long ll = strtoll(argv[i], &endptr, 10);
            if (endptr && *endptr == '\0' && errno == 0) {
                cJSON_AddNumberToObject(row, azColName[i], (double)ll);
            } else {
                double d = strtod(argv[i], &endptr);
                if (endptr && *endptr == '\0' && errno == 0) {
                    cJSON_AddNumberToObject(row, azColName[i], d);
                } else {
                    cJSON_AddStringToObject(row, azColName[i], argv[i]);
                }
            }
        } else {
            cJSON_AddNullToObject(row, azColName[i]);
        }
    }

    cJSON_AddItemToArray(acc->rows, row);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Worker main loop                                                          */
/* ------------------------------------------------------------------------- */

/**
 * @brief Background thread that owns the SQLite connection.
 *
 * Reads framed SQL requests from the socket, executes them, and writes
 * framed JSON responses back.
 */
static void *cwist_orm_socket_worker(void *arg)
{
    cwist_orm_worker_ctx_t *ctx = (cwist_orm_worker_ctx_t *)arg;
    if (!ctx) return NULL;

    sqlite3 *db = NULL;
    int rc = sqlite3_open(ctx->db_path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[ORM-SOCKET] sqlite3_open failed: %s\n",
                sqlite3_errmsg(db));
        sqlite3_close(db);
        close(ctx->fd);
        free_worker_ctx(ctx);
        return NULL;
    }

    /* Speed up concurrent access semantics for single-writer scenarios. */
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

    for (;;) {
        /* ---- read request length (4 bytes, big-endian) ---- */
        uint32_t net_len = 0;
        if (recv_all(ctx->fd, &net_len, sizeof(net_len)) != 0) break;
        uint32_t sql_len = ntohl(net_len);
        if (sql_len == 0 || sql_len > 16 * 1024 * 1024) break; /* sanity */

        /* ---- read SQL text ---- */
        char *sql = (char *)malloc(sql_len + 1);
        if (!sql) break;
        if (recv_all(ctx->fd, sql, sql_len) != 0) {
            free(sql);
            break;
        }
        sql[sql_len] = '\0';

        /* ---- execute ---- */
        query_accumulator_t acc;
        acc.rows = cJSON_CreateArray();
        char *errmsg = NULL;

        rc = sqlite3_exec(db, sql, socket_query_callback, &acc, &errmsg);
        free(sql);

        /* ---- build response payload ---- */
        cJSON *resp = cJSON_CreateObject();
        if (!resp) {
            cJSON_Delete(acc.rows);
            if (errmsg) sqlite3_free(errmsg);
            break;
        }

        cJSON_AddNumberToObject(resp, "status", (double)rc);
        if (errmsg) {
            cJSON_AddStringToObject(resp, "error", errmsg);
            sqlite3_free(errmsg);
        } else {
            cJSON_AddNullToObject(resp, "error");
        }
        cJSON_AddItemToObject(resp, "rows", acc.rows);

        char *payload = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (!payload) break;

        uint32_t payload_len = (uint32_t)strlen(payload);
        if (payload_len > UINT32_MAX - 1) {
            free(payload);
            break;
        }

        /* ---- send framed response ---- */
        uint32_t net_payload = htonl(payload_len);
        if (send_all(ctx->fd, &net_payload, sizeof(net_payload)) != 0) {
            free(payload);
            break;
        }
        if (send_all(ctx->fd, payload, payload_len) != 0) {
            free(payload);
            break;
        }
        free(payload);
    }

    sqlite3_close(db);
    close(ctx->fd);
    free_worker_ctx(ctx);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

int cwist_db_transfer_sqlite_to_socket(const char *db_path)
{
    if (!db_path) {
        errno = EINVAL;
        return -1;
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == -1) {
        return -1;
    }

    cwist_orm_worker_ctx_t *ctx =
        (cwist_orm_worker_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        close(sv[0]);
        close(sv[1]);
        return -1;
    }

    ctx->fd = sv[1];
    ctx->db_path = strdup(db_path);
    if (!ctx->db_path) {
        free(ctx);
        close(sv[0]);
        close(sv[1]);
        return -1;
    }

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    int rc = pthread_create(&tid, &attr, cwist_orm_socket_worker, ctx);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        free_worker_ctx(ctx);
        close(sv[0]);
        close(sv[1]);
        errno = rc;
        return -1;
    }

    return sv[0]; /* caller owns this end */
}
