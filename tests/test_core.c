/* Host tests of the one-shot core. Built with SMP_DEBUG, so the core also asserts that its inner
   loop stays inside the frames it fetched. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/smp_core.h"

static int failures;

#define CHECK(cond, ...)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            failures++;                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                   \
            printf(__VA_ARGS__);                                          \
            printf("\n");                                                 \
        }                                                                 \
    } while (0)

static uint64_t rng = 0x9E3779B97F4A7C15ull;

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

/* The output frame centred at P (32.32 frames), computed straight from the description in
   smp_core.h with a 64-bit playhead: taps at floor(P)-2 .. floor(P)+3, the row picked by the
   top 8 bits of the fraction, frames outside [start, end) taken as silence. */
static int64_t reference(const int16_t *pcm, int32_t start, int32_t end, uint64_t P)
{
    int64_t i = (int64_t)(P >> 32), acc = 0;
    const int16_t *c = smp_coeffs[(P >> 24) & 0xff];
    int t;

    for (t = 0; t < SMP_TAPS; t++) {
        int64_t j = i - (SMP_TAPS / 2 - 1) + t;
        acc += (int64_t)c[t] * ((j >= start && j < end) ? pcm[j] : 0);
    }
    return acc;
}

/* The fades as smp_core.h describes them: a fade's gain n frames in (Q15, the S curve 3x^2 - 2x^3,
   test_fade_curve holds it against the real curve), the frames until the playhead P reaches the
   end at this step (rounded up), and a frame scaled towards zero by the fade-in n frames after the
   trig and the fade-out rem frames before the end. */
static uint32_t gain(int64_t n)
{
    uint32_t x;

    if (n <= 0)
        return 0;
    if (n >= SMP_FADE)
        return 32768;
    x = (uint32_t)(n * (((1 << 25) + SMP_FADE - 1) / SMP_FADE)) >> 10;
    return ((x * x >> 15) * (3u * 32768 - 2 * x)) >> 15;
}

static int64_t frames_left(int32_t end, uint64_t P, uint64_t step)
{
    uint64_t E = (uint64_t)end << 32;

    return P >= E ? 0 : (int64_t)((E - P + step - 1) / step);
}

static int64_t faded(int64_t raw, int64_t n, int64_t rem)
{
    uint64_t g = gain(n), m = (uint64_t)(raw < 0 ? -raw : raw);

    if (n >= SMP_FADE && rem >= SMP_FADE)
        return raw;
    if (rem < SMP_FADE)
        g = g * gain(rem) >> 15;
    return raw < 0 ? -(int64_t)(m * g >> 15) : (int64_t)(m * g >> 15);
}

static void test_fade_curve(void)
{
    int n, bad = 0, down = 0;

    for (n = 0; n <= SMP_FADE; n++) {
        double x = (double)n / SMP_FADE, s = x * x * (3 - 2 * x);

        bad += fabs(gain(n) / 32768.0 - s) > 5.0 / 32768;     /* integer steps: about 4 at worst */
        down += n > 0 && gain(n) < gain(n - 1);
    }
    CHECK(gain(0) == 0 && gain(SMP_FADE) == 32768 && bad == 0 && down == 0,
          "fade curve: %d points off 3x^2 - 2x^3, %d steps down", bad, down);
}

static void test_table(void)
{
    int32_t worst = 0;
    int p, k;

    for (p = 0; p < SMP_PHASES; p++) {
        int32_t sum = 0, abs_sum = 0;

        for (k = 0; k < SMP_TAPS; k++) {
            sum += smp_coeffs[p][k];
            abs_sum += abs(smp_coeffs[p][k]);
        }
        CHECK(sum == 32768, "row %d sums to %d", p, sum);
        if (abs_sum > worst)
            worst = abs_sum;
    }
    /* |output| <= 32768 * worst must stay below 2^31 */
    CHECK(worst < 65536, "largest absolute row sum %d", worst);
}

static void test_ratio(void)
{
    smp_voice v;

    smp_set_ratio(&v, 0);
    CHECK(v.step_int == 0 && v.step_frac == SMP_MIN_RATIO_Q16 << 15, "low clamp");
    smp_set_ratio(&v, 0xffffffffu);
    CHECK(v.step_int == 4 && v.step_frac == 0, "high clamp");
    smp_set_ratio(&v, 1u << 16);
    CHECK(v.step_int == 0 && v.step_frac == 1u << 31, "ratio 1: half a frame per 96 kHz frame");
}

/* Random samples, windows and ratios; the ratio changes every block. Every frame must equal the
   reference bit for bit, including the frames around start and end, and frames outside the
   window must never leak in (the sample is random there too). In reverse, the voice must play as
   the forward reference plays the sample turned round. */
static void exact(int reverse)
{
    enum { LEN = 12000 };
    static int16_t pcm[LEN], rev[LEN], scratch[SMP_SCRATCH];
    int32_t out[SMP_BLOCK2X];
    int run, k;

    for (k = 0; k < LEN; k++)
        pcm[k] = (int16_t)rnd();
    for (k = 0; k < LEN; k++)
        rev[k] = pcm[LEN - 1 - k];
    for (run = 0; run < 400; run++) {
        int32_t start = (int32_t)(rnd() % LEN), end = start + (int32_t)(rnd() % 3000), from, to;
        int blocks = 0, active = 1;
        int64_t n = 0;                          /* frames since the trig */
        const int16_t *src = reverse ? rev : pcm;
        uint64_t P;
        smp_voice v;

        if (run % 7 == 0)
            start = 0;                          /* window at the very start of the sample */
        if (run % 5 == 0)
            end = LEN;                          /* ... and at its very end */
        if (end > LEN)
            end = LEN;
        from = reverse ? LEN - end : start;     /* the window as the reference plays it */
        to = reverse ? LEN - start : end;
        smp_trigger(&v, pcm, LEN, start, end, reverse ? SMP_REVERSE : 0);
        P = (uint64_t)from << 32;
        while (active && blocks < 100000) {
            uint32_t ratio = SMP_MIN_RATIO_Q16 + rnd() % (SMP_MAX_RATIO_Q16 - SMP_MIN_RATIO_Q16 + 1);
            uint64_t step;

            if (run % 3 == 0)
                ratio = (run % 2) ? SMP_MAX_RATIO_Q16 : 1u << 16;
            smp_set_ratio(&v, ratio);
            step = ((uint64_t)v.step_int << 32) | v.step_frac;
            active = v.active;
            if (smp_render(&v, out, scratch) != active) {
                CHECK(0, "%s run %d block %d: render result differs from active flag", reverse ? "reverse" : "forward",
                      run, blocks);
                break;
            }
            for (k = 0; k < SMP_BLOCK2X; k++) {
                int64_t raw = active ? reference(src, from, to, P) : 0, in = from > 0 ? n : SMP_FADE;
                int64_t rem = frames_left(to, P, step);
                /* the core may count one frame more to the end */
                int64_t want = active ? faded(raw, in, rem) : 0, alt = active ? faded(raw, in, rem + 1) : 0;

                if (out[k] != want && out[k] != alt) {
                    CHECK(0, "%s run %d block %d frame %d: %ld, want %lld", reverse ? "reverse" : "forward", run,
                          blocks, k, (long)out[k], (long long)want);
                    active = 0;
                    break;
                }
                if (active) {
                    P += step;
                    n++;
                }
            }
            blocks++;
        }
        CHECK(!v.active, "run %d: still active after %d blocks", run, blocks);
        /* finished means every tap of the next frame is past the end (an empty window never starts) */
        CHECK(from >= to || (int64_t)(P >> 32) - (SMP_TAPS / 2 - 1) >= to, "run %d: stopped too early",
              run);
    }
}

static void test_exact(void)
{
    exact(0);
    exact(1);
}

/* --- loops ------------------------------------------------------------------------------------ */

#define NO_STOP INT64_MAX

/* The window in the order a voice plays it: forward from start, or reverse from end - 1. */
static void play_order(int16_t *y, const int16_t *pcm, int32_t start, int32_t end, int reverse)
{
    int32_t k;

    for (k = 0; k < end - start; k++)
        y[k] = reverse ? pcm[end - 1 - k] : pcm[start + k];
}

/* The output frame centred at P (32.32 frames on from the first frame played, never folded back), as
   smp_core.h describes a loop: the window y of len frames again and again up to stop, silence before
   its first frame and from stop on. */
static int64_t ref_loop(const int16_t *y, int32_t len, int64_t stop, uint64_t P)
{
    int64_t i = (int64_t)(P >> 32), acc = 0;
    const int16_t *c = smp_coeffs[(P >> 24) & 0xff];
    int t;

    for (t = 0; t < SMP_TAPS; t++) {
        int64_t j = i - (SMP_TAPS / 2 - 1) + t;
        acc += (int64_t)c[t] * ((j >= 0 && j < stop) ? y[j % len] : 0);
    }
    return acc;
}

static int64_t frames_left64(int64_t stop, uint64_t P, uint64_t step)
{
    uint64_t E;

    if (stop == NO_STOP)
        return INT64_MAX / 2;
    E = (uint64_t)stop << 32;
    return P >= E ? 0 : (int64_t)((E - P + step - 1) / step);
}

/* A window of len frames somewhere in a sample of size frames: tiny ones (a pass shorter than a
   block at the top ratio), short ones and long ones; now and then at the sample's very start or end. */
static void some_window(int run, int32_t size, int32_t *start, int32_t *end)
{
    int32_t len = run % 4 == 0 ? 1 + (int32_t)(rnd() % 8) : run % 4 == 1 ? 1 + (int32_t)(rnd() % 300)
                                                             : 1 + (int32_t)(rnd() % 3000);

    *start = (int32_t)(rnd() % (uint32_t)(size - len + 1));
    if (run % 6 == 0)
        *start = 0;
    if (run % 6 == 3)
        *start = size - len;
    *end = *start + len;
}

/* Looping from the trig on, both directions, the ratio changing every block: every frame is the
   window again and again with a hard seam (the taps read across it), the overshoot carried over, no
   fade-out; the voice never ends. */
static void test_loop_exact(void)
{
    enum { LEN = 9000 };
    static int16_t pcm[LEN], y[LEN], scratch[SMP_SCRATCH];
    int32_t out[SMP_BLOCK2X];
    int run, k;

    for (k = 0; k < LEN; k++)
        pcm[k] = (int16_t)rnd();
    for (run = 0; run < 400; run++) {
        int32_t start, end, len;
        int reverse = run & 1, blocks, bad = 0, wraps = 0, fade_in;
        int64_t n = 0;
        uint64_t P = 0;
        smp_voice v;

        some_window(run, LEN, &start, &end);
        len = end - start;
        fade_in = reverse ? end < LEN : start > 0;
        play_order(y, pcm, start, end, reverse);
        smp_trigger(&v, pcm, LEN, start, end, SMP_LOOP | (reverse ? SMP_REVERSE : 0));
        for (blocks = 0; blocks < 60 && !bad; blocks++) {
            uint32_t ratio = SMP_MIN_RATIO_Q16 + rnd() % (SMP_MAX_RATIO_Q16 - SMP_MIN_RATIO_Q16 + 1);
            uint64_t step;

            if (run % 3 == 0)
                ratio = SMP_MAX_RATIO_Q16;
            smp_set_ratio(&v, ratio);
            step = ((uint64_t)v.step_int << 32) | v.step_frac;
            if (smp_render(&v, out, scratch) != 1 || !v.active) {
                CHECK(0, "loop run %d block %d: the voice stopped", run, blocks);
                break;
            }
            for (k = 0; k < SMP_BLOCK2X; k++, n++, P += step) {
                int64_t want = faded(ref_loop(y, len, NO_STOP, P), fade_in ? n : SMP_FADE, SMP_FADE);

                if (out[k] != want) {
                    CHECK(0, "loop run %d (len %d %s) block %d frame %d: %ld, want %lld", run, (int)len,
                          reverse ? "reverse" : "forward", blocks, k, (long)out[k], (long long)want);
                    bad = 1;
                    break;
                }
            }
            CHECK(v.pos >= 0 && v.pos < len && (uint64_t)v.pos == (P >> 32) % (uint64_t)len && v.frac == (uint32_t)P,
                  "loop run %d block %d: playhead %d.%08x, want %llu.%08x", run, blocks, (int)v.pos, v.frac,
                  (unsigned long long)((P >> 32) % (uint64_t)len), (uint32_t)P);
            wraps = (int)((P >> 32) / (uint64_t)len);
        }
        CHECK(bad || wraps > 0 || len > 2000, "loop run %d: never went round (len %d)", run, (int)len);
    }
}

/* The loop switched on and off at random blocks, one ratio per run. After each block the voice's end
   is checked against smp_core.h: switched on, it loops unless its fade-out has begun; switched off
   while it loops, it ends at the end of the first pass at least SMP_FADE frames ahead (a pass end,
   far enough for the whole fade, and no pass end before it would have been, give or take the core's
   rounding of 3 frames). Every frame must equal the reference with that end, and the voice must end
   exactly when every tap has passed it. */
static void test_loop_switch(void)
{
    enum { LEN = 9000 };
    static int16_t pcm[LEN], y[LEN], scratch[SMP_SCRATCH];
    int32_t out[SMP_BLOCK2X];
    int run, k, offs = 0, ons = 0, ended = 0;

    for (k = 0; k < LEN; k++)
        pcm[k] = (int16_t)rnd();
    for (run = 0; run < 600; run++) {
        int32_t start, end, len;
        int reverse = run & 1, blocks, bad = 0, fade_in, loop = (run >> 1) & 1;
        int64_t n = 0, stop;
        uint32_t ratio = SMP_MIN_RATIO_Q16 + rnd() % (SMP_MAX_RATIO_Q16 - SMP_MIN_RATIO_Q16 + 1);
        uint64_t P = 0, step, need;
        smp_voice v;

        if (run % 5 == 0)
            ratio = run % 10 ? SMP_MAX_RATIO_Q16 : SMP_MIN_RATIO_Q16;
        some_window(run, LEN, &start, &end);
        len = end - start;
        fade_in = reverse ? end < LEN : start > 0;
        play_order(y, pcm, start, end, reverse);
        smp_trigger(&v, pcm, LEN, start, end, (loop ? SMP_LOOP : 0) | (reverse ? SMP_REVERSE : 0));
        smp_set_ratio(&v, ratio);
        step = ((uint64_t)v.step_int << 32) | v.step_frac;
        need = SMP_FADE * step;                 /* what the fade-out takes, 32.32 frames */
        stop = loop ? NO_STOP : len;
        for (blocks = 0; blocks < 3000 && v.active && !bad; blocks++) {
            int64_t base = (int64_t)(P >> 32) - v.pos, was = stop, left = frames_left64(stop, P, step);

            if (rnd() % 6 == 0)
                loop = !loop;
            smp_set_loop(&v, loop);
            if (smp_render(&v, out, scratch) != 1) {
                CHECK(0, "switch run %d block %d: an active voice rendered nothing", run, blocks);
                break;
            }
            stop = v.stop == SMP_LOOPING ? NO_STOP : base + v.stop;
            if (loop && was != NO_STOP) {
                CHECK(left >= SMP_FADE + 1 ? stop == NO_STOP : left <= SMP_FADE - 2 ? stop == was
                                                              : stop == NO_STOP || stop == was,
                      "switch run %d block %d: switched on %lld frames before the end, end %lld (was %lld)", run,
                      blocks, (long long)left, (long long)stop, (long long)was);
                ons += stop == NO_STOP;
            } else if (loop) {
                CHECK(stop == NO_STOP, "switch run %d block %d: stopped looping while asked to loop", run, blocks);
            } else if (was == NO_STOP) {
                uint64_t ahead = stop == NO_STOP ? 0 : ((uint64_t)stop << 32) - P;

                CHECK(stop != NO_STOP && stop % len == 0 && ahead >= need && ahead < need + ((uint64_t)(len + 3) << 32),
                      "switch run %d block %d: switched off, end %lld (len %d, %.2f frames ahead, the fade %.2f)",
                      run, blocks, (long long)stop, (int)len, ahead / 4294967296.0, need / 4294967296.0);
                offs++;
            } else {
                CHECK(stop == was, "switch run %d block %d: the end moved while not looping", run, blocks);
            }
            for (k = 0; k < SMP_BLOCK2X; k++, n++, P += step) {
                int64_t raw = ref_loop(y, len, stop, P), in = fade_in ? n : SMP_FADE, rem = frames_left64(stop, P, step);
                int64_t want = faded(raw, in, rem), alt = faded(raw, in, rem + 1);

                if (out[k] != want && out[k] != alt) {
                    CHECK(0, "switch run %d (len %d %s) block %d frame %d: %ld, want %lld", run, (int)len,
                          reverse ? "reverse" : "forward", blocks, k, (long)out[k], (long long)want);
                    bad = 1;
                    break;
                }
            }
            CHECK(bad || v.active == (stop == NO_STOP || (int64_t)(P >> 32) - (SMP_TAPS / 2 - 1) < stop),
                  "switch run %d block %d: active %d at %lld, end %lld", run, blocks, (int)v.active,
                  (long long)(P >> 32), (long long)stop);
        }
        ended += !v.active;
    }
    CHECK(offs > 300 && ons > 100 && ended > 100, "switch: only %d offs, %d ons, %d ended", offs, ons, ended);
}

/* A single cycle of a sine as the whole sample, looped: after the first frames (whose taps reach
   before the sample) it plays as a continuous sine at any pitch, the seam included. Measured as what
   is left once the best sine of that frequency is taken out (the table's own gain and phase at that
   frequency are not the loop's business). */
static void test_single_cycle(void)
{
    static const int32_t sizes[] = { 64, 100, 441, 600 };
    static const uint32_t ratios[] = { 1u << 16, 1u << 15, 98304, 242483, SMP_MAX_RATIO_Q16 / 2 };
    static int16_t pcm[600], scratch[SMP_SCRATCH];
    static double got[200 * SMP_BLOCK2X], ph[200 * SMP_BLOCK2X];
    int32_t out[SMP_BLOCK2X];
    double worst = 1e9;
    unsigned i, r;
    int k, b, j;

    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        int32_t size = sizes[i];

        for (k = 0; k < size; k++)
            pcm[k] = (int16_t)lrint(30000.0 * sin(2 * M_PI * k / size));
        for (r = 0; r < sizeof ratios / sizeof ratios[0]; r++) {
            double ss = 0, cc = 0, sc = 0, ys = 0, yc = 0, a, c, det, sig = 0, err = 0, snr;
            uint64_t P = 0, step;
            int m = 0;
            smp_voice v;

            smp_trigger(&v, pcm, size, 0, size, SMP_LOOP);
            smp_set_ratio(&v, ratios[r]);
            step = ((uint64_t)v.step_int << 32) | v.step_frac;
            for (b = 0; b < 200; b++) {
                smp_render(&v, out, scratch);
                for (k = 0; k < SMP_BLOCK2X; k++, P += step) {
                    double x = (double)P / 4294967296.0;

                    if (x < 4)
                        continue;
                    got[m] = out[k];
                    ph[m++] = 2 * M_PI * fmod(x, size) / size;
                }
            }
            for (j = 0; j < m; j++) {
                double sn = sin(ph[j]), cs = cos(ph[j]);

                ss += sn * sn;
                cc += cs * cs;
                sc += sn * cs;
                ys += got[j] * sn;
                yc += got[j] * cs;
            }
            det = ss * cc - sc * sc;
            a = (ys * cc - yc * sc) / det;
            c = (yc * ss - ys * sc) / det;
            for (j = 0; j < m; j++) {
                double fit = a * sin(ph[j]) + c * cos(ph[j]);

                sig += fit * fit;
                err += (got[j] - fit) * (got[j] - fit);
            }
            snr = 10 * log10(sig / err);
            if (snr < worst)
                worst = snr;
            CHECK(snr > 60, "single cycle of %d frames at ratio %.3f: %.1f dB besides the sine", (int)size,
                  ratios[r] / 65536.0, snr);
        }
    }
    printf("single cycles: %.1f dB besides the sine at worst\n", worst);
}

/* A DC sample, the loop switched on and off at random, both directions: wherever the window starts
   inside the sample (so the trig fades in), the level never steps by more than the fades' S curve
   does from one frame to the next, at the seams, at the switches or at the end; and the voice ends
   in silence. */
static void test_loop_no_click(void)
{
    enum { LEN = 6000, LEVEL = 20000 };
    static int16_t pcm[LEN], scratch[SMP_SCRATCH];
    int32_t out[SMP_BLOCK2X];
    int64_t bound, worst = 0;
    uint32_t steepest = 0;
    int run, k, n;

    for (k = 0; k < LEN; k++)
        pcm[k] = LEVEL;
    for (n = 1; n <= SMP_FADE; n++)
        if (gain(n) - gain(n - 1) > steepest)
            steepest = gain(n) - gain(n - 1);
    bound = (int64_t)LEVEL * (2 * steepest + 2);        /* both fades at once in a short window */
    for (run = 0; run < 400; run++) {
        int32_t start, end, last = 0;
        int reverse = run & 1, loop = 1, blocks;
        uint32_t ratio = SMP_MIN_RATIO_Q16 + rnd() % (SMP_MAX_RATIO_Q16 - SMP_MIN_RATIO_Q16 + 1);
        smp_voice v;

        some_window(run, LEN - 2, &start, &end);
        start += 1;                             /* inside the sample on both sides: the trig fades in */
        end += 1;
        smp_trigger(&v, pcm, LEN, start, end, SMP_LOOP | (reverse ? SMP_REVERSE : 0));
        smp_set_ratio(&v, ratio);
        /* 3000 blocks of switching at random, then off until it ends (a pass lasts 6000 blocks at most) */
        for (blocks = 0; blocks < 20000 && smp_render(&v, out, scratch); blocks++) {
            for (k = 0; k < SMP_BLOCK2X; k++) {
                int64_t d = (int64_t)out[k] - last;

                if (d < 0)
                    d = -d;
                if (d > worst)
                    worst = d;
                last = out[k];
            }
            if (rnd() % 5 == 0)
                loop = !loop;
            smp_set_loop(&v, loop && blocks < 3000);
        }
        CHECK(!v.active && last == 0, "no click run %d: active %d, ended at %ld", run, (int)v.active, (long)last);
    }
    CHECK(worst <= bound, "no click: the level stepped by %lld, the fades by %lld at most", (long long)worst,
          (long long)bound);
}

/* A window of one frame loops as DC, either way round, and never ends. */
static void test_loop_tiny(void)
{
    static int16_t pcm[10], scratch[SMP_SCRATCH];
    int32_t out[SMP_BLOCK2X];
    int k, b, j, off = 0;

    for (k = 0; k < 10; k++)
        pcm[k] = (int16_t)(1000 * k);
    for (j = 0; j < 2; j++) {
        smp_voice v;

        smp_trigger(&v, pcm, 10, 4, 5, SMP_LOOP | (j ? SMP_REVERSE : 0));
        smp_set_ratio(&v, SMP_MAX_RATIO_Q16);
        for (b = 0; b < 50; b++) {
            CHECK(smp_render(&v, out, scratch) == 1, "one frame, %s: stopped", j ? "reverse" : "forward");
            for (k = 0; k < SMP_BLOCK2X; k++)         /* after the fade-in: the frame is inside the sample */
                off += b * SMP_BLOCK2X + k >= SMP_FADE + 8 && out[k] != 4000 * 32768;
        }
    }
    CHECK(off == 0, "one frame looped: %d frames off its level", off);
}

/* DC: wherever all six taps lie inside the window and the fade-out has not begun, the frame is
   exactly level << 15. */
static void test_dc(void)
{
    enum { LEN = 5000 };
    static int16_t pcm[LEN], scratch[SMP_SCRATCH];
    static const uint32_t ratios[] = { 1u << 10, 30000, 1u << 16, 99999, 1u << 19 };
    int32_t out[SMP_BLOCK2X];
    unsigned r;
    int k;

    for (k = 0; k < LEN; k++)
        pcm[k] = -21000;
    for (r = 0; r < sizeof ratios / sizeof ratios[0]; r++) {
        smp_voice v;
        uint64_t P = 0, step;
        int bad = 0;

        smp_trigger(&v, pcm, LEN, 0, LEN, 0);
        smp_set_ratio(&v, ratios[r]);
        step = ((uint64_t)v.step_int << 32) | v.step_frac;
        while (smp_render(&v, out, scratch)) {
            for (k = 0; k < SMP_BLOCK2X; k++, P += step) {
                int64_t i = (int64_t)(P >> 32);

                if (i - 2 >= 0 && i + 3 < LEN && frames_left(LEN, P, step) > SMP_FADE && out[k] != -21000 * 32768)
                    bad++;
            }
        }
        CHECK(bad == 0, "ratio %u: %d frames off the DC level", ratios[r], bad);
    }
}

/* The fades on a DC sample: a start past frame 0 fades in from silence, half way at half the
   fade; a start at frame 0 does not fade in; the fade-out takes SMP_FADE frames at every pitch. */
static void test_fades(void)
{
    enum { LEN = 20000, LEVEL = 20000 };
    static int16_t pcm[LEN], scratch[SMP_SCRATCH];
    static const uint32_t ratios[] = { 1u << 15, 1u << 16, 3u << 16, SMP_MAX_RATIO_Q16 };
    int32_t out[SMP_BLOCK2X], frames[2 * SMP_FADE];
    unsigned r;
    int k, j;

    for (k = 0; k < LEN; k++)
        pcm[k] = LEVEL;
    for (j = 0; j < 2; j++) {
        smp_voice v;
        int n = 0;

        smp_trigger(&v, pcm, LEN, j ? 0 : 100, LEN, 0);
        smp_set_ratio(&v, 1u << 16);
        while (n < 2 * SMP_FADE && smp_render(&v, out, scratch))
            for (k = 0; k < SMP_BLOCK2X; k++)
                frames[n++] = out[k];
        if (j == 0)
            CHECK(frames[0] == 0 && fabs(frames[SMP_FADE / 2] / (LEVEL * 32768.0) - 0.5) < 0.001 &&
                  frames[SMP_FADE + 8] == LEVEL * 32768, "start 100: fades in over %d frames", SMP_FADE);
        else
            CHECK(frames[8] == LEVEL * 32768, "start 0: no fade-in");
    }
    for (r = 0; r < sizeof ratios / sizeof ratios[0]; r++) {
        smp_voice v;
        int first = -1, last = -1, n = 0;

        smp_trigger(&v, pcm, LEN, 0, LEN, 0);
        smp_set_ratio(&v, ratios[r]);
        while (smp_render(&v, out, scratch))
            for (k = 0; k < SMP_BLOCK2X; k++, n++) {
                if (n > 8 && out[k] != LEVEL * 32768 && first < 0)
                    first = n;
                if (out[k])
                    last = n;
            }
        /* the S curve stays below one step of the level for its first and last 2 frames */
        CHECK(first > 0 && last - first + 1 >= SMP_FADE - 5 && last - first + 1 <= SMP_FADE + 1,
              "ratio %u: the fade-out takes %d frames", ratios[r], last - first + 1);
    }
}

/* A finished voice renders silence and says so; so does an empty window. */
static void test_finished(void)
{
    static int16_t pcm[100], scratch[SMP_SCRATCH];
    int32_t out[SMP_BLOCK2X];
    smp_voice v;
    int k, nonzero = 0;

    for (k = 0; k < 100; k++)
        pcm[k] = 30000;
    smp_trigger(&v, pcm, 100, 40, 40, 0);
    smp_set_ratio(&v, 1u << 16);
    CHECK(!v.active && smp_render(&v, out, scratch) == 0, "empty window plays");
    smp_trigger(&v, pcm, 100, 90, 500, 0);
    CHECK(v.end == 100, "end not clamped to the length");
    smp_set_ratio(&v, SMP_MAX_RATIO_Q16);
    CHECK(smp_render(&v, out, scratch) == 1 && !v.active, "short window: one block, then finished");
    memset(out, 0x55, sizeof out);
    CHECK(smp_render(&v, out, scratch) == 0, "finished voice still plays");
    for (k = 0; k < SMP_BLOCK2X; k++)
        nonzero += out[k] != 0;
    CHECK(nonzero == 0, "finished voice leaves %d frames non-zero", nonzero);
}

int main(void)
{
    test_fade_curve();
    test_table();
    test_ratio();
    test_exact();
    test_dc();
    test_fades();
    test_finished();
    test_loop_exact();
    test_loop_switch();
    test_single_cycle();
    test_loop_no_click();
    test_loop_tiny();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
