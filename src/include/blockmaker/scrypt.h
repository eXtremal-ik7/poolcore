#pragma once
#include <stdint.h>

static const int LTC_SCRATCHPAD_SIZE = 131072 + 63;

#ifdef __cplusplus
extern "C" {
#endif

void scrypt_1024_1_1_256(const void *input, uint8_t output[32]);

#ifdef __cplusplus
}
#endif
