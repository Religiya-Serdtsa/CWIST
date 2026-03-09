#include <stdio.h>
#include <cwist/core/sstring/sstring.h>

int main() {
    printf("=== SString: Compare & Copy ===\n");

    cwist_sstring *a = cwist_sstring_create();
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring *c = cwist_sstring_create();

    if (!a || !b || !c) {
        fprintf(stderr, "Failed to allocate strings\n");
        return 1;
    }

    /* 1. Assign values */
    cwist_sstring_assign(a, (char *)"Hello, World!");
    cwist_sstring_assign(b, (char *)"Hello, World!");
    cwist_sstring_assign(c, (char *)"Hello, CWIST!");

    /* 2. Compare two sstrings */
    printf("\n[Compare sstring vs sstring]\n");
    int cmp_ab = cwist_sstring_compare_sstring(a, b);
    int cmp_ac = cwist_sstring_compare_sstring(a, c);
    printf("a == b? %s  (cmp=%d)\n", cmp_ab == 0 ? "yes" : "no", cmp_ab);
    printf("a == c? %s  (cmp=%d)\n", cmp_ac == 0 ? "yes" : "no", cmp_ac);

    /* 3. Compare sstring vs raw C-string */
    printf("\n[Compare sstring vs C-string]\n");
    int cmp_raw = cwist_sstring_compare(a, "Hello, World!");
    printf("a == \"Hello, World!\"? %s\n", cmp_raw == 0 ? "yes" : "no");

    /* 4. Copy: sstring → sstring */
    printf("\n[Copy sstring → sstring]\n");
    cwist_sstring_copy_sstring(c, a);   /* c now holds a's content */
    printf("c after copy from a: '%s'\n", c->data);

    /* 5. Copy: sstring → raw char buffer */
    printf("\n[Copy sstring → char buffer]\n");
    char buf[64];
    cwist_sstring_copy(a, buf);
    printf("buf after copy from a: '%s'\n", buf);

    cwist_sstring_destroy(a);
    cwist_sstring_destroy(b);
    cwist_sstring_destroy(c);
    printf("\n=== Done ===\n");
    return 0;
}
