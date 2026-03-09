#include <stdio.h>
#include <string.h>
#include <cwist/core/mem/alloc.h>

int main() {
    printf("=== Memory Allocation Tutorial ===\n");

    /* 1. Basic allocation — returns zeroed memory */
    printf("\n[cwist_alloc]\n");
    char *buf = cwist_alloc(64);
    if (!buf) {
        fprintf(stderr, "Allocation failed\n");
        return 1;
    }
    snprintf(buf, 64, "Hello from cwist_alloc!");
    printf("buf: '%s'\n", buf);

    /* 2. Realloc — resize an existing allocation */
    printf("\n[cwist_realloc]\n");
    buf = cwist_realloc(buf, 128);
    if (!buf) {
        fprintf(stderr, "Realloc failed\n");
        return 1;
    }
    strncat(buf, " (resized)", 128 - strlen(buf) - 1);
    printf("buf after realloc: '%s'\n", buf);

    /* 3. String duplication */
    printf("\n[cwist_strdup / cwist_strndup]\n");
    const char *original = "Duplicate me!";
    char *dup  = cwist_strdup(original);
    char *ndup = cwist_strndup(original, 9); /* "Duplicate" */
    printf("strdup:  '%s'\n", dup);
    printf("strndup: '%s'\n", ndup);

    /* 4. Array allocation — sizeof(int) * 8 elements, zeroed */
    printf("\n[cwist_alloc_array]\n");
    int *arr = cwist_alloc_array(8, sizeof(int));
    for (int i = 0; i < 8; i++) arr[i] = (i + 1) * 10;
    printf("arr: ");
    for (int i = 0; i < 8; i++) printf("%d ", arr[i]);
    printf("\n");

    /* 5. Free all allocations */
    cwist_free(buf);
    cwist_free(dup);
    cwist_free(ndup);
    cwist_free(arr);
    printf("\n=== Done ===\n");
    return 0;
}
