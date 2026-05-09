#include "app_codec_g711.h"

static uint8_t app_codec_g711a_from_pcm(int16_t pcm)
{
    static const uint16_t seg_end[8] = { 0x001F, 0x003F, 0x007F, 0x00FF, 0x01FF, 0x03FF, 0x07FF, 0x0FFF };
    uint8_t mask;
    uint8_t seg = 0;
    uint16_t mag;

    if (pcm >= 0) {
        mask = 0xD5;
        mag = (uint16_t)pcm;
    } else {
        mask = 0x55;
        mag = (uint16_t)(-pcm - 1);
    }

    mag >>= 3;
    while (seg < 8 && mag > seg_end[seg]) {
        seg++;
    }

    if (seg >= 8) {
        return (uint8_t)(0x7F ^ mask);
    }

    uint8_t aval = (uint8_t)(seg << 4);
    if (seg < 2) {
        aval |= (uint8_t)((mag >> 1) & 0x0F);
    } else {
        aval |= (uint8_t)((mag >> seg) & 0x0F);
    }
    return (uint8_t)(aval ^ mask);
}

static int16_t app_codec_g711a_to_pcm(uint8_t aval)
{
    aval ^= 0x55U;

    int16_t t = (int16_t)((aval & 0x0FU) << 4);
    int16_t seg = (int16_t)((aval & 0x70U) >> 4);

    switch (seg) {
    case 0:
        t += 8;
        break;
    case 1:
        t += 0x108;
        break;
    default:
        t += 0x108;
        t <<= (seg - 1);
        break;
    }

    return (aval & 0x80U) ? t : (int16_t)(-t);
}

size_t app_codec_g711a_encode(const int16_t *pcm, size_t sample_count, uint8_t *out, size_t out_size)
{
    size_t encoded = sample_count < out_size ? sample_count : out_size;

    if (pcm == NULL || out == NULL) {
        return 0;
    }

    for (size_t i = 0; i < encoded; i++) {
        out[i] = app_codec_g711a_from_pcm(pcm[i]);
    }
    return encoded;
}

size_t app_codec_g711a_decode(const uint8_t *encoded, size_t encoded_size, int16_t *out, size_t out_capacity)
{
    size_t decoded = encoded_size < out_capacity ? encoded_size : out_capacity;

    if (encoded == NULL || out == NULL) {
        return 0;
    }

    for (size_t i = 0; i < decoded; i++) {
        out[i] = app_codec_g711a_to_pcm(encoded[i]);
    }
    return decoded;
}
