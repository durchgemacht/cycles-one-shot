/* One-shot sample voice, see smp_core.h. */
#include "smp_core.h"

#ifdef SMP_DEBUG
#include <assert.h>
#define SMP_ASSERT(x) assert(x)
#else
#define SMP_ASSERT(x) ((void)0)
#endif

/* Taps reach this many frames back from the playhead (and SMP_TAPS - 1 - BACK forward). */
#define BACK (SMP_TAPS / 2 - 1)

/* 2^25 / SMP_FADE rounded up: a frame count times this, >> 10, is its share of a fade in Q15. */
#define FADE_RECIP (((1u << 25) + SMP_FADE - 1) / SMP_FADE)

void smp_trigger(smp_voice *v, const int16_t *pcm, int32_t length, int32_t start, int32_t end)
{
    if (end > length)
        end = length;
    if (start < 0)
        start = 0;
    v->pcm = pcm;
    v->start = start;
    v->end = end;
    v->pos = start;
    v->frac = 0;
    v->active = start < end;
    v->fade_in = start > 0 ? 0 : SMP_FADE;
}

void smp_set_ratio(smp_voice *v, uint32_t ratio_q16)
{
    if (ratio_q16 > SMP_MAX_RATIO_Q16)
        ratio_q16 = SMP_MAX_RATIO_Q16;
    if (ratio_q16 < SMP_MIN_RATIO_Q16)
        ratio_q16 = SMP_MIN_RATIO_Q16;
    /* At 96 kHz each frame moves half the ratio on; as 32.32 that is the ratio shifted by 15. */
    v->step_int = ratio_q16 >> 17;
    v->step_frac = ratio_q16 << 15;
}

/* A fade's gain n frames in, Q15: 0 at n = 0, 32768 from SMP_FADE on, the S curve 3x^2 - 2x^3
   of x = n / SMP_FADE between. */
static uint32_t fade_gain(int32_t n)
{
    uint32_t x;

    if (n <= 0)
        return 0;
    if (n >= SMP_FADE)
        return 32768;
    x = ((uint32_t)n * FADE_RECIP) >> 10;
    return ((x * x >> 15) * (3u * 32768 - 2 * x)) >> 15;
}

/* a * g / 32768 for g up to 32768, towards zero, without 64-bit arithmetic. */
static int32_t scale(int32_t a, uint32_t g)
{
    uint32_t m = a < 0 ? 0u - (uint32_t)a : (uint32_t)a;
    uint32_t r = (m >> 15) * g + (((m & 0x7fff) * g) >> 15);

    return a < 0 ? -(int32_t)r : (int32_t)r;
}

/* Frames from the block's first until the playhead reaches the end, rounded up (distance and
   step cut to 16 fractional bits, so possibly one more); SMP_FADE + SMP_BLOCK2X while the end
   lies beyond the block and its fade. */
static int32_t frames_to_end(const smp_voice *v)
{
    int32_t d = v->end - v->pos;
    uint32_t dq, sq;

    if (d > (int32_t)((SMP_FADE + SMP_BLOCK2X + 1) * (v->step_int + 1)))
        return SMP_FADE + SMP_BLOCK2X;
    if (d <= 0)
        return 0;
    dq = ((uint32_t)d << 16) - (v->frac >> 16);
    sq = (v->step_int << 16) | (v->step_frac >> 16);
    return (int32_t)((dq + sq - 1) / sq);
}

/* Copies the frames the next block reads into scratch, with zeros outside [start, end), so the
   inner loop needs no bounds checks. Returns how many frames it wrote. */
static int32_t fetch(const smp_voice *v, int16_t *scratch)
{
    /* The last of the 64 frames sits 63 steps on. The fraction carries at most once a step;
       the top 16 bits of fraction and step, rounded up, bound the carries from above. */
    uint32_t carries = ((v->frac >> 16) + 1u + 63u * ((v->step_frac >> 16) + 1u)) >> 16;
    int32_t n = (int32_t)(63u * v->step_int + carries) + SMP_TAPS;
    int32_t first = v->pos - BACK;
    int32_t lo = v->start - first, hi = v->end - first, k = 0;

    if (lo > n)
        lo = n;
    if (hi > n)
        hi = n;
    for (; k < lo; k++)
        scratch[k] = 0;
    for (; k < hi; k++)
        scratch[k] = v->pcm[first + k];
    for (; k < n; k++)
        scratch[k] = 0;
    return n;
}

int smp_render_with(smp_voice *v, int32_t *out, int16_t *scratch,
                    const int16_t (*table)[SMP_TAPS])
{
    uint32_t f, ip, si, sf;
    int32_t k, n, fi, rem;

    if (!v->active) {
        for (k = 0; k < SMP_BLOCK2X; k++)
            out[k] = 0;
        return 0;
    }
    n = fetch(v, scratch);
    SMP_ASSERT(n <= (int32_t)SMP_SCRATCH);
    (void)n;

    f = v->frac;
    ip = 0;
    si = v->step_int;
    sf = v->step_frac;
    fi = v->fade_in;
    rem = frames_to_end(v);
    for (k = 0; k < SMP_BLOCK2X; k++, rem--) {
        const int16_t *c = table[f >> 24];
        const int16_t *x = scratch + ip;
        uint32_t g;

        SMP_ASSERT(ip + SMP_TAPS <= (uint32_t)n);
        out[k] = c[0] * x[0] + c[1] * x[1] + c[2] * x[2] + c[3] * x[3] + c[4] * x[4]
               + c[5] * x[5];
        if (fi < SMP_FADE || rem < SMP_FADE) {
            uint32_t gain = fade_gain(fi);

            if (rem < SMP_FADE)
                gain = (gain * fade_gain(rem)) >> 15;
            out[k] = scale(out[k], gain);
            fi += fi < SMP_FADE;
        }
        g = f + sf;
        ip += si + (g < f);
        f = g;
    }
    v->pos += (int32_t)ip;
    v->frac = f;
    v->fade_in = fi;
    if (v->pos - BACK >= v->end)        /* every tap of the next frame is past the end */
        v->active = 0;
    return 1;
}

int smp_render(smp_voice *v, int32_t *out, int16_t *scratch)
{
    return smp_render_with(v, out, scratch, smp_coeffs);
}
