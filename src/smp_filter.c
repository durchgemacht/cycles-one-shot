/* The sampler machine's filter, see smp_filter.h. */
#include "smp_filter.h"

#define RING_LEVEL (1 << 14)    /* 2^-17 of full scale: below -102 dB */
#define ONE SMP_FILTER_ONE
#define GLIDE (SMP_FILTER_BLOCK - 1)    /* a weight reaches its target on the block's last frame */

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

/* The dry/wet weight at the distance d (> 0) from the centre: the whole filter from one knob
   step (256) out, a share of it within. */
static int32_t wet_of(int32_t d)
{
    return d >= 256 ? ONE : d << 23;
}

void smp_filter_reset(smp_filter *f)
{
    f->l = f->b = 0;
    f->f2 = f->q2 = f->df2 = f->dq2 = 0;
    f->wet = f->dwet = f->mix = f->dmix = 0;
    f->f2t = f->q2t = f->wett = f->mixt = 0;
    f->mode = SMP_FILTER_OFF;
    f->loop = SMP_FILTER_LOOP_LP;
    f->prime = 0;
}

void smp_filter_set(smp_filter *f, int32_t cutoff, int32_t reso)
{
    int32_t d, mode, i, f2, q2, wett, mixt;

    cutoff = clamp_word(cutoff);
    reso = clamp_word(reso);
    d = cutoff - SMP_FILTER_CENTRE;
    if (d == 0) {
        if (f->mode == SMP_FILTER_OFF)
            return;
        if (f->wett == 0) {                             /* wet has glided to 0: off from here */
            smp_filter_reset(f);
            return;
        }
        /* the centre: wet glides to 0 across this block; the side and its coefficients stay */
        f->f2 = f->f2t;
        f->q2 = f->q2t;
        f->df2 = f->dq2 = 0;
        f->mix = f->mixt;
        f->dmix = 0;
        f->wet = f->wett;
        f->wett = 0;
        f->dwet = (0 - f->wet) / GLIDE;
        return;
    }
    if (d < 0) {
        mode = SMP_FILTER_LP;
        i = cutoff;
        mixt = 0;
        wett = wet_of(-d);
    } else {
        mode = SMP_FILTER_HP;
        i = d;
        mixt = ONE;
        wett = wet_of(d);
    }
    lookup(i, reso, &f2, &q2);
    if (f->mode == SMP_FILTER_OFF) {                    /* a step in from off: start afresh */
        f->l = f->b = 0;
        f->prime = 1;
        f->f2 = f2;
        f->q2 = q2;
        f->df2 = f->dq2 = 0;
        f->mix = mixt;
        f->dmix = 0;
        f->wet = 0;
        f->dwet = wett / GLIDE;
    } else {                                            /* glide from the last targets */
        f->f2 = f->f2t;
        f->q2 = f->q2t;
        f->df2 = (f2 - f->f2) >> 5;
        f->dq2 = (q2 - f->q2) >> 5;
        f->mix = f->mixt;
        f->dmix = (mixt - f->mix) / GLIDE;
        f->wet = f->wett;
        f->dwet = (wett - f->wet) / GLIDE;
    }
    f->f2t = f2;
    f->q2t = q2;
    f->wett = wett;
    f->mixt = mixt;
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
    if (f->wet == ONE && !f->dwet && !f->dmix && (f->mix == 0 || f->mix == ONE))
        f->loop = f->mix ? SMP_FILTER_LOOP_HP : SMP_FILTER_LOOP_LP;
    else
        f->loop = SMP_FILTER_LOOP_MIX;
    smp_filter_frames(f, buf);
}

#ifndef __mcoldfire__
/* The host model of the ColdFire loops (smp_filter_cf.S), with the EMAC's arithmetic in
   fractional, saturating mode (MACSR 0xa0): each product of two Q31 values is truncated to 2^-39
   (floored), sums are exact beyond that, and reading a sum floors it to Q31 and saturates. The
   accumulator here holds units of 2^-39. The loop runs on half the signal (the bandpass state
   carries up to twice the output's level near the top of the range) and doubles the result, so
   only an output beyond full scale saturates. ONE is as close to 1.0 as Q31 gets: doubling a
   value by adding ONE times it is a 2^-31 short, which the model shares with the loop; -1.0
   (0x80000000) times a value subtracts it exactly, so "- load(v)" below is what the loop does. */
typedef int64_t acc_t;

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
    int32_t l = f->l, b = f->b, f2 = f->f2, q2 = f->q2, wet = f->wet, mix = f->mix;
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
        if (f->loop == SMP_FILTER_LOOP_LP)
            acc = load(lp) + prod(ONE, lp);             /* 2 lp */
        else if (f->loop == SMP_FILTER_LOOP_HP)
            acc = load(x) + prod(ONE, x) - load(lp) - prod(ONE, lp);   /* 2 (x - lp) */
        else {
            int32_t d, filt, u, y;

            acc = load(x) - load(lp) - load(lp);        /* hp - lp = x - 2 lp */
            d = read(acc);
            acc = load(lp) + prod(mix, d);              /* filt = lp + mix (hp - lp) */
            filt = read(acc);
            acc = load(filt) - load(x);                 /* filt - x */
            u = read(acc);
            acc = load(x) + prod(wet, u);               /* y = x + wet (filt - x) */
            y = read(acc);
            acc = load(y) + prod(ONE, y);               /* 2 y */
            wet += f->dwet;
            mix += f->dmix;
        }
        buf[n] = read(acc);
        f2 += f->df2;
        q2 += f->dq2;
    }
    f->l = l;
    f->b = b;
}
#endif
