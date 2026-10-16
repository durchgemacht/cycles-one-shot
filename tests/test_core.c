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
   window must never leak in (the sample is random there too). */
static void test_exact(void)
{
    enum { LEN = 12000 };
    static int16_t pcm[LEN], scratch[SMP_SCRATCH];
    int32_t out[SMP_BLOCK2X];
    int run, k;

    for (k = 0; k < LEN; k++)
        pcm[k] = (int16_t)rnd();
    for (run = 0; run < 400; run++) {
        int32_t start = (int32_t)(rnd() % LEN), end = start + (int32_t)(rnd() % 3000);
        int blocks = 0, active = 1;
        int64_t n = 0;                          /* frames since the trig */
        uint64_t P;
        smp_voice v;

        if (run % 7 == 0)
            start = 0;                          /* window at the very start of the sample */
        if (run % 5 == 0)
            end = LEN;                          /* ... and at its very end */
        if (end > LEN)
            end = LEN;
        smp_trigger(&v, pcm, LEN, start, end);
        P = (uint64_t)start << 32;
        while (active && blocks < 100000) {
            uint32_t ratio = SMP_MIN_RATIO_Q16 + rnd() % (SMP_MAX_RATIO_Q16 - SMP_MIN_RATIO_Q16 + 1);
            uint64_t step;

            if (run % 3 == 0)
                ratio = (run % 2) ? SMP_MAX_RATIO_Q16 : 1u << 16;
            smp_set_ratio(&v, ratio);
            step = ((uint64_t)v.step_int << 32) | v.step_frac;
            active = v.active;
            if (smp_render(&v, out, scratch) != active) {
                CHECK(0, "run %d block %d: render result differs from active flag", run, blocks);
                break;
            }
            for (k = 0; k < SMP_BLOCK2X; k++) {
                int64_t raw = active ? reference(pcm, start, end, P) : 0, in = start > 0 ? n : SMP_FADE;
                int64_t rem = frames_left(end, P, step);
                /* the core may count one frame more to the end */
                int64_t want = active ? faded(raw, in, rem) : 0, alt = active ? faded(raw, in, rem + 1) : 0;

                if (out[k] != want && out[k] != alt) {
                    CHECK(0, "run %d block %d frame %d: %ld, want %lld", run, blocks, k, (long)out[k],
                          (long long)want);
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
        CHECK(start >= end || (int64_t)(P >> 32) - (SMP_TAPS / 2 - 1) >= end, "run %d: stopped too early",
              run);
    }
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

        smp_trigger(&v, pcm, LEN, 0, LEN);
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

        smp_trigger(&v, pcm, LEN, j ? 0 : 100, LEN);
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

        smp_trigger(&v, pcm, LEN, 0, LEN);
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
    smp_trigger(&v, pcm, 100, 40, 40);
    smp_set_ratio(&v, 1u << 16);
    CHECK(!v.active && smp_render(&v, out, scratch) == 0, "empty window plays");
    smp_trigger(&v, pcm, 100, 90, 500);
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
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
