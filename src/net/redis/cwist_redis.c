/**
 * @file cwist_redis.c
 * @brief Minimal RESP2 Redis client with connection pooling.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/net/redis/cwist_redis.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/sys/err/cwist_err.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#define CWIST_REDIS_BUF_SIZE 65536
#define CWIST_REDIS_LINE_MAX 8192

struct cwist_redis {
    int fd;
    char *host;
    int port;
    char recv_buf[CWIST_REDIS_BUF_SIZE];
    size_t recv_len;
    pthread_mutex_t mtx;
};

struct cwist_redis_pool {
    char *host;
    int port;
    size_t max_conns;
    cwist_redis_t **conns;
    bool *available;
    pthread_mutex_t mtx;
    pthread_cond_t cond;
};

/* --- Socket helpers ----------------------------------------------------- */

static int redis_open_socket(const char *host, int port) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int redis_send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int redis_recv_line(cwist_redis_t *r, char *line, size_t line_len) {
    size_t i = 0;
    while (1) {
        if (r->recv_len > 0) {
            while (r->recv_len > 0 && i < line_len - 1) {
                char c = r->recv_buf[0];
                /* simple shift */
                memmove(r->recv_buf, r->recv_buf + 1, r->recv_len - 1);
                r->recv_len--;
                if (c == '\n') {
                    if (i > 0 && line[i - 1] == '\r') i--;
                    line[i] = '\0';
                    return 0;
                }
                line[i++] = c;
            }
            if (i >= line_len - 1) return -1;
        }
        ssize_t n = recv(r->fd, r->recv_buf + r->recv_len,
                         CWIST_REDIS_BUF_SIZE - r->recv_len - 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        r->recv_len += (size_t)n;
    }
}

static int redis_recv_bytes(cwist_redis_t *r, char *out, size_t len) {
    size_t got = 0;
    while (got < len) {
        if (r->recv_len > 0) {
            size_t take = r->recv_len < (len - got) ? r->recv_len : (len - got);
            memcpy(out + got, r->recv_buf, take);
            memmove(r->recv_buf, r->recv_buf + take, r->recv_len - take);
            r->recv_len -= take;
            got += take;
            continue;
        }
        ssize_t n = recv(r->fd, r->recv_buf + r->recv_len,
                         CWIST_REDIS_BUF_SIZE - r->recv_len - 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        r->recv_len += (size_t)n;
    }
    return 0;
}

/* --- RESP2 helpers ------------------------------------------------------ */

static int write_bulk_string(cwist_sstring *out, const char *s) {
    char prefix[32];
    size_t len = s ? strlen(s) : 0;
    snprintf(prefix, sizeof(prefix), "$%zu\r\n", len);
    cwist_sstring_append(out, prefix);
    if (len) cwist_sstring_append(out, s);
    cwist_sstring_append(out, "\r\n");
    return 0;
}

static cwist_error_t read_reply(cwist_redis_t *r, char **out_value, size_t *out_len, char *out_type) {
    char line[CWIST_REDIS_LINE_MAX];
    if (out_len) *out_len = 0;
    if (redis_recv_line(r, line, sizeof(line)) != 0) return make_error(CWIST_ERR_INT16);
    if (out_type) *out_type = line[0];

    switch (line[0]) {
        case '+': /* simple string */
        case '-': /* error */
            if (out_value) {
                if (line[0] == '+') {
                    *out_value = cwist_strdup(line + 1);
                    if (out_len) *out_len = strlen(line + 1);
                } else {
                    *out_value = NULL;
                }
            }
            return (line[0] == '-') ? make_error(CWIST_ERR_INT16) : (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = 0};

        case ':': /* integer */
            if (out_value) *out_value = cwist_strdup(line + 1);
            if (out_len) *out_len = strlen(line + 1);
            return (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = 0};

        case '$': { /* bulk string */
            long long blen = atoll(line + 1);
            if (blen < 0) {
                if (out_value) *out_value = NULL;
                return (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = 0};
            }
            char *buf = cwist_alloc((size_t)blen + 1);
            if (!buf) return make_error(CWIST_ERR_INT16);
            if (redis_recv_bytes(r, buf, (size_t)blen) != 0) {
                cwist_free(buf);
                return make_error(CWIST_ERR_INT16);
            }
            buf[blen] = '\0';
            /* consume trailing \r\n */
            char crlf[2];
            if (redis_recv_bytes(r, crlf, 2) != 0) {
                cwist_free(buf);
                return make_error(CWIST_ERR_INT16);
            }
            if (out_value) *out_value = buf;
            else cwist_free(buf);
            if (out_len) *out_len = (size_t)blen;
            return (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = 0};
        }

        case '*': { /* array - skip for simple client */
            long long count = atoll(line + 1);
            for (long long i = 0; i < count; i++) {
                char *tmp = NULL;
                cwist_error_t e = read_reply(r, &tmp, NULL, NULL);
                cwist_free(tmp);
                if (e.error.err_i16 != 0) return e;
            }
            if (out_value) *out_value = NULL;
            return (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = 0};
        }

        default:
            return make_error(CWIST_ERR_INT16);
    }
}

static cwist_error_t redis_command_frame(cwist_redis_t *r, const char *frame, size_t frame_len,
                                         char **out_value, size_t *out_len) {
    if (!r || !frame) return make_error(CWIST_ERR_INT16);
    if (out_value) *out_value = NULL;
    /* A request/reply pair must be serialized as one critical section.  Keeping
     * send and receive separate allows two callers to consume each other's
     * replies on the same connection. */
    pthread_mutex_lock(&r->mtx);
    cwist_error_t err = redis_send_all(r->fd, frame, frame_len) == 0
        ? read_reply(r, out_value, out_len, NULL) : make_error(CWIST_ERR_INT16);
    pthread_mutex_unlock(&r->mtx);
    return err;
}

/* --- Connection lifecycle ----------------------------------------------- */

cwist_redis_t *cwist_redis_connect(const char *host, int port) {
    if (!host || port <= 0) return NULL;
    int fd = redis_open_socket(host, port);
    if (fd < 0) return NULL;

    cwist_redis_t *r = cwist_alloc(sizeof(*r));
    if (!r) {
        close(fd);
        return NULL;
    }
    r->fd = fd;
    r->host = cwist_strdup(host);
    r->port = port;
    r->recv_len = 0;
    pthread_mutex_init(&r->mtx, NULL);
    return r;
}

void cwist_redis_close(cwist_redis_t *r) {
    if (!r) return;
    /* shutdown wakes a subscriber blocked in recv before the descriptor is
     * released.  This is also the documented way to stop subscribe(). */
    if (r->fd >= 0) {
        shutdown(r->fd, SHUT_RDWR);
        close(r->fd);
        r->fd = -1;
    }
    cwist_free(r->host);
    pthread_mutex_destroy(&r->mtx);
    cwist_free(r);
}

/* --- Command helpers ---------------------------------------------------- */

static cwist_error_t recv_reply(cwist_redis_t *r, char **out_value) {
    if (!r) return make_error(CWIST_ERR_INT16);
    pthread_mutex_lock(&r->mtx);
    cwist_error_t err = read_reply(r, out_value, NULL, NULL);
    pthread_mutex_unlock(&r->mtx);
    return err;
}

static cwist_error_t send_command(cwist_redis_t *r, const char *cmd) {
    pthread_mutex_lock(&r->mtx);
    cwist_error_t err = redis_send_all(r->fd, cmd, strlen(cmd)) == 0
        ? (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = 0}
        : make_error(CWIST_ERR_INT16);
    pthread_mutex_unlock(&r->mtx);
    return err;
}

cwist_error_t cwist_redis_command(cwist_redis_t *r, const char *cmd, char **out) {
    if (!r || !cmd) return make_error(CWIST_ERR_INT16);
    char *value = NULL;
    cwist_error_t err = redis_command_frame(r, cmd, strlen(cmd), &value, NULL);
    if (out) *out = value; else cwist_free(value);
    return err;
}

cwist_error_t cwist_redis_command_argv(cwist_redis_t *r, size_t argc,
                                       const void *const *argv, const size_t *argv_lens,
                                       char **out, size_t *out_len) {
    if (!r || !argc || !argv || !argv_lens) return make_error(CWIST_ERR_INT16);
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    cwist_sstring *frame = cwist_sstring_create();
    if (!frame) return make_error(CWIST_ERR_INT16);
    char count[32];
    snprintf(count, sizeof(count), "*%zu\r\n", argc);
    if (cwist_sstring_append_len(frame, count, strlen(count)).error.err_i8) goto fail;
    for (size_t i = 0; i < argc; ++i) {
        if (!argv[i] && argv_lens[i]) goto fail;
        char len[32];
        snprintf(len, sizeof(len), "$%zu\r\n", argv_lens[i]);
        if (cwist_sstring_append_len(frame, len, strlen(len)).error.err_i8 ||
            (argv_lens[i] && cwist_sstring_append_len(frame, argv[i], argv_lens[i]).error.err_i8) ||
            cwist_sstring_append_len(frame, "\r\n", 2).error.err_i8) goto fail;
    }
    char *value = NULL;
    cwist_error_t err = redis_command_frame(r, frame->data, frame->size, &value, out_len);
    cwist_sstring_destroy(frame);
    if (out) *out = value; else cwist_free(value);
    return err;
fail:
    cwist_sstring_destroy(frame);
    return make_error(CWIST_ERR_INT16);
}

cwist_error_t cwist_redis_auth(cwist_redis_t *r, const char *username, const char *password) {
    if (!r || !password) return make_error(CWIST_ERR_INT16);
    const void *args[3] = { "AUTH", username, password };
    size_t lengths[3] = { 4, username ? strlen(username) : 0, strlen(password) };
    char *reply = NULL;
    cwist_error_t err = cwist_redis_command_argv(r, username ? 3 : 2, args, lengths, &reply, NULL);
    cwist_free(reply);
    return err;
}

cwist_error_t cwist_redis_select(cwist_redis_t *r, unsigned int database) {
    char db[16];
    snprintf(db, sizeof(db), "%u", database);
    const void *args[] = { "SELECT", db };
    size_t lengths[] = { 6, strlen(db) };
    char *reply = NULL;
    cwist_error_t err = cwist_redis_command_argv(r, 2, args, lengths, &reply, NULL);
    cwist_free(reply);
    return err;
}

cwist_error_t cwist_redis_get(cwist_redis_t *r, const char *key, char **out_value) {
    if (!r || !key || !out_value) return make_error(CWIST_ERR_INT16);
    cwist_sstring *cmd = cwist_sstring_create();
    if (!cmd) return make_error(CWIST_ERR_INT16);
    cwist_sstring_append(cmd, "*2\r\n");
    write_bulk_string(cmd, "GET");
    write_bulk_string(cmd, key);
    cwist_error_t err = cwist_redis_command(r, cmd->data, out_value);
    cwist_sstring_destroy(cmd);
    return err;
}

cwist_error_t cwist_redis_set(cwist_redis_t *r, const char *key, const char *value) {
    if (!r || !key || !value) return make_error(CWIST_ERR_INT16);
    cwist_sstring *cmd = cwist_sstring_create();
    if (!cmd) return make_error(CWIST_ERR_INT16);
    cwist_sstring_append(cmd, "*3\r\n");
    write_bulk_string(cmd, "SET");
    write_bulk_string(cmd, key);
    write_bulk_string(cmd, value);
    char *out = NULL;
    cwist_error_t err = cwist_redis_command(r, cmd->data, &out);
    cwist_free(out);
    cwist_sstring_destroy(cmd);
    return err;
}

cwist_error_t cwist_redis_setex(cwist_redis_t *r, const char *key, const char *value, int seconds) {
    if (!r || !key || !value || seconds <= 0) return make_error(CWIST_ERR_INT16);
    cwist_sstring *cmd = cwist_sstring_create();
    if (!cmd) return make_error(CWIST_ERR_INT16);
    cwist_sstring_append(cmd, "*4\r\n");
    write_bulk_string(cmd, "SETEX");
    write_bulk_string(cmd, key);
    char sec[32];
    snprintf(sec, sizeof(sec), "%d", seconds);
    write_bulk_string(cmd, sec);
    write_bulk_string(cmd, value);
    char *out = NULL;
    cwist_error_t err = cwist_redis_command(r, cmd->data, &out);
    cwist_free(out);
    cwist_sstring_destroy(cmd);
    return err;
}

cwist_error_t cwist_redis_del(cwist_redis_t *r, const char *key) {
    if (!r || !key) return make_error(CWIST_ERR_INT16);
    cwist_sstring *cmd = cwist_sstring_create();
    if (!cmd) return make_error(CWIST_ERR_INT16);
    cwist_sstring_append(cmd, "*2\r\n");
    write_bulk_string(cmd, "DEL");
    write_bulk_string(cmd, key);
    char *out = NULL;
    cwist_error_t err = cwist_redis_command(r, cmd->data, &out);
    cwist_free(out);
    cwist_sstring_destroy(cmd);
    return err;
}

cwist_error_t cwist_redis_expire(cwist_redis_t *r, const char *key, int seconds) {
    if (!r || !key || seconds <= 0) return make_error(CWIST_ERR_INT16);
    cwist_sstring *cmd = cwist_sstring_create();
    if (!cmd) return make_error(CWIST_ERR_INT16);
    cwist_sstring_append(cmd, "*3\r\n");
    write_bulk_string(cmd, "EXPIRE");
    write_bulk_string(cmd, key);
    char sec[32];
    snprintf(sec, sizeof(sec), "%d", seconds);
    write_bulk_string(cmd, sec);
    char *out = NULL;
    cwist_error_t err = cwist_redis_command(r, cmd->data, &out);
    cwist_free(out);
    cwist_sstring_destroy(cmd);
    return err;
}

cwist_error_t cwist_redis_publish(cwist_redis_t *r, const char *channel, const char *message) {
    if (!r || !channel || !message) return make_error(CWIST_ERR_INT16);
    cwist_sstring *cmd = cwist_sstring_create();
    if (!cmd) return make_error(CWIST_ERR_INT16);
    cwist_sstring_append(cmd, "*3\r\n");
    write_bulk_string(cmd, "PUBLISH");
    write_bulk_string(cmd, channel);
    write_bulk_string(cmd, message);
    char *out = NULL;
    cwist_error_t err = cwist_redis_command(r, cmd->data, &out);
    cwist_free(out);
    cwist_sstring_destroy(cmd);
    return err;
}

/* --- Pub/Sub ------------------------------------------------------------ */

cwist_error_t cwist_redis_subscribe(cwist_redis_t *r,
                                    const char **channels,
                                    cwist_redis_msg_cb cb,
                                    void *ctx) {
    if (!r || !channels || !channels[0] || !cb) return make_error(CWIST_ERR_INT16);

    cwist_sstring *cmd = cwist_sstring_create();
    if (!cmd) return make_error(CWIST_ERR_INT16);
    size_t count = 0;
    while (channels[count]) count++;
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "*%zu\r\n", count + 1);
    cwist_sstring_append(cmd, prefix);
    write_bulk_string(cmd, "SUBSCRIBE");
    for (size_t i = 0; i < count; i++) {
        write_bulk_string(cmd, channels[i]);
    }
    cwist_error_t err = send_command(r, cmd->data);
    cwist_sstring_destroy(cmd);
    if (err.error.err_i16 != 0) return err;

    /* Consume subscription confirmations. */
    for (size_t i = 0; i < count; i++) {
        char *out = NULL;
        err = recv_reply(r, &out);
        cwist_free(out);
        if (err.error.err_i16 != 0) return err;
    }

    /* Blocking read loop for messages. */
    while (1) {
        char line[CWIST_REDIS_LINE_MAX];
        if (redis_recv_line(r, line, sizeof(line)) != 0) break;
        if (line[0] != '*') continue;
        long long arr_count = atoll(line + 1);
        if (arr_count < 3) continue;

        char *type = NULL;
        if (read_reply(r, &type, NULL, NULL).error.err_i16 != 0) break;

        char *channel = NULL;
        if (read_reply(r, &channel, NULL, NULL).error.err_i16 != 0) {
            cwist_free(type);
            break;
        }

        char *message = NULL;
        if (read_reply(r, &message, NULL, NULL).error.err_i16 != 0) {
            cwist_free(type);
            cwist_free(channel);
            break;
        }

        if (type && strcmp(type, "message") == 0) {
            cb(channel, message, ctx);
        }
        cwist_free(type);
        cwist_free(channel);
        cwist_free(message);
    }
    return (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = 0};
}

/* --- Connection pool ---------------------------------------------------- */

cwist_redis_pool_t *cwist_redis_pool_create(const char *host, int port, size_t max_conns) {
    if (!host || port <= 0 || max_conns == 0) return NULL;

    cwist_redis_pool_t *pool = cwist_alloc(sizeof(*pool));
    if (!pool) return NULL;
    pool->host = cwist_strdup(host);
    pool->port = port;
    pool->max_conns = max_conns;
    pool->conns = cwist_alloc_array(max_conns, sizeof(cwist_redis_t *));
    pool->available = cwist_alloc_array(max_conns, sizeof(bool));
    if (!pool->host || !pool->conns || !pool->available) goto fail;

    for (size_t i = 0; i < max_conns; i++) {
        pool->conns[i] = cwist_redis_connect(host, port);
        if (!pool->conns[i]) goto fail;
        pool->available[i] = true;
    }

    pthread_mutex_init(&pool->mtx, NULL);
    pthread_cond_init(&pool->cond, NULL);
    return pool;

fail:
    if (pool) {
        for (size_t i = 0; i < max_conns && pool->conns; i++) {
            if (pool->conns[i]) cwist_redis_close(pool->conns[i]);
        }
        cwist_free(pool->conns);
        cwist_free(pool->available);
        cwist_free(pool->host);
        cwist_free(pool);
    }
    return NULL;
}

void cwist_redis_pool_destroy(cwist_redis_pool_t *pool) {
    if (!pool) return;
    pthread_mutex_lock(&pool->mtx);
    for (size_t i = 0; i < pool->max_conns; i++) {
        if (pool->conns[i]) cwist_redis_close(pool->conns[i]);
    }
    pthread_mutex_unlock(&pool->mtx);
    pthread_mutex_destroy(&pool->mtx);
    pthread_cond_destroy(&pool->cond);
    cwist_free(pool->conns);
    cwist_free(pool->available);
    cwist_free(pool->host);
    cwist_free(pool);
}

static cwist_redis_t *redis_pool_acquire(cwist_redis_pool_t *pool) {
    if (!pool) return NULL;
    pthread_mutex_lock(&pool->mtx);
    while (1) {
        for (size_t i = 0; i < pool->max_conns; i++) {
            if (pool->available[i]) {
                pool->available[i] = false;
                pthread_mutex_unlock(&pool->mtx);
                return pool->conns[i];
            }
        }
        pthread_cond_wait(&pool->cond, &pool->mtx);
    }
}

static void redis_pool_release(cwist_redis_pool_t *pool, cwist_redis_t *r) {
    if (!pool || !r) return;
    pthread_mutex_lock(&pool->mtx);
    for (size_t i = 0; i < pool->max_conns; i++) {
        if (pool->conns[i] == r) {
            pool->available[i] = true;
            pthread_cond_signal(&pool->cond);
            break;
        }
    }
    pthread_mutex_unlock(&pool->mtx);
}

cwist_error_t cwist_redis_pool_get(cwist_redis_pool_t *pool, const char *key, char **out_value) {
    cwist_redis_t *r = redis_pool_acquire(pool);
    if (!r) return make_error(CWIST_ERR_INT16);
    cwist_error_t err = cwist_redis_get(r, key, out_value);
    redis_pool_release(pool, r);
    return err;
}

cwist_error_t cwist_redis_pool_set(cwist_redis_pool_t *pool, const char *key, const char *value) {
    cwist_redis_t *r = redis_pool_acquire(pool);
    if (!r) return make_error(CWIST_ERR_INT16);
    cwist_error_t err = cwist_redis_set(r, key, value);
    redis_pool_release(pool, r);
    return err;
}

cwist_error_t cwist_redis_pool_setex(cwist_redis_pool_t *pool, const char *key, const char *value, int seconds) {
    cwist_redis_t *r = redis_pool_acquire(pool);
    if (!r) return make_error(CWIST_ERR_INT16);
    cwist_error_t err = cwist_redis_setex(r, key, value, seconds);
    redis_pool_release(pool, r);
    return err;
}

cwist_error_t cwist_redis_pool_del(cwist_redis_pool_t *pool, const char *key) {
    cwist_redis_t *r = redis_pool_acquire(pool);
    if (!r) return make_error(CWIST_ERR_INT16);
    cwist_error_t err = cwist_redis_del(r, key);
    redis_pool_release(pool, r);
    return err;
}

cwist_error_t cwist_redis_pool_publish(cwist_redis_pool_t *pool, const char *channel, const char *message) {
    cwist_redis_t *r = redis_pool_acquire(pool);
    if (!r) return make_error(CWIST_ERR_INT16);
    cwist_error_t err = cwist_redis_publish(r, channel, message);
    redis_pool_release(pool, r);
    return err;
}

cwist_error_t cwist_redis_pool_command_argv(cwist_redis_pool_t *pool, size_t argc,
                                            const void *const *argv, const size_t *argv_lens,
                                            char **out, size_t *out_len) {
    cwist_redis_t *r = redis_pool_acquire(pool);
    if (!r) return make_error(CWIST_ERR_INT16);
    cwist_error_t err = cwist_redis_command_argv(r, argc, argv, argv_lens, out, out_len);
    redis_pool_release(pool, r);
    return err;
}
