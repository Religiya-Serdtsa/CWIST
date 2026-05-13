#ifndef __CWIST_SYS_IO_REACTOR_H__
#define __CWIST_SYS_IO_REACTOR_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*cwist_reactor_cb_t)(int fd, void *ctx);

typedef struct cwist_reactor cwist_reactor_t;

cwist_reactor_t *cwist_reactor_create(void);
void cwist_reactor_destroy(cwist_reactor_t *reactor);

bool cwist_reactor_add(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb, void *ctx);
bool cwist_reactor_mod(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb, void *ctx);
bool cwist_reactor_del(cwist_reactor_t *reactor, int fd);

void cwist_reactor_run(cwist_reactor_t *reactor);
void cwist_reactor_stop(cwist_reactor_t *reactor);

#ifdef __cplusplus
}
#endif

#endif
