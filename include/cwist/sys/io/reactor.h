#ifndef __CWIST_SYS_IO_REACTOR_H__
#define __CWIST_SYS_IO_REACTOR_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*cwist_reactor_cb_t)(int fd, void *ctx);

typedef struct cwist_reactor cwist_reactor_t;

/* Per-event inline payload capacity. The reactor copies the caller's
 * payload into a pooled, zero-initialized slot at add/mod time, so the
 * callback never depends on caller stack or heap lifetime. */
#define CWIST_REACTOR_PAYLOAD_SIZE 64

cwist_reactor_t *cwist_reactor_create(void);
void cwist_reactor_destroy(cwist_reactor_t *reactor);

/* cb receives a pointer to the slot's inline payload (the copied bytes),
 * never NULL. Slots are zeroed on checkout, preserving calloc semantics. */
bool cwist_reactor_add(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb,
                       const void *payload, size_t payload_size);
bool cwist_reactor_mod(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb,
                       const void *payload, size_t payload_size);
bool cwist_reactor_del(cwist_reactor_t *reactor, int fd);

void cwist_reactor_run(cwist_reactor_t *reactor);
void cwist_reactor_stop(cwist_reactor_t *reactor);

#ifdef __cplusplus
}
#endif

#endif
