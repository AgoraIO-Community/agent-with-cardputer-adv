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
#include <limits.h>

#include "app_vendor_g722.h"

#if !defined(FALSE)
#define FALSE 0
#endif
#if !defined(TRUE)
#define TRUE (!FALSE)
#endif

static inline int16_t app_vendor_g722_saturate(int32_t amp)
{
    int16_t amp16 = (int16_t)amp;

    if (amp == amp16) {
        return amp16;
    }
    if (amp > INT16_MAX) {
        return INT16_MAX;
    }
    return INT16_MIN;
}

static inline void app_vendor_g722_block4(struct app_vendor_g722_band *band, int d)
{
    int wd1;
    int wd2;
    int wd3;
    int i;

    band->d[0] = d;
    band->r[0] = app_vendor_g722_saturate(band->s + d);
    band->p[0] = app_vendor_g722_saturate(band->sz + d);

    for (i = 0; i < 3; i++) {
        band->sg[i] = band->p[i] >> 15;
    }
    wd1 = app_vendor_g722_saturate(band->a[1] << 2);

    wd2 = (band->sg[0] == band->sg[1]) ? -wd1 : wd1;
    if (wd2 > 32767) {
        wd2 = 32767;
    }
    wd3 = (wd2 >> 7) + ((band->sg[0] == band->sg[2]) ? 128 : -128);
    wd3 += (band->a[2] * 32512) >> 15;
    if (wd3 > 12288) {
        wd3 = 12288;
    } else if (wd3 < -12288) {
        wd3 = -12288;
    }
    band->ap[2] = wd3;

    band->sg[0] = band->p[0] >> 15;
    band->sg[1] = band->p[1] >> 15;
    wd1 = (band->sg[0] == band->sg[1]) ? 192 : -192;
    wd2 = (band->a[1] * 32640) >> 15;

    band->ap[1] = app_vendor_g722_saturate(wd1 + wd2);
    wd3 = app_vendor_g722_saturate(15360 - band->ap[2]);
    if (band->ap[1] > wd3) {
        band->ap[1] = wd3;
    } else if (band->ap[1] < -wd3) {
        band->ap[1] = -wd3;
    }

    wd1 = (d == 0) ? 0 : 128;
    band->sg[0] = d >> 15;
    for (i = 1; i < 7; i++) {
        band->sg[i] = band->d[i] >> 15;
        wd2 = (band->sg[i] == band->sg[0]) ? wd1 : -wd1;
        wd3 = (band->b[i] * 32640) >> 15;
        band->bp[i] = app_vendor_g722_saturate(wd2 + wd3);
    }

    for (i = 6; i > 0; i--) {
        band->d[i] = band->d[i - 1];
        band->b[i] = band->bp[i];
    }

    for (i = 2; i > 0; i--) {
        band->r[i] = band->r[i - 1];
        band->p[i] = band->p[i - 1];
        band->a[i] = band->ap[i];
    }

    wd1 = app_vendor_g722_saturate(band->r[1] + band->r[1]);
    wd1 = (band->a[1] * wd1) >> 15;
    wd2 = app_vendor_g722_saturate(band->r[2] + band->r[2]);
    wd2 = (band->a[2] * wd2) >> 15;
    band->sp = app_vendor_g722_saturate(wd1 + wd2);

    band->sz = 0;
    for (i = 6; i > 0; i--) {
        wd1 = app_vendor_g722_saturate(band->d[i] + band->d[i]);
        band->sz += (band->b[i] * wd1) >> 15;
    }
    band->sz = app_vendor_g722_saturate(band->sz);
    band->s = app_vendor_g722_saturate(band->sp + band->sz);
}
