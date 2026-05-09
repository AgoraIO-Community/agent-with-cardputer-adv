/*
 * Vendored from https://github.com/sippy/libg722
 *
 * Copyright (c) CMU 1993
 * Copyright (C) 2005 Steve Underwood
 * Copyright (c) 2014-2025 Sippy Software, Inc.
 *
 * See third_party/libg722/LICENSE for the full original license text.
 */

#pragma once

#include <stdint.h>

enum
{
    APP_VENDOR_G722_DEFAULT = 0x0000,
    APP_VENDOR_G722_SAMPLE_RATE_8000 = 0x0001,
    APP_VENDOR_G722_PACKED = 0x0002
};

struct app_vendor_g722_band
{
    int s;
    int sp;
    int sz;
    int r[3];
    int a[3];
    int ap[3];
    int p[3];
    int d[7];
    int b[7];
    int bp[7];
    int sg[7];
    int nb;
    int det;
};

typedef struct
{
    int itu_test_mode;
    int packed;
    int eight_k;
    int bits_per_sample;
    int x[24];
    struct app_vendor_g722_band band[2];
    unsigned int in_buffer;
    int in_bits;
    unsigned int out_buffer;
    int out_bits;
} app_vendor_g722_enc_ctx_t;

typedef struct
{
    int itu_test_mode;
    int packed;
    int eight_k;
    int bits_per_sample;
    int x[24];
    struct app_vendor_g722_band band[2];
    unsigned int in_buffer;
    int in_bits;
    unsigned int out_buffer;
    int out_bits;
} app_vendor_g722_dec_ctx_t;

void app_vendor_g722_encoder_init(app_vendor_g722_enc_ctx_t *ctx, int rate, int options);
void app_vendor_g722_decoder_init(app_vendor_g722_dec_ctx_t *ctx, int rate, int options);
int app_vendor_g722_encode(app_vendor_g722_enc_ctx_t *ctx, const int16_t amp[], int len, uint8_t g722_data[]);
int app_vendor_g722_decode(app_vendor_g722_dec_ctx_t *ctx, const uint8_t g722_data[], int len, int16_t amp[]);
