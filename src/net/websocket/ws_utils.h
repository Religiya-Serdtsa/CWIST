#ifndef __CWIST_WS_UTILS_H__
#define __CWIST_WS_UTILS_H__

#include <stddef.h>
#include <stdint.h>

void sha1(const uint8_t *data, size_t len, uint8_t *hash);
char *base64_encode(const uint8_t *data, size_t input_length, size_t *output_length);

#endif
