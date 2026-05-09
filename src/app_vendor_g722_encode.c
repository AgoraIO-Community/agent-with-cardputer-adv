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

void app_vendor_g722_encoder_init(app_vendor_g722_enc_ctx_t *ctx, int rate, int options)
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

int app_vendor_g722_encode(app_vendor_g722_enc_ctx_t *ctx, const int16_t amp[], int len, uint8_t g722_data[])
{
    static const int q6[32] = {
        0, 35, 72, 110, 150, 190, 233, 276,
        323, 370, 422, 473, 530, 587, 650, 714,
        786, 858, 940, 1023, 1121, 1219, 1339, 1458,
        1612, 1765, 1980, 2195, 2557, 2919, 0, 0
    };
    static const int iln[32] = {
        0, 63, 62, 31, 30, 29, 28, 27,
        26, 25, 24, 23, 22, 21, 20, 19,
        18, 17, 16, 15, 14, 13, 12, 11,
        10, 9, 8, 7, 6, 5, 4, 0
    };
    static const int ilp[32] = {
        0, 61, 60, 59, 58, 57, 56, 55,
        54, 53, 52, 51, 50, 49, 48, 47,
        46, 45, 44, 43, 42, 41, 40, 39,
        38, 37, 36, 35, 34, 33, 32, 0
    };
    static const int wl[8] = { -60, -30, 58, 172, 334, 538, 1198, 3042 };
    static const int rl42[16] = { 0, 7, 6, 5, 4, 3, 2, 1, 7, 6, 5, 4, 3, 2, 1, 0 };
    static const int ilb[32] = {
        2048, 2093, 2139, 2186, 2233, 2282, 2332, 2383,
        2435, 2489, 2543, 2599, 2656, 2714, 2774, 2834,
        2896, 2960, 3025, 3091, 3158, 3228, 3298, 3371,
        3444, 3520, 3597, 3676, 3756, 3838, 3922, 4008
    };
    static const int qm4[16] = {
        0, -20456, -12896, -8968,
        -6288, -4240, -2584, -1200,
        20456, 12896, 8968, 6288,
        4240, 2584, 1200, 0
    };
    static const int qm2[4] = { -7408, -1616, 7408, 1616 };
    static const int qmf_coeffs[12] = { 3, -11, 12, 32, -210, 951, 3876, -805, 362, -156, 53, -11 };
    static const int ihn[3] = { 0, 1, 0 };
    static const int ihp[3] = { 0, 3, 2 };
    static const int wh[3] = { 0, -214, 798 };
    static const int rh2[4] = { 2, 1, 2, 1 };

    int g722_bytes = 0;
    int xhigh = 0;

    if (ctx == NULL || amp == NULL || g722_data == NULL || len <= 0) {
        return 0;
    }

    for (int j = 0; j < len;) {
        int dlow;
        int dhigh;
        int el;
        int wd;
        int wd1;
        int ril;
        int wd2;
        int il4;
        int ih2;
        int wd3;
        int eh;
        int mih;
        int xlow;
        int sumeven;
        int sumodd;
        int ihigh;
        int ilow;
        int code;

        if (ctx->itu_test_mode) {
            xlow = amp[j++] >> 1;
            xhigh = xlow;
        } else if (ctx->eight_k) {
            xlow = amp[j++] >> 1;
        } else {
            for (int i = 0; i < 22; i++) {
                ctx->x[i] = ctx->x[i + 2];
            }
            ctx->x[22] = amp[j++];
            ctx->x[23] = amp[j++];

            sumeven = 0;
            sumodd = 0;
            for (int i = 0; i < 12; i++) {
                sumodd += ctx->x[2 * i] * qmf_coeffs[i];
                sumeven += ctx->x[2 * i + 1] * qmf_coeffs[11 - i];
            }
            xlow = (sumeven + sumodd) >> 14;
            xhigh = (sumeven - sumodd) >> 14;
        }

        el = app_vendor_g722_saturate(xlow - ctx->band[0].s);
        wd = (el >= 0) ? el : -(el + 1);

        int i;
        for (i = 1; i < 30; i++) {
            wd1 = (q6[i] * ctx->band[0].det) >> 12;
            if (wd < wd1) {
                break;
            }
        }
        ilow = (el < 0) ? iln[i] : ilp[i];

        ril = ilow >> 2;
        wd2 = qm4[ril];
        dlow = (ctx->band[0].det * wd2) >> 15;

        il4 = rl42[ril];
        wd = (ctx->band[0].nb * 127) >> 7;
        ctx->band[0].nb = wd + wl[il4];
        if (ctx->band[0].nb < 0) {
            ctx->band[0].nb = 0;
        } else if (ctx->band[0].nb > 18432) {
            ctx->band[0].nb = 18432;
        }

        wd1 = (ctx->band[0].nb >> 6) & 31;
        wd2 = 8 - (ctx->band[0].nb >> 11);
        wd3 = (wd2 < 0) ? (ilb[wd1] << -wd2) : (ilb[wd1] >> wd2);
        ctx->band[0].det = wd3 << 2;

        app_vendor_g722_block4(&ctx->band[0], dlow);

        if (ctx->eight_k) {
            code = (0xC0 | ilow) >> (8 - ctx->bits_per_sample);
        } else {
            eh = app_vendor_g722_saturate(xhigh - ctx->band[1].s);
            wd = (eh >= 0) ? eh : -(eh + 1);
            wd1 = (564 * ctx->band[1].det) >> 12;
            mih = (wd >= wd1) ? 2 : 1;
            ihigh = (eh < 0) ? ihn[mih] : ihp[mih];

            wd2 = qm2[ihigh];
            dhigh = (ctx->band[1].det * wd2) >> 15;

            ih2 = rh2[ihigh];
            wd = (ctx->band[1].nb * 127) >> 7;
            ctx->band[1].nb = wd + wh[ih2];
            if (ctx->band[1].nb < 0) {
                ctx->band[1].nb = 0;
            } else if (ctx->band[1].nb > 22528) {
                ctx->band[1].nb = 22528;
            }

            wd1 = (ctx->band[1].nb >> 6) & 31;
            wd2 = 10 - (ctx->band[1].nb >> 11);
            wd3 = (wd2 < 0) ? (ilb[wd1] << -wd2) : (ilb[wd1] >> wd2);
            ctx->band[1].det = wd3 << 2;

            app_vendor_g722_block4(&ctx->band[1], dhigh);
            code = ((ihigh << 6) | ilow) >> (8 - ctx->bits_per_sample);
        }

        if (ctx->packed) {
            ctx->out_buffer |= ((unsigned int)code << ctx->out_bits);
            ctx->out_bits += ctx->bits_per_sample;
            if (ctx->out_bits >= 8) {
                g722_data[g722_bytes++] = (uint8_t)(ctx->out_buffer & 0xFFU);
                ctx->out_bits -= 8;
                ctx->out_buffer >>= 8;
            }
        } else {
            g722_data[g722_bytes++] = (uint8_t)code;
        }
    }

    return g722_bytes;
}
