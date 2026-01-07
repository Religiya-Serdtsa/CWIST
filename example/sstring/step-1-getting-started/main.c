#include <stdio.h>
#include <cwist/core/sstring/sstring.h>

int main() {
    printf("=== SString Getting Started ===\n");

    // 1. Create a cwist_sstring instance
    cwist_sstring *s = cwist_sstring_create();
    if (!s) {
        fprintf(stderr, "Failed to create cwist_sstring\n");
        return 1;
    }

    // 2. Assign some data (with leading/trailing spaces)
    printf("\n[Assign]\n");
    const char *initial_text = "   Hello, SString!   ";
    printf("Original text: '%s'\n", initial_text);
    cwist_sstring_assign(s, (char *)initial_text);

    // 3. Trim the string
    printf("\n[Trim]\n");
    cwist_sstring_trim(s);
    printf("Trimmed text:  '%s'\n", s->data);

    // 4. Seek (Extract substring)
    printf("\n[Seek]\n");
    char buffer[64];
    // Let's skip "Hello, " (7 chars) to get "SString!"
    cwist_error_t seek_err = cwist_sstring_seek(s, buffer, 7);
    if (seek_err.errtype == CWIST_ERR_INT8 && seek_err.error.err_i8 == ERR_SSTRING_OKAY) { 
        printf("Seek(7):       '%s'\n", buffer);
    } else {
        printf("Seek failed!\n");
    }

    // 5. Change size (safe resize)
    printf("\n[Change Size]\n");
    printf("Current size: %zu\n", s->size);
    
    // Resize to a smaller size (truncate)
    // Warning: Data loss expected if new size < current length
    cwist_error_t err = cwist_sstring_change_size(s, 5, true); // true = allow blow data
    if (err.errtype == CWIST_ERR_INT8 && err.error.err_i8 == ERR_SSTRING_OKAY) {
        printf("Resized to 5:  '%s'\n", s->data);
    } else {
        printf("Resize failed or warned!\n");
    }

    // 6. Clean up
    cwist_sstring_destroy(s);
    printf("\n=== Done ===\n");

    return 0;
}
