/**
 * @file cwist_redis.h
 * @brief Minimal in-tree Redis client (RESP2) with connection pooling.
 */

#ifndef __CWIST_REDIS_H__
#define __CWIST_REDIS_H__

#include <cwist/sys/err/cwist_err.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Single Redis connection handle.
 */
typedef struct cwist_redis cwist_redis_t;

/**
 * @brief Redis connection pool handle.
 */
typedef struct cwist_redis_pool cwist_redis_pool_t;

/**
 * @brief Pub/sub message callback.
 */
typedef void (*cwist_redis_msg_cb)(const char *channel, const char *message, void *ctx);

/**
 * @brief Open a TCP connection to a Redis server.
 *
 * @param host Hostname or IP address.
 * @param port TCP port.
 * @return Connection handle, or NULL on failure.
 */
cwist_redis_t *cwist_redis_connect(const char *host, int port);

/**
 * @brief Close a Redis connection.
 */
void cwist_redis_close(cwist_redis_t *r);

/**
 * @brief Execute a raw Redis command and return the bulk reply.
 *
 * @param r        Connection.
 * @param cmd      Space-separated command string (e.g. "GET foo").
 * @param out      If non-NULL and reply is a string, receives a heap copy. Caller frees with cwist_free().
 * @return 0 on success, -1 on error.
 */
cwist_error_t cwist_redis_command(cwist_redis_t *r, const char *cmd, char **out);

/** Execute a binary-safe Redis command expressed as explicit arguments.
 * Reply strings are allocated with cwist_alloc() and may contain NUL bytes;
 * use @p out_len to obtain their exact length. */
cwist_error_t cwist_redis_command_argv(cwist_redis_t *r, size_t argc,
                                       const void *const *argv, const size_t *argv_lens,
                                       char **out, size_t *out_len);

/** Authenticate/select a logical Redis database on this connection. */
cwist_error_t cwist_redis_auth(cwist_redis_t *r, const char *username, const char *password);
cwist_error_t cwist_redis_select(cwist_redis_t *r, unsigned int database);

/**
 * @brief Convenience commands.
 * @{ */
cwist_error_t cwist_redis_get(cwist_redis_t *r, const char *key, char **out_value);
cwist_error_t cwist_redis_set(cwist_redis_t *r, const char *key, const char *value);
cwist_error_t cwist_redis_setex(cwist_redis_t *r, const char *key, const char *value, int seconds);
cwist_error_t cwist_redis_del(cwist_redis_t *r, const char *key);
cwist_error_t cwist_redis_expire(cwist_redis_t *r, const char *key, int seconds);
cwist_error_t cwist_redis_publish(cwist_redis_t *r, const char *channel, const char *message);
/** @} */

/**
 * @brief Subscribe to channels on a dedicated connection.
 *
 * This call blocks the current thread and invokes @p cb for each message.
 * Use it inside a background thread. To stop, call cwist_redis_close() from
 * another thread (the read loop will exit on socket error).
 *
 * @param r         Connection. Must not be shared with other commands.
 * @param channels  NULL-terminated array of channel names.
 * @param cb        Message callback.
 * @param ctx       User context forwarded to callback.
 * @return 0 when stopped cleanly, -1 on error.
 */
cwist_error_t cwist_redis_subscribe(cwist_redis_t *r,
                                    const char **channels,
                                    cwist_redis_msg_cb cb,
                                    void *ctx);

/**
 * @brief Create a Redis connection pool.
 *
 * @param host      Redis host.
 * @param port      Redis port.
 * @param max_conns Maximum connections in pool.
 * @return Pool handle, or NULL on failure.
 */
cwist_redis_pool_t *cwist_redis_pool_create(const char *host, int port, size_t max_conns);

/**
 * @brief Destroy the pool and close all connections.
 */
void cwist_redis_pool_destroy(cwist_redis_pool_t *pool);

/**
 * @brief Pooled convenience commands.
 * @{ */
cwist_error_t cwist_redis_pool_get(cwist_redis_pool_t *pool, const char *key, char **out_value);
cwist_error_t cwist_redis_pool_set(cwist_redis_pool_t *pool, const char *key, const char *value);
cwist_error_t cwist_redis_pool_setex(cwist_redis_pool_t *pool, const char *key, const char *value, int seconds);
cwist_error_t cwist_redis_pool_del(cwist_redis_pool_t *pool, const char *key);
cwist_error_t cwist_redis_pool_publish(cwist_redis_pool_t *pool, const char *channel, const char *message);
cwist_error_t cwist_redis_pool_command_argv(cwist_redis_pool_t *pool, size_t argc,
                                            const void *const *argv, const size_t *argv_lens,
                                            char **out, size_t *out_len);
/** @} */

#ifdef __cplusplus
}
#endif

#endif /* __CWIST_REDIS_H__ */
