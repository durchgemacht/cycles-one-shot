/* Host tests of the filter (src/smp_filter.c): the table, the laws it follows, the fixed-point
   loop against the exact response of the poles it is meant to place, the glide, the starts, and
   saturation. The loop here is the C model of the ColdFire's EMAC arithmetic (smp_filter.h). */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/smp_filter.h"

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

#define FS 48000.0
#define PI 3.14159265358979323846

/* The laws of tools/gen_filter_table.py, for the poles a setting means to place. */
static void poles(int32_t i, int32_t reso, double *r, double *theta)
{
    double fc = 4.918 * pow(4400.0, i / 16384.0);
    double w0 = 2 * PI * fc / FS;
    double red = 1 - pow(fc / 24000.0, 2.5);
    double Q = 0.4918 * pow(43.8, reso * red / 32512.0);

    *r = exp(-w0 / (2 * Q));
    *theta = w0 * (1 - 0.70 * exp(-reso / 3300.0) - 0.233 * exp(-reso / 381.0));
}

/* |H| of the lowpass G / (1 - 2 r cos th z^-1 + r^2 z^-2) (unity at DC) or of 1 minus it; within
   a knob step of the centre only a share of it is heard, y = x + wet (H x - x), wet the distance
   to the centre over 256. */
static double response(int32_t cutoff, int32_t reso, double f)
{
    int hp = cutoff > SMP_FILTER_CENTRE;
    double r, theta, g, w = 2 * PI * f / FS;
    double re, im, dre, dim, mag2;
    double wet = fabs((double)(cutoff - SMP_FILTER_CENTRE)) / 256;

    if (wet > 1)
        wet = 1;

    poles(hp ? cutoff - SMP_FILTER_CENTRE : cutoff, reso, &r, &theta);
    g = 1 - 2 * r * cos(theta) + r * r;
    /* den = 1 - 2 r cos th e^-jw + r^2 e^-2jw */
    dre = 1 - 2 * r * cos(theta) * cos(w) + r * r * cos(2 * w);
    dim = 2 * r * cos(theta) * sin(w) - r * r * sin(2 * w);
    mag2 = dre * dre + dim * dim;
    re = g * dre / mag2;                /* H = g / den = g * conj(den) / |den|^2 */
    im = -g * dim / mag2;
    if (hp) {
        re = 1 - re;
        im = -im;
    }
    re = 1 + wet * (re - 1);
    im = wet * im;
    return sqrt(re * re + im * im);
}

/* The amplitude the fixed-point filter gives a sine of frequency f at the setting, measured after
   it has settled: a least-squares fit of sin and cos over whole cycles. The sine is at a hundredth
   of full scale, so even a +27 dB peak stays well inside Q31. */
static double measure(int32_t cutoff, int32_t reso, double f)
{
    smp_filter flt;
    double r, theta, amp = 0.01 * 2147483648.0, ws, wc;
    int hp = cutoff > SMP_FILTER_CENTRE;
    long settle, n, cycles, k;
    int32_t buf[SMP_FILTER_BLOCK];

    poles(hp ? cutoff - SMP_FILTER_CENTRE : cutoff, reso, &r, &theta);
    settle = (long)(12.0 / (1 - r)) + 4096;         /* 12 time constants of the slowest decay */
    if (settle > 1200000)
        settle = 1200000;
    cycles = (long)ceil(4096 * f / FS);
    if (cycles < 2)
        cycles = 2;
    n = (long)(cycles * FS / f);
    smp_filter_reset(&flt);
    ws = wc = 0;
    for (k = 0; k < settle + n; k += SMP_FILTER_BLOCK) {
        int j;

        for (j = 0; j < SMP_FILTER_BLOCK; j++)
            buf[j] = (int32_t)lrint(amp * sin(2 * PI * f * (k + j) / FS));
        smp_filter_set(&flt, cutoff, reso);
        smp_filter_run(&flt, buf);
        for (j = 0; j < SMP_FILTER_BLOCK; j++) {
            long t = k + j - settle;

            if (t >= 0 && t < n) {
                ws += buf[j] * sin(2 * PI * f * (k + j) / FS);
                wc += buf[j] * cos(2 * PI * f * (k + j) / FS);
            }
        }
    }
    return sqrt(ws * ws + wc * wc) * 2 / n / amp;
}

static double db(double x)
{
    return 20 * log10(x);
}

static void test_table(void)
{
    static const int32_t grid[][2] = { { 0, 0 }, { 8192, 0 }, { 8192, 16384 }, { 16384, 32768 }, { 4096, 2048 } };
    int ri, ci;
    unsigned k;

    for (ri = 0; ri < SMP_FILTER_NR; ri++)
        for (ci = 0; ci < SMP_FILTER_NI; ci++) {
            int32_t f2 = smp_filter_tab[ri][ci][0], q2 = smp_filter_tab[ri][ci][1];

            CHECK(f2 > 0 && q2 > 0, "table %d %d: %d %d", ri, ci, f2, q2);
        }
    /* the table's values against the laws at grid points: f/2 and q/2 to 1 LSB */
    for (k = 0; k < sizeof grid / sizeof grid[0]; k++) {
        double r, theta, f, q;
        const int32_t *e = smp_filter_tab[grid[k][1] >> 11][grid[k][0] >> 7];

        poles(grid[k][0], grid[k][1], &r, &theta);
        f = sqrt(1 - 2 * r * cos(theta) + r * r);
        q = (1 - r * r) / f;
        CHECK(labs(e[0] - lrint(f / 2 * 2147483648.0)) <= 1, "f at %d/%d", grid[k][0], grid[k][1]);
        CHECK(labs(e[1] - lrint(q / 2 * 2147483648.0)) <= 1, "q at %d/%d", grid[k][0], grid[k][1]);
    }
}

/* The fixed-point filter against the exact response of the poles its setting means to place:
   lowpass and highpass, resonances from none to full, cutoffs from the bottom to the top, probed
   around the cutoff and across the band. */
static void test_response(void)
{
    static const int32_t cuts[] = { 0, 2048, 4096, 8192, 12288, 15360, 16128, 16383,
                                    16640, 20480, 24576, 28672, 31744, 32512 };
    static const int32_t resos[] = { 0, 4096, 16256, 24576, 32512 };
    static const double mult[] = { 0.25, 0.5, 0.8, 1.0, 1.25, 2.0, 4.0 };
    double worst = 0;
    int worst_cut = 0, worst_reso = 0;
    double worst_f = 0;
    unsigned a, b, c;

    for (a = 0; a < sizeof cuts / sizeof *cuts; a++)
        for (b = 0; b < sizeof resos / sizeof *resos; b++) {
            int32_t cut = cuts[a], reso = resos[b];
            int hp = cut > SMP_FILTER_CENTRE;
            double fc = 4.918 * pow(4400.0, (hp ? cut - SMP_FILTER_CENTRE : cut) / 16384.0);

            for (c = 0; c < sizeof mult / sizeof *mult; c++) {
                double f = fc * mult[c], want, got, dev;

                if (f < 8 || f > 20000)
                    continue;
                want = response(cut, reso, f);
                if (want < 0.01)                    /* below -40 dB: the noise floor, not the law */
                    continue;
                got = measure(cut, reso, f);
                dev = fabs(db(got) - db(want));
                if (dev > worst) {
                    worst = dev;
                    worst_cut = cut;
                    worst_reso = reso;
                    worst_f = f;
                }
                CHECK(dev < 0.25, "cut %d reso %d at %.1f Hz: %.2f dB, law %.2f dB", cut, reso, f,
                      db(got), db(want));
            }
        }
    printf("  response: worst %.3f dB at cut %d reso %d, %.1f Hz\n", worst, worst_cut, worst_reso, worst_f);
}

/* Off leaves the block alone; the centre is off; the lowpass starts at the signal and the
   highpass from rest, so a step into either side does not click (the EMAC's truncation leaves
   a frame a few units short of 2^-31); back at the centre, wet fades out over one block before
   the filter is off. */
static void test_off_and_starts(void)
{
    smp_filter flt;
    int32_t buf[SMP_FILTER_BLOCK], ref[SMP_FILTER_BLOCK];
    int j;

    for (j = 0; j < SMP_FILTER_BLOCK; j++)
        buf[j] = ref[j] = (j * 7919) % 100000 - 50000;
    smp_filter_reset(&flt);
    smp_filter_set(&flt, SMP_FILTER_CENTRE, 32512);
    CHECK(flt.mode == SMP_FILTER_OFF, "centre is off");
    smp_filter_run(&flt, buf);
    CHECK(!memcmp(buf, ref, sizeof buf), "off changes the block");
    CHECK(!smp_filter_ringing(&flt), "off rings");

    /* lowpass near the top on a constant: output equals it from the first frame */
    for (j = 0; j < SMP_FILTER_BLOCK; j++)
        buf[j] = 300000000;
    smp_filter_set(&flt, 16000, 0);
    smp_filter_run(&flt, buf);
    for (j = 0; j < SMP_FILTER_BLOCK; j++)
        CHECK(abs(buf[j] - 300000000) < 64, "lowpass start frame %d: %d", j, buf[j]);
    CHECK(smp_filter_ringing(&flt), "lowpass holding a level does not ring");

    /* back to off: one block lets wet glide to 0 with the side still set, then the filter is
       off, the block untouched and ringing no longer counted */
    smp_filter_set(&flt, SMP_FILTER_CENTRE, 0);
    CHECK(flt.mode == SMP_FILTER_LP && flt.wett == 0 && flt.dwet < 0, "the centre fades out first");
    smp_filter_run(&flt, buf);
    smp_filter_set(&flt, SMP_FILTER_CENTRE, 0);
    CHECK(flt.mode == SMP_FILTER_OFF, "off after the fade");
    CHECK(!smp_filter_ringing(&flt), "off still rings");
    for (j = 0; j < SMP_FILTER_BLOCK; j++)
        buf[j] = ref[j];
    smp_filter_run(&flt, buf);
    CHECK(!memcmp(buf, ref, sizeof buf), "off changes the block");

    /* highpass near the bottom, from off: the first frame passes untouched */
    smp_filter_set(&flt, SMP_FILTER_CENTRE + 300, 0);
    CHECK(flt.prime && flt.wet == 0 && flt.mix == SMP_FILTER_ONE, "the highpass starts from rest");
    for (j = 0; j < SMP_FILTER_BLOCK; j++)
        buf[j] = ref[j];
    smp_filter_run(&flt, buf);
    CHECK(abs(buf[0] - ref[0]) <= 4, "highpass start: %d vs %d", buf[0], ref[0]);
    for (j = 1; j < SMP_FILTER_BLOCK; j++)
        CHECK(abs(buf[j] - ref[j]) < 2000, "highpass at 5.7 Hz frame %d: %d vs %d", j, buf[j], ref[j]);
}

/* A changed setting glides across the block: the coefficients end within 32 LSB of the new
   targets, and the next block starts from the targets. */
static void test_glide(void)
{
    smp_filter flt;
    int32_t buf[SMP_FILTER_BLOCK] = { 0 };
    int32_t f_end, q_end;

    smp_filter_reset(&flt);
    smp_filter_set(&flt, 4000, 1000);
    CHECK(flt.wet == 0 && flt.dwet > 0 && flt.wett == SMP_FILTER_ONE, "wet glides in from off");
    smp_filter_run(&flt, buf);
    smp_filter_set(&flt, 9000, 20000);
    CHECK(flt.df2 != 0 && flt.dq2 != 0, "no glide");
    CHECK(flt.wet == SMP_FILTER_ONE && flt.dwet == 0 && flt.mix == 0 && flt.dmix == 0, "the weights stand");
    f_end = flt.f2 + 32 * flt.df2;
    q_end = flt.q2 + 32 * flt.dq2;
    CHECK(labs(f_end - flt.f2t) < 32 && labs(q_end - flt.q2t) < 32, "glide misses: %d %d", f_end - flt.f2t,
          q_end - flt.q2t);
    smp_filter_run(&flt, buf);
    smp_filter_set(&flt, 9000, 20000);
    CHECK(flt.df2 == 0 && flt.dq2 == 0 && flt.f2 == flt.f2t, "a steady setting still glides");
}

/* A sweep across a block boundary at full resonance: no frame jumps more than the signal itself
   could explain (the glide, not a step). */
static void test_sweep_smooth(void)
{
    smp_filter flt;
    int32_t buf[SMP_FILTER_BLOCK], last = 0;
    int32_t cut = 2000, biggest = 0;
    long k;

    smp_filter_reset(&flt);
    for (k = 0; k < 4800; k += SMP_FILTER_BLOCK) {
        int j;

        for (j = 0; j < SMP_FILTER_BLOCK; j++)
            buf[j] = (int32_t)(200000000.0 * sin(2 * PI * 440 * (k + j) / FS));
        smp_filter_set(&flt, cut, 32512);
        smp_filter_run(&flt, buf);
        for (j = 0; j < SMP_FILTER_BLOCK; j++) {
            if (k && abs(buf[j] - last) > biggest)
                biggest = abs(buf[j] - last);
            last = buf[j];
        }
        cut += 90;                                      /* L56 to L7 in a tenth of a second */
    }
    CHECK(biggest < 120000000, "a sweep steps by %d", biggest);
}

/* Full resonance, a full-scale square: the loop saturates instead of wrapping, and once the input
   stops the ringing dies down to nothing. */
static void test_saturation(void)
{
    smp_filter flt;
    int32_t buf[SMP_FILTER_BLOCK];
    long k;
    int j, blocks_ringing = 0;

    smp_filter_reset(&flt);
    for (k = 0; k < 48000; k += SMP_FILTER_BLOCK) {
        for (j = 0; j < SMP_FILTER_BLOCK; j++)
            buf[j] = ((k + j) / 24) & 1 ? INT32_MAX : -INT32_MAX;
        smp_filter_set(&flt, 8192, 32512);              /* 326 Hz, Q 21.5 */
        smp_filter_run(&flt, buf);
        for (j = 0; j < SMP_FILTER_BLOCK; j++)
            CHECK(abs(j ? buf[j] - buf[j - 1] : 0) < INT32_MAX, "wrap");
    }
    for (k = 0; k < 48000 * 10 && smp_filter_ringing(&flt); k += SMP_FILTER_BLOCK) {
        memset(buf, 0, sizeof buf);
        smp_filter_set(&flt, 8192, 32512);
        smp_filter_run(&flt, buf);
        blocks_ringing++;
    }
    CHECK(!smp_filter_ringing(&flt), "still ringing after 10 s");
    CHECK(blocks_ringing > 10, "no ringing at all after a full-scale drive");
    printf("  saturation: rang for %.2f s after the drive\n", blocks_ringing * 32 / FS);
}

/* The blending loop at wet = 1 and mix = 0 or 1 is the plain lowpass or highpass loop: the same
   states afterwards, the output within a few LSB (the blend's two extra roundings). */
static void test_loops_agree(void)
{
    smp_filter a, b;
    int32_t bufa[SMP_FILTER_BLOCK], bufb[SMP_FILTER_BLOCK];
    int side, j, k;

    for (side = 0; side < 2; side++) {
        int32_t cut = side ? 20480 : 8192;

        smp_filter_reset(&a);
        for (k = 0; k < 100; k++) {                     /* settle in the steady loop */
            for (j = 0; j < SMP_FILTER_BLOCK; j++)
                bufa[j] = (int32_t)(1000000000.0 * sin(2 * PI * 700 * (k * 32 + j) / FS));
            smp_filter_set(&a, cut, 24576);
            smp_filter_run(&a, bufa);
        }
        CHECK(a.loop == (side ? SMP_FILTER_LOOP_HP : SMP_FILTER_LOOP_LP), "side %d: not the steady loop", side);
        b = a;
        b.loop = SMP_FILTER_LOOP_MIX;
        for (j = 0; j < SMP_FILTER_BLOCK; j++)
            bufa[j] = bufb[j] = (int32_t)(1000000000.0 * sin(2 * PI * 700 * (100 * 32 + j) / FS));
        smp_filter_frames(&a, bufa);
        smp_filter_frames(&b, bufb);
        CHECK(a.l == b.l && a.b == b.b, "side %d: the loops' states differ", side);
        for (j = 0; j < SMP_FILTER_BLOCK; j++)
            CHECK(abs(bufa[j] - bufb[j]) <= 4, "side %d frame %d: %d vs %d", side, j, bufa[j], bufb[j]);
    }
}

/* The knob jumping from one side to the other every 16 blocks, as a square LFO does, at full
   resonance: the states carry on and the output blends from lp to hp across the jump's block,
   so no frame there steps further than the filter's own frames do once settled. The same jumps
   with the states cleared at each one, as 0.5 did, step several times further. */
static void test_side_jump(void)
{
    smp_filter flt, old;
    int32_t buf[SMP_FILTER_BLOCK], ref[SMP_FILTER_BLOCK], last = 0, last_old = 0;
    int32_t steady = 0, jump = 0, jump_old = 0;
    long k;
    int blk = 0;

    smp_filter_reset(&flt);
    smp_filter_reset(&old);
    for (k = 0; k < 48000; k += SMP_FILTER_BLOCK, blk++) {
        int32_t cut = (blk / 16) & 1 ? 24064 : 8704;    /* H30 (252 Hz) and L30 (426 Hz) */
        int j, at_jump = blk && blk % 16 == 0;

        for (j = 0; j < SMP_FILTER_BLOCK; j++)
            buf[j] = ref[j] = (int32_t)(200000000.0 * sin(2 * PI * 330 * (k + j) / FS));
        smp_filter_set(&flt, cut, 32512);
        smp_filter_set(&old, cut, 32512);
        if (at_jump) {
            CHECK(flt.dmix != 0 && (flt.l || flt.b) && !flt.prime, "block %d: the jump resets", blk);
            old.l = old.b = 0;                          /* 0.5: a fresh start on the other side */
            old.prime = 1;
            old.mix = old.mixt;
            old.dmix = 0;
        }
        smp_filter_run(&flt, buf);
        smp_filter_run(&old, ref);
        for (j = 0; j < SMP_FILTER_BLOCK; j++) {
            int32_t step = abs(buf[j] - last), step_old = abs(ref[j] - last_old);

            if (at_jump) {
                if (step > jump)
                    jump = step;
                if (step_old > jump_old)
                    jump_old = step_old;
            } else if (blk % 16 >= 4 && step > steady)
                steady = step;
            last = buf[j];
            last_old = ref[j];
        }
    }
    printf("  side jump: biggest step %d in the jump's block, %d settled, %d with a reset\n", jump, steady, jump_old);
    CHECK(jump <= steady, "a jump steps by %d, settled frames by %d", jump, steady);
    CHECK(jump_old > 2 * jump, "a reset would step by %d, the blend by %d", jump_old, jump);
}

/* Within a knob step of the centre a share of the filter is heard, the distance over 256: at
   128 above the centre on a constant, where the highpass passes nothing once settled, half of
   the input is left. */
static void test_centre_fade(void)
{
    smp_filter flt;
    int32_t buf[SMP_FILTER_BLOCK];
    long k;
    int j;

    smp_filter_reset(&flt);
    smp_filter_set(&flt, SMP_FILTER_CENTRE + 64, 0);
    CHECK(flt.wett == 64 << 23, "wet at a quarter of a step: %d", flt.wett);
    smp_filter_set(&flt, SMP_FILTER_CENTRE - 256, 0);
    CHECK(flt.wett == SMP_FILTER_ONE, "wet at a step: %d", flt.wett);
    smp_filter_reset(&flt);
    for (k = 0; k < 96000; k += SMP_FILTER_BLOCK) {
        for (j = 0; j < SMP_FILTER_BLOCK; j++)
            buf[j] = 1 << 28;
        smp_filter_set(&flt, SMP_FILTER_CENTRE + 128, 0);
        smp_filter_run(&flt, buf);
    }
    CHECK(flt.loop == SMP_FILTER_LOOP_MIX, "the blend loop runs within the step");
    CHECK(abs(buf[SMP_FILTER_BLOCK - 1] - (1 << 27)) < 1 << 20, "half way at 128 above the centre: %d",
          buf[SMP_FILTER_BLOCK - 1]);
}

/* The knob set to 64 from a side: wet glides to 0 across one block with the side and its states
   kept, so a full-resonance lowpass fades into the dry signal instead of stepping; from the next
   block the filter is off and the block passes bit for bit. */
static void test_off_glide(void)
{
    smp_filter flt;
    int32_t buf[SMP_FILTER_BLOCK], ref[SMP_FILTER_BLOCK];
    double first = 0;
    long k;
    int j;

    smp_filter_reset(&flt);
    for (k = 0; k < 4800; k += SMP_FILTER_BLOCK) {
        for (j = 0; j < SMP_FILTER_BLOCK; j++)
            buf[j] = (int32_t)(200000000.0 * sin(2 * PI * 3100 * (k + j) / FS));
        smp_filter_set(&flt, 8704, 32512);              /* L30, 426 Hz: little of 3.1 kHz passes */
        smp_filter_run(&flt, buf);
    }
    for (j = 0; j < SMP_FILTER_BLOCK; j++)
        buf[j] = ref[j] = (int32_t)(200000000.0 * sin(2 * PI * 3100 * (k + j) / FS));
    smp_filter_set(&flt, SMP_FILTER_CENTRE, 32512);
    CHECK(flt.mode == SMP_FILTER_LP && flt.wett == 0 && flt.dwet < 0, "fading out");
    smp_filter_run(&flt, buf);
    for (j = 0; j < 8; j++)
        first += fabs((double)buf[j] - ref[j]);
    CHECK(first > 100000000, "the first frames are still filtered: %.0f apart", first);
    CHECK(abs(buf[SMP_FILTER_BLOCK - 1] - ref[SMP_FILTER_BLOCK - 1]) <= 4, "the last frame is dry: %d vs %d",
          buf[SMP_FILTER_BLOCK - 1], ref[SMP_FILTER_BLOCK - 1]);
    smp_filter_set(&flt, SMP_FILTER_CENTRE, 32512);
    CHECK(flt.mode == SMP_FILTER_OFF, "off after the fade");
    for (j = 0; j < SMP_FILTER_BLOCK; j++)
        buf[j] = ref[j];
    smp_filter_run(&flt, buf);
    CHECK(!memcmp(buf, ref, sizeof buf), "off changes the block");
}

static void test_layout(void)
{
    CHECK(offsetof(smp_filter, l) == 0 && offsetof(smp_filter, b) == 4 && offsetof(smp_filter, f2) == 8 &&
          offsetof(smp_filter, q2) == 12 && offsetof(smp_filter, df2) == 16 && offsetof(smp_filter, dq2) == 20 &&
          offsetof(smp_filter, mode) == 24 && offsetof(smp_filter, loop) == 28 && offsetof(smp_filter, wet) == 32 &&
          offsetof(smp_filter, dwet) == 36 && offsetof(smp_filter, mix) == 40 && offsetof(smp_filter, dmix) == 44,
          "layout differs from smp_filter_cf.S");
}

int main(void)
{
    test_layout();
    test_table();
    test_off_and_starts();
    test_glide();
    test_loops_agree();
    test_side_jump();
    test_centre_fade();
    test_off_glide();
    test_sweep_smooth();
    test_saturation();
    test_response();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("filter: all tests passed\n");
    return 0;
}
