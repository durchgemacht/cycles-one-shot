/* The sampler machine's filter, see smp_filter.h. */
#include "smp_filter.h"

#define RING_LEVEL (1 << 14)    /* 2^-17 of full scale: below -102 dB */

static int32_t clamp_word(int32_t w)
{
    return w < 0 ? 0 : w > SMP_FILTER_MAX ? SMP_FILTER_MAX : w;
}

/* a + (b - a) * t / 256 for t = 0..255, within 32 bits: 8 bits of the difference are dropped
   first (an error below 2^-23, far under the table's own rounding at any cutoff). */
static int32_t lerp(int32_t a, int32_t b, int32_t t)
{
    return a + ((b - a) >> 8) * t;
}

/* f/2 and q/2 for the cutoff index i (0..16384) and the resonance word r (0..32512), bilinear
   from the table. */
static void lookup(int32_t i, int32_t r, int32_t *f2, int32_t *q2)
{
    int32_t ci = i >> 7, ti = (i & 127) << 1;           /* column and its fraction, 0..254 */
    int32_t cr = r >> 11, tr = (r & 2047) >> 3;         /* row and its fraction, 0..255 */
    const int32_t (*a)[2] = smp_filter_tab[cr], (*b)[2] = smp_filter_tab[cr + 1];

    if (ci >= SMP_FILTER_NI - 1) {                      /* i = 16384 itself */
        ci = SMP_FILTER_NI - 2;
        ti = 256;
    }
    *f2 = lerp(lerp(a[ci][0], a[ci + 1][0], ti), lerp(b[ci][0], b[ci + 1][0], ti), tr);
    *q2 = lerp(lerp(a[ci][1], a[ci + 1][1], ti), lerp(b[ci][1], b[ci + 1][1], ti), tr);
}

void smp_filter_reset(smp_filter *f)
{
    f->l = f->b = 0;
    f->f2 = f->q2 = f->df2 = f->dq2 = 0;
    f->f2t = f->q2t = 0;
    f->mode = SMP_FILTER_OFF;
    f->prime = 0;
}

void smp_filter_set(smp_filter *f, int32_t cutoff, int32_t reso)
{
    int32_t mode, i, f2, q2;

    cutoff = clamp_word(cutoff);
    reso = clamp_word(reso);
    if (cutoff == SMP_FILTER_CENTRE) {
        f->mode = SMP_FILTER_OFF;
        return;
    }
    if (cutoff < SMP_FILTER_CENTRE) {
        mode = SMP_FILTER_LP;
        i = cutoff;
    } else {
        mode = SMP_FILTER_HP;
        i = cutoff - SMP_FILTER_CENTRE;
    }
    lookup(i, reso, &f2, &q2);
    if (f->mode != mode) {                              /* entering this side: start afresh */
        f->l = f->b = 0;
        f->prime = 1;
        f->f2 = f2;
        f->q2 = q2;
        f->df2 = f->dq2 = 0;
    } else {
        f->f2 = f->f2t;                                 /* glide from the last targets */
        f->q2 = f->q2t;
        f->df2 = (f2 - f->f2) >> 5;
        f->dq2 = (q2 - f->q2) >> 5;
    }
    f->f2t = f2;
    f->q2t = q2;
    f->mode = mode;
}

int smp_filter_ringing(const smp_filter *f)
{
    int32_t l = f->l < 0 ? -f->l : f->l, b = f->b < 0 ? -f->b : f->b;

    return f->mode != SMP_FILTER_OFF && (l >= RING_LEVEL || b >= RING_LEVEL);
}

void smp_filter_run(smp_filter *f, int32_t *buf)
{
    if (f->mode == SMP_FILTER_OFF)
        return;
    if (f->prime) {
        f->l = f->mode == SMP_FILTER_LP ? buf[0] >> 1 : 0;   /* a lowpass starts at the signal */
        f->b = 0;
        f->prime = 0;
    }
    smp_filter_frames(f, buf);
}

#ifndef __mcoldfire__
/* The host model of the ColdFire loop (smp_filter_cf.S), with the EMAC's arithmetic in
   fractional, saturating mode (MACSR 0xa0): each product of two Q31 values is truncated to 2^-39
   (floored), sums are exact beyond that, and reading a sum floors it to Q31 and saturates. The
   accumulator here holds units of 2^-39. The loop runs on half the signal (the bandpass state
   carries up to twice the output's level near the top of the range) and doubles the result, so
   only an output beyond full scale saturates. ONE is as close to 1.0 as Q31 gets: doubling a
   value by adding ONE times it is a 2^-31 short, which the model shares with the loop. */
typedef int64_t acc_t;

#define ONE INT32_MAX

static acc_t prod(int32_t a, int32_t b)
{
    return ((acc_t)a * b) >> 23;
}

static acc_t load(int32_t v)
{
    return (acc_t)v << 8;
}

static int32_t read(acc_t acc)
{
    acc >>= 8;
    return acc > INT32_MAX ? INT32_MAX : acc < INT32_MIN ? INT32_MIN : (int32_t)acc;
}

void smp_filter_frames(smp_filter *f, int32_t *buf)
{
    int32_t l = f->l, b = f->b, f2 = f->f2, q2 = f->q2;
    int hp = f->mode == SMP_FILTER_HP;
    int n;

    for (n = 0; n < SMP_FILTER_BLOCK; n++) {
        int32_t x = buf[n] >> 1, h, lp;
        acc_t acc;

        acc = load(l) + prod(f2, b) + prod(f2, b);      /* l' = l + f b */
        l = read(acc);
        acc = load(x) - prod(q2, b) - prod(q2, b) - load(l);   /* h = x - l' - q b */
        h = read(acc);
        acc = load(b) + prod(f2, h) + prod(f2, h);      /* b' = b + f h */
        b = read(acc);
        acc = load(l) + prod(f2, b) + prod(f2, b);      /* lp = l' + f b' */
        lp = read(acc);
        if (hp)
            acc = load(x) + prod(ONE, x) - load(lp) - prod(ONE, lp);   /* 2 (x - lp) */
        else
            acc = load(lp) + prod(ONE, lp);             /* 2 lp */
        buf[n] = read(acc);
        f2 += f->df2;
        q2 += f->dq2;
    }
    f->l = l;
    f->b = b;
}
#endif
