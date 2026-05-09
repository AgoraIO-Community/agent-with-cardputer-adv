#pragma once

#include <stddef.h>
#include <stdint.h>

size_t app_codec_g711a_encode(const int16_t *pcm, size_t sample_count, uint8_t *out, size_t out_size);
size_t app_codec_g711a_decode(const uint8_t *encoded, size_t encoded_size, int16_t *out, size_t out_capacity);
