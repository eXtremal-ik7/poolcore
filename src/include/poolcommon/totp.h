#pragma once

#include <stdint.h>

int base32_encode(const uint8_t *data, int length, uint8_t *result, int bufSize);
int generateCode(const char *key, unsigned long tm);
