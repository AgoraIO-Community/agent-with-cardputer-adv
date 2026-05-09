/*
 * Vendored from https://github.com/sippy/libg722
 *
 * Copyright (c) CMU 1993
 * Copyright (C) 2005 Steve Underwood
 * Copyright (c) 2014-2025 Sippy Software, Inc.
 *
 * See third_party/libg722/LICENSE for the full original license text.
 */

#include <string.h>

#include "app_vendor_g722.h"
#include "app_vendor_g722_common.h"

void app_vendor_g722_decoder_init(app_vendor_g722_dec_ctx_t *ctx, int rate, int options)
{
    if (ctx == NULL) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
    if (rate == 48000) {
        ctx->bits_per_sample = 6;
    } else if (rate == 56000) {
        ctx->bits_per_sample = 7;
    } else {
        ctx->bits_per_sample = 8;
    }
    if ((options & APP_VENDOR_G722_SAMPLE_RATE_8000) != 0) {
        ctx->eight_k = TRUE;
    }
    if ((options & APP_VENDOR_G722_PACKED) != 0 && ctx->bits_per_sample != 8) {
        ctx->packed = TRUE;
    } else {
        ctx->packed = FALSE;
    }
    ctx->band[0].det = 32;
    ctx->band[1].det = 8;
}

int app_vendor_g722_decode(app_vendor_g722_dec_ctx_t *ctx, const uint8_t g722_data[], int len, int16_t amp[])
{
    static const int wl[8] = { -60, -30, 58, 172, 334, 538, 1198, 3042 };
    static const int rl42[16] = { 0, 7, 6, 5, 4, 3, 2, 1, 7, 6, 5, 4, 3, 2, 1, 0 };
    static const int ilb[32] = {
        2048, 2093, 2139, 2186, 2233, 2282, 2332, 2383,
        2435, 2489, 2543, 2599, 2656, 2714, 2774, 2834,
        2896, 2960, 3025, 3091, 3158, 3228, 3298, 3371,
        3444, 3520, 3597, 3676, 3756, 3838, 3922, 4008
    };
    static const int wh[3] = { 0, -214, 798 };
    static const int rh2[4] = { 2, 1, 2, 1 };
    static const int qm2[4] = { -7408, -1616, 7408, 1616 };
    static const int qm4[16] = {
        0, -20456, -12896, -8968,
        -6288, -4240, -2584, -1200,
        20456, 12896, 8968, 6288,
        4240, 2584, 1200, 0
    };
    static const int qm5[32] = {
        -280, -280, -23352, -17560,
        -14120, -11664, -9752, -8184,
        -6864, -5712, -4696, -3784,
        -2960, -2208, -1520, -880,
        23352, 17560, 14120, 11664,
        9752, 8184, 6864, 5712,
        4696, 3784, 2960, 2208,
        1520, 880, 280, -280
    };
    static const int qm6[64] = {
        -136, -136, -136, -136,
        -24808, -21904, -19008, -16704,
        -14984, -13512, -12280, -11192,
        -10232, -9360, -8576, -7856,
        -7192, -6576, -6000, -5456,
        -4944, -4464, -4008, -3576,
        -3168, -2776, -2400, -2032,
        -1688, -1360, -1040, -728,
        24808, 21904, 19008, 16704,
        14984, 13512, 12280, 11192,
        10232, 9360, 8576, 7856,
        7192, 6576, 6000, 5456,
        4944, 4464, 4008, 3576,
        3168, 2776, 2400, 2032,
        1688, 1360, 1040, 728,
        432, 136, -432, -136
    };
    static const int qmf_coeffs[12] = { 3, -11, 12, 32, -210, 951, 3876, -805, 362, -156, 53, -11 };

    int outlen = 0;
    int rhigh = 0;

    if (ctx == NULL || g722_data == NULL || amp == NULL || len <= 0) {
        return 0;
    }

    for (int j = 0; j < len;) {
        int dlowt;
        int rlow;
        int ihigh;
        int dhigh;
        int xout1;
        int xout2;
        int wd1;
        int wd2;
        int wd3;
        int code;

        if (ctx->packed) {
            if (ctx->in_bits < ctx->bits_per_sample) {
                ctx->in_buffer |= ((unsigned int)g722_data[j++] << ctx->in_bits);
                ctx->in_bits += 8;
            }
            code = (int)(ctx->in_buffer & ((1U << ctx->bits_per_sample) - 1U));
            ctx->in_buffer >>= ctx->bits_per_sample;
            ctx->in_bits -= ctx->bits_per_sample;
        } else {
            code = g722_data[j++];
        }

        switch (ctx->bits_per_sample) {
        case 7:
            wd1 = code & 0x1F;
            ihigh = (code >> 5) & 0x03;
            wd2 = qm5[wd1];
            wd1 >>= 1;
            break;
        case 6:
            wd1 = code & 0x0F;
            ihigh = (code >> 4) & 0x03;
            wd2 = qm4[wd1];
            break;
        case 8:
        default:
            wd1 = code & 0x3F;
            ihigh = (code >> 6) & 0x03;
            wd2 = qm6[wd1];
            wd1 >>= 2;
            break;
        }

        wd2 = (ctx->band[0].det * wd2) >> 15;
        rlow = ctx->band[0].s + wd2;
        if (rlow > 16383) {
            rlow = 16383;
        } else if (rlow < -16384) {
            rlow = -16384;
        }

        wd2 = qm4[wd1];
        dlowt = (ctx->band[0].det * wd2) >> 15;

        wd2 = rl42[wd1];
        wd1 = (ctx->band[0].nb * 127) >> 7;
        wd1 += wl[wd2];
        if (wd1 < 0) {
            wd1 = 0;
        } else if (wd1 > 18432) {
            wd1 = 18432;
        }
        ctx->band[0].nb = wd1;

        wd1 = (ctx->band[0].nb >> 6) & 31;
        wd2 = 8 - (ctx->band[0].nb >> 11);
        wd3 = (wd2 < 0) ? (ilb[wd1] << -wd2) : (ilb[wd1] >> wd2);
        ctx->band[0].det = wd3 << 2;

        app_vendor_g722_block4(&ctx->band[0], dlowt);

        if (!ctx->eight_k) {
            wd2 = qm2[ihigh];
            dhigh = (ctx->band[1].det * wd2) >> 15;
            rhigh = dhigh + ctx->band[1].s;
            if (rhigh > 16383) {
                rhigh = 16383;
            } else if (rhigh < -16384) {
                rhigh = -16384;
            }

            wd2 = rh2[ihigh];
            wd1 = (ctx->band[1].nb * 127) >> 7;
            wd1 += wh[wd2];
            if (wd1 < 0) {
                wd1 = 0;
            } else if (wd1 > 22528) {
                wd1 = 22528;
            }
            ctx->band[1].nb = wd1;

            wd1 = (ctx->band[1].nb >> 6) & 31;
            wd2 = 10 - (ctx->band[1].nb >> 11);
            wd3 = (wd2 < 0) ? (ilb[wd1] << -wd2) : (ilb[wd1] >> wd2);
            ctx->band[1].det = wd3 << 2;

            app_vendor_g722_block4(&ctx->band[1], dhigh);
        }

        if (ctx->itu_test_mode) {
            amp[outlen++] = (int16_t)(rlow << 1);
            amp[outlen++] = (int16_t)(rhigh << 1);
        } else if (ctx->eight_k) {
            amp[outlen++] = (int16_t)(rlow << 1);
        } else {
            for (int i = 0; i < 22; i++) {
                ctx->x[i] = ctx->x[i + 2];
            }
            ctx->x[22] = rlow + rhigh;
            ctx->x[23] = rlow - rhigh;

            xout1 = 0;
            xout2 = 0;
            for (int i = 0; i < 12; i++) {
                xout2 += ctx->x[2 * i] * qmf_coeffs[i];
                xout1 += ctx->x[2 * i + 1] * qmf_coeffs[11 - i];
            }
            amp[outlen++] = app_vendor_g722_saturate(xout1 >> 11);
            amp[outlen++] = app_vendor_g722_saturate(xout2 >> 11);
        }
    }

    return outlen;
}
