#ifndef __TTAK_MEM_EPOCH_GC_H__
#define __TTAK_MEM_EPOCH_GC_H__

#include <stddef.h>
#include <stdlib.h>

typedef void (*ttak_epoch_gc_free_fn)(void *);

typedef struct ttak_epoch_gc {
    void *dummy;
} ttak_epoch_gc_t;

void ttak_epoch_gc_init(ttak_epoch_gc_t *gc, ttak_epoch_gc_free_fn free_fn, void *ctx);
void ttak_epoch_gc_shutdown(ttak_epoch_gc_t *gc);
void ttak_epoch_gc_rotate(ttak_epoch_gc_t *gc);
void ttak_epoch_gc_retire(ttak_epoch_gc_t *gc, void *ptr);

#endif
