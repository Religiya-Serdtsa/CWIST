#include <stdio.h>
#include <cwist/core/sstring/sstring.h>

int main() {
    printf("=== SString: Substr & Append ===\n");

    cwist_sstring *s = cwist_sstring_create();
    if (!s) {
        fprintf(stderr, "Failed to allocate string\n");
        return 1;
    }

    /* 1. Build a string via repeated appends */
    printf("\n[Append]\n");
    cwist_sstring_assign(s, (char *)"Hello");
    cwist_sstring_append(s, ", ");
    cwist_sstring_append(s, "CWIST");
    cwist_sstring_append(s, "!");
    printf("After appends: '%s'\n", s->data);

    /* 2. Append with HTML-escaping */
    printf("\n[Append escaped]\n");
    cwist_sstring *escaped = cwist_sstring_create();
    cwist_sstring_assign(escaped, (char *)"Safe: ");
    cwist_sstring_append_escaped(escaped, "<script>alert('xss')</script>");
    printf("Escaped:       '%s'\n", escaped->data);

    /* 3. Extract a substring with cwist_sstring_substr */
    printf("\n[Substr]\n");
    cwist_sstring *sub = cwist_sstring_substr(s, 7, 5); /* "CWIST" */
    if (sub) {
        printf("substr(7,5):   '%s'\n", sub->data);
        cwist_sstring_destroy(sub);
    }

    /* 4. Seek: copy everything from offset into a char buffer */
    printf("\n[Seek]\n");
    char buf[64];
    cwist_sstring_seek(s, buf, 7);
    printf("seek(7):       '%s'\n", buf);

    /* 5. Trim variants */
    printf("\n[Trim]\n");
    cwist_sstring *padded = cwist_sstring_create();
    cwist_sstring_assign(padded, (char *)"   left-padded");
    cwist_sstring_ltrim(padded);
    printf("ltrim:         '%s'\n", padded->data);

    cwist_sstring_assign(padded, (char *)"right-padded   ");
    cwist_sstring_rtrim(padded);
    printf("rtrim:         '%s'\n", padded->data);

    cwist_sstring_destroy(padded);
    cwist_sstring_destroy(escaped);
    cwist_sstring_destroy(s);
    printf("\n=== Done ===\n");
    return 0;
}
