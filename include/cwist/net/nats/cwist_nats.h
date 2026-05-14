/**
 * @file cwist_nats.h
 * @brief NATS client interface.
 */

#ifndef __CWIST_NATS_H__
#define __CWIST_NATS_H__

#include <cwist/sys/err/cwist_err.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cwist_nats cwist_nats_t;

typedef void (*cwist_nats_msg_cb)(const char *subject, const char *data, size_t len, void *ctx);

/**
 * @brief Connect to a NATS server.
 */
cwist_error_t cwist_nats_connect(cwist_nats_t **nats, const char *url);

/**
 * @brief Subscribe to a subject.
 */
cwist_error_t cwist_nats_subscribe(cwist_nats_t *nats, const char *subject, cwist_nats_msg_cb cb, void *ctx);

/**
 * @brief Publish a string message.
 */
cwist_error_t cwist_nats_publish_string(cwist_nats_t *nats, const char *subject, const char *data);

/**
 * @brief Dispatch incoming messages.
 */
void cwist_nats_dispatch(cwist_nats_t *nats);

/**
 * @brief Disconnect and destroy the NATS handle.
 */
void cwist_nats_destroy(cwist_nats_t *nats);

#ifdef __cplusplus
}
#endif

#endif
