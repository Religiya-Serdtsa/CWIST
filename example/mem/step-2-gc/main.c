#include <stdio.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/mem/gc.h>

int main() {
    printf("=== Epoch GC Tutorial ===\n");

    /* 1. Initialise a GC context with manual epoch rotation */
    cwist_gc_t gc;
    cwist_gc(&gc, /*manual_rotation=*/true);
    printf("GC initialized (manual rotation mode)\n");

    /* 2. Allocate some buffers and register them with the GC */
    printf("\n[Register pointers]\n");
    void *p1 = cwist_alloc(256);
    void *p2 = cwist_alloc(512);
    cwist_reg_ptr(&gc, p1);
    cwist_reg_ptr_sized(&gc, p2, 512);
    printf("Registered p1 (256 B) and p2 (512 B)\n");

    /* 3. Manual epoch rotation — reclaims retired nodes */
    printf("\n[Manual rotation]\n");
    cwist_gc_rotate(&gc);
    printf("Epoch rotated; retired allocations reclaimed\n");

    /* 4. Allocate again and do another rotation */
    void *p3 = cwist_alloc(1024);
    cwist_reg_ptr_sized(&gc, p3, 1024);
    printf("Registered p3 (1024 B)\n");
    cwist_gc_rotate(&gc);
    printf("Second rotation complete\n");

    /* 5. Shut down — frees internal GC state */
    printf("\n[Shutdown]\n");
    cwist_gc_shutdown(&gc);
    printf("GC shut down\n");

    printf("\n=== Done ===\n");
    return 0;
}
