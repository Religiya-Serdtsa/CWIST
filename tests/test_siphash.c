#include <cwist/core/siphash/siphash.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <inttypes.h>

void test_basic_hashing() {
    printf("Testing basic hashing...\n");
    uint8_t key[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const char *data = "hello world";
    
    uint64_t hash1 = siphash24(data, strlen(data), key);
    uint64_t hash2 = siphash24(data, strlen(data), key);
    
    // Consistency check
    assert(hash1 == hash2);
    printf("Hash of '%s': %" PRIu64 "\n", data, hash1);
    
    // Distinctness check (different data)
    const char *data2 = "hello world!";
    uint64_t hash3 = siphash24(data2, strlen(data2), key);
    assert(hash1 != hash3);
    
    // Distinctness check (different key)
    uint8_t key2[16] = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    uint64_t hash4 = siphash24(data, strlen(data), key2);
    assert(hash1 != hash4);

    printf("Passed basic hashing.\n");
}

void test_key_generation() {
    printf("Testing key generation...\n");
    uint8_t key1[16];
    uint8_t key2[16];
    
    memset(key1, 0, 16);
    memset(key2, 0, 16);
    
    cwist_generate_hash_seed(key1);
    cwist_generate_hash_seed(key2);
    
    // Very unlikely to be same
    assert(memcmp(key1, key2, 16) != 0);
    
    // Check it's not all zeros (probabilistic, but highly likely)
    int all_zeros = 1;
    for(int i=0; i<16; i++) {
        if (key1[i] != 0) {
            all_zeros = 0;
            break;
        }
    }
    assert(!all_zeros);

    printf("Passed key generation.\n");
}

/*
 * Verification with official test vectors could go here if we had them handy.
 * For now, we rely on the internal consistency.
 */

int main() {
    test_basic_hashing();
    test_key_generation();
    printf("All siphash tests passed!\n");
    return 0;
}
