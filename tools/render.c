/* render: play a WAV file through the core, or another method, at a pitch - for listening tests.

       render --decimator FILE [--table FILE | --design proto FP FS WS LIFT_DB | --zoh | --original]
              [--semitones ST] [--gain-db G] [--peak] IN.wav OUT.wav

   IN may be 16-, 24- or 32-bit PCM or 32-bit float, mono or stereo (mixed to mono), at any rate.
   It is first brought to 48 kHz with a long windowed sinc and rounded to 16 bits, the way it
   would sit on the device; --original writes just that. Otherwise it is played at ST semitones
   by one of these methods:
     (default)  the core with its built-in table (src/smp_coeffs.c)
     --table    the core with a table file (tools/samples_table.py writes the Model:Samples one)
     --design   the core with a table designed on the spot (tools/design.h)
     --zoh      the frame under the playhead at 48 kHz, held for two frames (how Model-TG plays)
   and brought from 96 to 48 kHz by a model of the Cycles' decimator (0x400a9f58 in OS 1.13) with
   the four coefficients in FILE (tools/samples_table.py writes them). Each table is scaled to
   unity gain at DC, so all methods play equally loud. OUT is 24-bit mono at 48 kHz, scaled by
   --gain-db. --peak prints the peak level in dBFS (before the gain) and writes nothing. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/smp_core.h"
#include "design.h"

#define RATE 48000

static int16_t table[SMP_PHASES][SMP_TAPS];

static void die(const char *msg, const char *arg)
{
    fprintf(stderr, "render: %s%s%s\n", msg, arg ? " " : "", arg ? arg : "");
    exit(1);
}

static unsigned char *slurp(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;

    if (!f)
        die("cannot open", path);
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(*size ? *size : 1);
    if (!buf || fread(buf, 1, *size, f) != (size_t)*size)
        die("cannot read", path);
    fclose(f);
    return buf;
}

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const unsigned char *p) { return p[0] | p[1] << 8; }

/* Reads a WAV file into mono samples in [-1, 1]. */
static double *read_wav(const char *path, long *frames, int *rate)
{
    long size, off = 12, data = -1, dlen = 0;
    unsigned char *b = slurp(path, &size);
    int fmt = 0, ch = 0, bits = 0, c;
    double *x;
    long i;

    if (size < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4))
        die("not a WAV file:", path);
    while (off + 8 <= size) {
        long len = le32(b + off + 4);

        if (!memcmp(b + off, "fmt ", 4) && len >= 16) {
            fmt = le16(b + off + 8);
            ch = le16(b + off + 10);
            *rate = (int)le32(b + off + 12);
            bits = le16(b + off + 22);
            if (fmt == 0xFFFE && len >= 26)             /* extensible: the subformat decides */
                fmt = le16(b + off + 32);
        } else if (!memcmp(b + off, "data", 4)) {
            data = off + 8;
            dlen = len < size - data ? len : size - data;
        }
        off += 8 + len + (len & 1);
    }
    if (data < 0 || ch < 1 || ch > 2 || !((fmt == 1 && (bits == 16 || bits == 24 || bits == 32)) ||
                                          (fmt == 3 && bits == 32)))
        die("unsupported WAV format (16/24/32-bit PCM or 32-bit float, mono or stereo):", path);
    *frames = dlen / (ch * bits / 8);
    x = calloc(*frames, sizeof *x);
    for (i = 0; i < *frames; i++)
        for (c = 0; c < ch; c++) {
            const unsigned char *p = b + data + (i * ch + c) * (bits / 8);
            double v;

            if (fmt == 3) {
                float fv;
                uint32_t u = le32(p);

                memcpy(&fv, &u, 4);
                v = fv;
            } else if (bits == 16)
                v = (int16_t)le16(p) / 32768.0;
            else if (bits == 24)
                v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) / 2147483648.0;
            else
                v = (int32_t)le32(p) / 2147483648.0;
            x[i] += v / ch;
        }
    free(b);
    return x;
}

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    int k;

    for (k = 1; k < 200; k++) {
        term *= q / ((double)k * k);
        sum += term;
        if (term < sum * 1e-17)
            break;
    }
    return sum;
}

/* Brings x to 48 kHz (Kaiser-windowed sinc over 64 input frames, cutoff 96 % of the lower
   Nyquist) and rounds to 16 bits. */
static int16_t *to_device(const double *x, long n, int rate, long *out_n)
{
    const int half = 32;
    const double beta = 10.0, i0b = bessel_i0(beta), r = (double)rate / RATE;
    const double fc = 0.48 * (r > 1.0 ? 1.0 / r : 1.0);         /* cycles per input frame */
    long m, len = rate == RATE ? n : (long)floor(n / r);
    int16_t *y = malloc(len * sizeof *y);

    for (m = 0; m < len; m++) {
        double v;

        if (rate == RATE)
            v = x[m];
        else {
            double t = m * r, acc = 0.0;
            long i0 = (long)floor(t), i;

            for (i = i0 - half + 1; i <= i0 + half; i++) {
                double d = t - i, u = d / half, a = 2.0 * M_PI * fc * d;

                if (i < 0 || i >= n || fabs(u) >= 1.0)
                    continue;
                acc += x[i] * 2.0 * fc * (fabs(a) < 1e-12 ? 1.0 : sin(a) / a)
                     * bessel_i0(beta * sqrt(1.0 - u * u)) / i0b;
            }
            v = acc;
        }
        v = round(v * 32768.0);
        y[m] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    *out_n = len;
    return y;
}

/* The Cycles' 2:1 decimator: the even input frames delayed, the odd ones through a 4th-order
   allpass, added. x at 96 kHz in half-Q31 units; returns 48 kHz in [-1, 1]. */
static double *decimate(const double *x, long n2, const double c[4], long *n)
{
    long len = (n2 - 9) / 2, k;
    double *y = malloc((len > 0 ? len : 1) * sizeof *y), h[4] = { 0, 0, 0, 0 };

    for (k = 0; k < len; k++) {
        double v = x[2 * k + 1] + c[0] * x[2 * k + 3] + c[1] * x[2 * k + 5] + c[2] * x[2 * k + 7]
                 + c[3] * x[2 * k + 9] - c[0] * h[0] - c[1] * h[1] - c[2] * h[2] - c[3] * h[3];

        h[3] = h[2];
        h[2] = h[1];
        h[1] = h[0];
        h[0] = v;
        y[k] = (v + x[2 * k + 2]) / 2147483648.0;
    }
    *n = len > 0 ? len : 0;
    return y;
}

static void write_wav(const char *path, const double *y, long n, double gain)
{
    FILE *f = fopen(path, "wb");
    unsigned char h[44];
    long i;
    uint32_t data = (uint32_t)n * 3;

    if (!f)
        die("cannot write", path);
    memcpy(h, "RIFF", 4);
    h[4] = (36 + data) & 255; h[5] = (36 + data) >> 8 & 255; h[6] = (36 + data) >> 16 & 255; h[7] = (36 + data) >> 24;
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = h[18] = h[19] = 0;
    h[20] = 1; h[21] = 0;                               /* PCM */
    h[22] = 1; h[23] = 0;                               /* mono */
    h[24] = RATE & 255; h[25] = RATE >> 8 & 255; h[26] = RATE >> 16; h[27] = 0;
    h[28] = (RATE * 3) & 255; h[29] = (RATE * 3) >> 8 & 255; h[30] = (RATE * 3) >> 16; h[31] = 0;
    h[32] = 3; h[33] = 0; h[34] = 24; h[35] = 0;
    memcpy(h + 36, "data", 4);
    h[40] = data & 255; h[41] = data >> 8 & 255; h[42] = data >> 16 & 255; h[43] = data >> 24;
    fwrite(h, 1, 44, f);
    for (i = 0; i < n; i++) {
        double v = round(y[i] * gain * 8388608.0);
        int32_t s = (int32_t)(v > 8388607 ? 8388607 : v < -8388608 ? -8388608 : v);
        unsigned char o[3] = { s & 255, s >> 8 & 255, s >> 16 & 255 };

        fwrite(o, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    enum { CORE, ZOH, ORIGINAL } method = CORE;
    const char *dec_path = NULL, *in = NULL, *out = NULL;
    const int16_t (*tab)[SMP_TAPS] = smp_coeffs;
    double st = 0.0, gain_db = 0.0, c[4], peak = 0.0, *x, *y2, *y;
    int peak_only = 0, rate, i;
    long n_in, n, n2, ny, k;
    int16_t *pcm;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--decimator") && i + 1 < argc)
            dec_path = argv[++i];
        else if (!strcmp(argv[i], "--table") && i + 1 < argc) {
            long size;
            unsigned char *b = slurp(argv[++i], &size);
            int p, t;

            if (size != SMP_PHASES * SMP_TAPS * 2)
                die("wrong size:", argv[i]);
            for (p = 0; p < SMP_PHASES; p++)
                for (t = 0; t < SMP_TAPS; t++)
                    table[p][t] = (int16_t)(b[2 * (p * SMP_TAPS + t)] << 8 | b[2 * (p * SMP_TAPS + t) + 1]);
            free(b);
            tab = (const int16_t (*)[SMP_TAPS])table;
        } else if (!strcmp(argv[i], "--design") && i + 5 < argc && !strcmp(argv[i + 1], "proto")) {
            if (design_proto(atof(argv[i + 2]), atof(argv[i + 3]), atof(argv[i + 4]), atof(argv[i + 5]), table) < 0)
                die("design failed", NULL);
            tab = (const int16_t (*)[SMP_TAPS])table;
            i += 5;
        } else if (!strcmp(argv[i], "--zoh"))
            method = ZOH;
        else if (!strcmp(argv[i], "--original"))
            method = ORIGINAL;
        else if (!strcmp(argv[i], "--semitones") && i + 1 < argc)
            st = atof(argv[++i]);
        else if (!strcmp(argv[i], "--gain-db") && i + 1 < argc)
            gain_db = atof(argv[++i]);
        else if (!strcmp(argv[i], "--peak"))
            peak_only = 1;
        else if (!in)
            in = argv[i];
        else if (!out)
            out = argv[i];
        else
            die("too many arguments", NULL);
    }
    if (!in || (!out && !peak_only) || (!dec_path && method != ORIGINAL))
        die("usage: render --decimator FILE [--table FILE | --design proto FP FS WS LIFT_DB | --zoh |"
            " --original] [--semitones ST] [--gain-db G] [--peak] IN.wav OUT.wav", NULL);
    if (dec_path) {
        long size;
        unsigned char *b = slurp(dec_path, &size);

        if (size != 16)
            die("wrong size:", dec_path);
        for (i = 0; i < 4; i++)
            c[i] = (int32_t)((uint32_t)b[4 * i] << 24 | b[4 * i + 1] << 16 | b[4 * i + 2] << 8 | b[4 * i + 3])
                 / 2147483648.0;
        free(b);
    }

    x = read_wav(in, &n_in, &rate);
    pcm = to_device(x, n_in, rate, &n);
    free(x);

    if (method == ORIGINAL) {
        ny = n;
        y = malloc(n * sizeof *y);
        for (k = 0; k < n; k++)
            y[k] = pcm[k] / 32768.0;
    } else {
        uint32_t ratio_q16 = (uint32_t)lround(pow(2.0, st / 12.0) * 65536.0);
        long cap = (long)(n / (ratio_q16 / 65536.0) * 2.0) + 64 * SMP_BLOCK2X;

        y2 = calloc(cap, sizeof *y2);
        n2 = 0;
        if (method == ZOH) {
            uint64_t P = 0, step = (uint64_t)ratio_q16 << 16;   /* per 48 kHz frame */

            for (; (long)(P >> 32) < n && n2 + 2 <= cap; P += step) {
                y2[n2++] = pcm[P >> 32] * 32768.0;
                y2[n2++] = pcm[P >> 32] * 32768.0;
            }
        } else {
            static int16_t scratch[SMP_SCRATCH];
            int32_t blk[SMP_BLOCK2X];
            double dc = 0.0;
            smp_voice v;

            for (i = 0; i < SMP_TAPS; i++)
                dc += tab[0][i];
            smp_trigger(&v, pcm, (int32_t)n, 0, (int32_t)n);
            smp_set_ratio(&v, ratio_q16);
            while (n2 + SMP_BLOCK2X <= cap && smp_render_with(&v, blk, scratch, tab))
                for (i = 0; i < SMP_BLOCK2X; i++)
                    y2[n2++] = blk[i] * (32768.0 / dc);
        }
        n2 += 64;                                       /* let the decimator ring out */
        if (n2 > cap)
            n2 = cap;
        y = decimate(y2, n2, c, &ny);
        free(y2);
    }
    for (k = 0; k < ny; k++)
        if (fabs(y[k]) > peak)
            peak = fabs(y[k]);
    if (peak_only)
        printf("%.2f\n", peak > 0 ? 20.0 * log10(peak) : -200.0);
    else
        write_wav(out, y, ny, pow(10.0, gain_db / 20.0));
    free(y);
    free(pcm);
    return 0;
}
