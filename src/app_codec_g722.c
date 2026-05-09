#include "app_codec_g722.h"

#include <stdbool.h>

#include "app_vendor_g722.h"

#define APP_CODEC_G722_RATE_BPS 64000

static app_vendor_g722_enc_ctx_t s_encoder;
static app_vendor_g722_dec_ctx_t s_decoder;
static bool s_encoder_ready;
static bool s_decoder_ready;

static void app_codec_g722_ensure_encoder(void)
{
    if (!s_encoder_ready) {
        app_vendor_g722_encoder_init(&s_encoder, APP_CODEC_G722_RATE_BPS, APP_VENDOR_G722_DEFAULT);
        s_encoder_ready = true;
    }
}

static void app_codec_g722_ensure_decoder(void)
{
    if (!s_decoder_ready) {
        app_vendor_g722_decoder_init(&s_decoder, APP_CODEC_G722_RATE_BPS, APP_VENDOR_G722_DEFAULT);
        s_decoder_ready = true;
    }
}

size_t app_codec_g722_encode(const int16_t *pcm, size_t sample_count, uint8_t *out, size_t out_size)
{
    size_t limited_sample_count;

    if (pcm == NULL || out == NULL || sample_count < 2U || out_size == 0U) {
        return 0;
    }

    limited_sample_count = sample_count & ~(size_t)1U;
    if (limited_sample_count > (out_size * 2U)) {
        limited_sample_count = (out_size * 2U) & ~(size_t)1U;
    }
    if (limited_sample_count == 0U) {
        return 0;
    }

    app_codec_g722_ensure_encoder();
    return (size_t)app_vendor_g722_encode(&s_encoder, pcm, (int)limited_sample_count, out);
}

size_t app_codec_g722_decode(const uint8_t *encoded, size_t encoded_size, int16_t *out, size_t out_capacity)
{
    size_t limited_encoded_size;

    if (encoded == NULL || out == NULL || encoded_size == 0U || out_capacity < 2U) {
        return 0;
    }

    limited_encoded_size = encoded_size;
    if (limited_encoded_size > (out_capacity / 2U)) {
        limited_encoded_size = out_capacity / 2U;
    }
    if (limited_encoded_size == 0U) {
        return 0;
    }

    app_codec_g722_ensure_decoder();
    return (size_t)app_vendor_g722_decode(&s_decoder, encoded, (int)limited_encoded_size, out);
}
