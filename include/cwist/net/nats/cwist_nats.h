#ifndef __CWIST_NATS_H__
#define __CWIST_NATS_H__

#include <cwist/sys/err/cwist_err.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cwist_nats cwist_nats_t;

typedef void (*cwist_nats_msg_cb)(const char *subject, const char *data, size_t len, void *ctx);

cwist_error_t cwist_nats_connect(cwist_nats_t **nats, const char *url);
cwist_error_t cwist_nats_subscribe(cwist_nats_t *nats, const char *subject, cwist_nats_msg_cb cb, void *ctx);
cwist_error_t cwist_nats_publish_string(cwist_nats_t *nats, const char *subject, const char *data);
void cwist_nats_dispatch(cwist_nats_t *nats);
void cwist_nats_destroy(cwist_nats_t *nats);

#ifdef __cplusplus
}
#endif

#endif
