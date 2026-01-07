#include <stdio.h>
#include <cwist/core/siphash/siphash.h>
#include <string.h>
#include <inttypes.h>

int main() {
    printf("=== SipHash Simple Example ===\n");

    // 1. Generate a random 128-bit key
    uint8_t key[16];
    printf("Generating random key...\n");
    cwist_generate_hash_seed(key);

    // Print key for demonstration
    printf("Key: ");
    for(int i=0; i<16; i++) {
        printf("%02x", key[i]);
    }
    printf("\n");

    // 2. Define some data to hash
    const char *data = "The quick brown fox jumps over the lazy dog";
    printf("Data: \"%s\"\n", data);

    // 3. Compute hash
    uint64_t hash = siphash24(data, strlen(data), key);

    // 4. Print result
    printf("SipHash-2-4: %" PRIu64 " (0x%" PRIx64 ")\n", hash, hash);

    return 0;
}

