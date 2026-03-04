#define _POSIX_C_SOURCE 200809L
#include <cwist/net/db_sync/db_sync.h>
#include <cwist/security/db_crypt/db_crypt.h>

#include <sqlite3.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Protocol constants
 * ---------------------------------------------------------------------- */

static const unsigned char SYNC_MAGIC[4] = { 0x43, 0x57, 0x53, 0x59 }; /* "CWSY" */

/* -------------------------------------------------------------------------
 * Internal I/O helpers (handle short reads/writes on TCP)
 * ---------------------------------------------------------------------- */

static int write_all(int fd, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) return -1;
        p   += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t len) {
    unsigned char *p = (unsigned char *)buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n <= 0) return -1;
        p   += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int cwist_db_sync_serve(sqlite3 *db, const cwist_db_crypt_ctx_t *ctx, int port) {
    if (!db || !ctx) return CWIST_DB_SYNC_ERR_ARGS;
    if (port <= 0) port = CWIST_DB_SYNC_DEFAULT_PORT;

    /* 1. Serialise the database to a byte buffer. */
    sqlite3_int64 db_size = 0;
    unsigned char *db_bytes = sqlite3_serialize(db, "main", &db_size, 0);
    if (!db_bytes || db_size <= 0) {
        fprintf(stderr, "[db_sync] serialize failed\n");
        return CWIST_DB_SYNC_ERR_MEM;
    }

    /* 2. Seal the bytes. */
    size_t blob_len = 0;
    unsigned char *blob = cwist_db_crypt_seal(ctx,
                                               db_bytes,
                                               (size_t)db_size,
                                               &blob_len);
    sqlite3_free(db_bytes);

    if (!blob) {
        fprintf(stderr, "[db_sync] seal failed\n");
        return CWIST_DB_SYNC_ERR_CRYPTO;
    }

    /* 3. Create a listening TCP socket. */
    int srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) {
        free(blob);
        return CWIST_DB_SYNC_ERR_NET;
    }

    int opt = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(srv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[db_sync] bind port %d failed: %s\n",
                port, strerror(errno));
        close(srv_fd);
        free(blob);
        return CWIST_DB_SYNC_ERR_NET;
    }

    if (listen(srv_fd, 1) < 0) {
        close(srv_fd);
        free(blob);
        return CWIST_DB_SYNC_ERR_NET;
    }

    /* 4. Accept one connection. */
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int cli_fd = accept(srv_fd, (struct sockaddr *)&client_addr, &client_len);
    close(srv_fd);

    if (cli_fd < 0) {
        free(blob);
        return CWIST_DB_SYNC_ERR_NET;
    }

    /* 5. Read client magic. */
    unsigned char magic_buf[4];
    if (read_all(cli_fd, magic_buf, 4) < 0 ||
        memcmp(magic_buf, SYNC_MAGIC, 4) != 0) {
        fprintf(stderr, "[db_sync] bad client magic\n");
        close(cli_fd);
        free(blob);
        return CWIST_DB_SYNC_ERR_PROTO;
    }

    /* 6. Send length (little-endian uint64) then blob. */
    unsigned char len_buf[8];
    for (int i = 0; i < 8; i++)
        len_buf[i] = (unsigned char)((uint64_t)blob_len >> (8 * i));
    if (write_all(cli_fd, len_buf, 8) < 0 ||
        write_all(cli_fd, blob, blob_len) < 0) {
        close(cli_fd);
        free(blob);
        return CWIST_DB_SYNC_ERR_NET;
    }

    close(cli_fd);
    free(blob);
    return CWIST_DB_SYNC_OK;
}

int cwist_db_sync_pull(const char *host, int port,
                       const cwist_db_crypt_ctx_t *ctx,
                       unsigned char **out_bytes, size_t *out_len) {
    if (!host || !ctx || !out_bytes || !out_len) return CWIST_DB_SYNC_ERR_ARGS;
    if (port <= 0) port = CWIST_DB_SYNC_DEFAULT_PORT;

    /* 1. Resolve host. */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        fprintf(stderr, "[db_sync] getaddrinfo failed for %s\n", host);
        return CWIST_DB_SYNC_ERR_NET;
    }

    /* 2. Connect. */
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return CWIST_DB_SYNC_ERR_NET;
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        fprintf(stderr, "[db_sync] connect to %s:%d failed: %s\n",
                host, port, strerror(errno));
        close(fd);
        freeaddrinfo(res);
        return CWIST_DB_SYNC_ERR_NET;
    }
    freeaddrinfo(res);

    /* 3. Send magic. */
    if (write_all(fd, SYNC_MAGIC, 4) < 0) {
        close(fd);
        return CWIST_DB_SYNC_ERR_NET;
    }

    /* 4. Read blob length (little-endian uint64). */
    unsigned char len_buf[8];
    if (read_all(fd, len_buf, 8) < 0) {
        close(fd);
        return CWIST_DB_SYNC_ERR_PROTO;
    }
    uint64_t blob_len_u64 = 0;
    for (int i = 0; i < 8; i++)
        blob_len_u64 |= ((uint64_t)len_buf[i]) << (8 * i);
    if (blob_len_u64 == 0) {
        close(fd);
        return CWIST_DB_SYNC_ERR_PROTO;
    }

    size_t blob_len = (size_t)blob_len_u64;
    unsigned char *blob = (unsigned char *)malloc(blob_len);
    if (!blob) {
        close(fd);
        return CWIST_DB_SYNC_ERR_MEM;
    }

    /* 5. Read blob. */
    if (read_all(fd, blob, blob_len) < 0) {
        free(blob);
        close(fd);
        return CWIST_DB_SYNC_ERR_NET;
    }
    close(fd);

    /* 6. Decrypt. */
    size_t pt_len = 0;
    unsigned char *pt = cwist_db_crypt_open(ctx, blob, blob_len, &pt_len);
    free(blob);

    if (!pt) {
        fprintf(stderr, "[db_sync] decrypt failed\n");
        return CWIST_DB_SYNC_ERR_CRYPTO;
    }

    *out_bytes = pt;
    *out_len   = pt_len;
    return CWIST_DB_SYNC_OK;
}
