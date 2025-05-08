#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void aesRound(uint32_t in0, uint32_t in1, uint32_t in2, uint32_t in3,
              uint32_t key0, uint32_t key1, uint32_t key2, uint32_t key3,
              uint32_t *out0, uint32_t *out1, uint32_t *out2, uint32_t *out3);

#ifdef __cplusplus
}
#endif
