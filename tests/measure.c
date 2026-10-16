/* measure: how cleanly each method plays a sine at a given pitch.

       measure [--samples FILE] [--sweep | --design proto FP FS WS LIFT_DB | --design kaiser FC BETA]

   Methods, all rendering at 96 kHz:
     zoh      the frame under the playhead, held for two frames (how Model-TG plays a sample)
     linear   linear interpolation between the two frames around the playhead
     core     src/smp_core.c with its built-in table (src/smp_coeffs.c)
     samples  the core with the Model:Samples OS 1.13 table, read from FILE, which
              tools/samples_table.py makes from your own copy of the OS file
   One long FIR - not the Cycles' decimator - brings each result down to 48 kHz and passes
   up to about 21 kHz. Reported: the SINAD (the tone against everything else in that band) and
   the gain at the tone, both in dB. The source is a sine at -1 dBFS in 16 bits, pitched so the
   output tone lands on the frequency of the row.

   --sweep rates table designs (tools/design.c) against each other instead; --design reports
   one design on the whole grid in place of the built-in table. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/smp_core.h"
#include "../tools/design.h"

#define FS 48000.0
#define DEC_TAPS 511
#define SKIP 1024                       /* 48 kHz frames dropped at each end */

enum { ZOH, LINEAR, CORE };

static const int semitones[] = { -24, -12, -5, 0, 5, 12, 24 };
static const double tones[] = { 1000, 5000, 10000, 15000 };
#define NST ((int)(sizeof semitones / sizeof semitones[0]))
#define NTONE ((int)(sizeof tones / sizeof tones[0]))

static double dec[DEC_TAPS];

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

/* Kaiser lowpass at 96 kHz, cutoff 21.5 kHz, beta 12 (about 117 dB down from 22.2 kHz on). */
static void make_decimator(void)
{
    const double fc = 21500.0 / (2 * FS), beta = 12.0, mid = (DEC_TAPS - 1) / 2.0;
    double sum = 0.0;
    int k;

    for (k = 0; k < DEC_TAPS; k++) {
        double x = k - mid, u = x / mid, a = 2.0 * M_PI * fc * x;

        dec[k] = (x == 0 ? 2.0 * fc : sin(a) / (M_PI * x)) * bessel_i0(beta * sqrt(1.0 - u * u))
               / bessel_i0(beta);
        sum += dec[k];
    }
    for (k = 0; k < DEC_TAPS; k++)
        dec[k] /= sum;
}

/* Renders n2 frames at 96 kHz of src played at ratio_q16 into y2, in units of the source. */
static void render(int method, const int16_t (*table)[SMP_TAPS], const int16_t *src, int32_t len,
                   uint32_t ratio_q16, double *y2, int n2)
{
    if (method == CORE) {
        static int16_t scratch[SMP_SCRATCH];
        int32_t out[SMP_BLOCK2X], dc = 0;
        smp_voice v;
        int j, k;

        for (k = 0; k < SMP_TAPS; k++)
            dc += table[0][k];
        smp_trigger(&v, src, len, 0, len);
        smp_set_ratio(&v, ratio_q16);
        for (j = 0; j < n2; j += SMP_BLOCK2X) {
            smp_render_with(&v, out, scratch, table);
            for (k = 0; k < SMP_BLOCK2X && j + k < n2; k++)
                y2[j + k] = out[k] / (double)dc;
        }
    } else if (method == ZOH) {
        uint64_t P = 0, step = (uint64_t)ratio_q16 << 16;      /* per 48 kHz frame */
        int j;

        for (j = 0; j + 1 < n2; j += 2, P += step)
            y2[j] = y2[j + 1] = src[P >> 32];
    } else {
        uint64_t P = 0, step = (uint64_t)ratio_q16 << 15;      /* per 96 kHz frame */
        int j;

        for (j = 0; j < n2; j++, P += step) {
            int64_t i = (int64_t)(P >> 32);
            double fr = (double)(uint32_t)P / 4294967296.0;

            y2[j] = src[i] + fr * (src[i + 1] - src[i]);
        }
    }
}

/* SINAD and gain (dB) of method at the given pitch, with the output tone at fo. */
static void measure(int method, const int16_t (*table)[SMP_TAPS], int st, double fo, int n,
                    double *sinad, double *gain)
{
    const double amp = 32767.0 * pow(10.0, -1.0 / 20.0);
    uint32_t ratio_q16 = (uint32_t)lround(pow(2.0, st / 12.0) * 65536.0);
    double ratio = ratio_q16 / 65536.0, fsrc = fo / ratio, w = 2.0 * M_PI * fo / FS;
    int32_t len = (int32_t)(ratio * (n + DEC_TAPS + SMP_FADE)) + 64, k;     /* the fade-out after what is used */
    int n2 = 2 * n + DEC_TAPS, m;
    int16_t *src = malloc(len * sizeof *src);
    double *y2 = calloc(n2, sizeof *y2), *y = malloc(n * sizeof *y);
    double s11 = 0, s12 = 0, s13 = 0, s22 = 0, s23 = 0, s33 = 0, b1 = 0, b2 = 0, b3 = 0;
    double det, a, b, c, res = 0;

    for (k = 0; k < len; k++)
        src[k] = (int16_t)lround(amp * sin(2.0 * M_PI * fsrc / FS * k + 0.3));
    render(method, table, src, len, ratio_q16, y2, n2);
    for (m = 0; m < n; m++) {
        double acc = 0;

        for (k = 0; k < DEC_TAPS; k++)
            acc += dec[k] * y2[2 * m + k];
        y[m] = acc;
    }
    /* least squares: y = a sin + b cos + c over the steady part */
    for (m = SKIP; m < n - SKIP; m++) {
        double x1 = sin(w * m), x2 = cos(w * m);

        s11 += x1 * x1; s12 += x1 * x2; s13 += x1; s22 += x2 * x2; s23 += x2; s33 += 1;
        b1 += x1 * y[m]; b2 += x2 * y[m]; b3 += y[m];
    }
    det = s11 * (s22 * s33 - s23 * s23) - s12 * (s12 * s33 - s23 * s13) + s13 * (s12 * s23 - s22 * s13);
    a = (b1 * (s22 * s33 - s23 * s23) - s12 * (b2 * s33 - s23 * b3) + s13 * (b2 * s23 - s22 * b3)) / det;
    b = (s11 * (b2 * s33 - s23 * b3) - b1 * (s12 * s33 - s23 * s13) + s13 * (s12 * b3 - b2 * s13)) / det;
    c = (s11 * (s22 * b3 - b2 * s23) - s12 * (s12 * b3 - b2 * s13) + b1 * (s12 * s23 - s22 * s13)) / det;
    for (m = SKIP; m < n - SKIP; m++) {
        double e = y[m] - (a * sin(w * m) + b * cos(w * m) + c);

        res += e * e;
    }
    res /= (n - 2 * SKIP);
    *sinad = 10.0 * log10((a * a + b * b) / 2.0 / res);
    *gain = 20.0 * log10(sqrt(a * a + b * b) / amp);
    free(src);
    free(y2);
    free(y);
}

static int valid(int st, double fo)
{
    double fsrc = fo / pow(2.0, st / 12.0);

    return fsrc >= 20.0 && fsrc <= 20000.0;
}

static void table_report(const char *name, int method, const int16_t (*table)[SMP_TAPS])
{
    double sinad[NTONE][NST], gain[NTONE][NST];
    int i, j;

    for (i = 0; i < NTONE; i++)
        for (j = 0; j < NST; j++)
            if (valid(semitones[j], tones[i]))
                measure(method, table, semitones[j], tones[i], 32768, &sinad[i][j], &gain[i][j]);
    printf("\n%s - SINAD in dB (gain in dB)\n%-9s", name, "tone");
    for (j = 0; j < NST; j++)
        printf("  %+4d st        ", semitones[j]);
    printf("\n");
    for (i = 0; i < NTONE; i++) {
        printf("%5.0f Hz ", tones[i]);
        for (j = 0; j < NST; j++) {
            if (valid(semitones[j], tones[i]))
                printf("  %5.1f (%+5.2f)", sinad[i][j], gain[i][j]);
            else
                printf("  %-14s", "   -");
        }
        printf("\n");
    }
}

/* One line for the sweep: worst and mean SINAD over the grid, then the SINAD at -5 and +5
   semitones with a 1 kHz and a 10 kHz tone, and the gain at 10 and 15 kHz at the original pitch. */
static void score(const char *name, const int16_t (*table)[SMP_TAPS])
{
    double worst = 1e9, mean = 0, s, g, g10 = 0, g15 = 0, s5[4];
    int i, j, cnt = 0;

    for (i = 0; i < NTONE; i++)
        for (j = 0; j < NST; j++) {
            if (!valid(semitones[j], tones[i]))
                continue;
            measure(CORE, table, semitones[j], tones[i], 16384, &s, &g);
            if (s < worst)
                worst = s;
            mean += s;
            cnt++;
            if (semitones[j] == 0 && tones[i] == 10000)
                g10 = g;
            if (semitones[j] == 0 && tones[i] == 15000)
                g15 = g;
            if ((semitones[j] == -5 || semitones[j] == 5) && (tones[i] == 1000 || tones[i] == 10000))
                s5[(semitones[j] > 0) * 2 + (tones[i] > 1000)] = s;
        }
    printf("%-30s %6.1f %6.1f   %5.1f %5.1f %5.1f %5.1f   %+5.2f %+5.2f\n", name, worst, mean / cnt,
           s5[0], s5[1], s5[2], s5[3], g10, g15);
}

static int load_table(const char *path, int16_t (*table)[SMP_TAPS])
{
    unsigned char raw[SMP_PHASES * SMP_TAPS * 2];
    FILE *f = fopen(path, "rb");
    size_t got;
    int p, k;

    if (!f) {
        perror(path);
        return 0;
    }
    got = fread(raw, 1, sizeof raw, f);
    fclose(f);
    if (got != sizeof raw) {
        fprintf(stderr, "%s: expected %zu bytes, got %zu\n", path, sizeof raw, got);
        return 0;
    }
    for (p = 0; p < SMP_PHASES; p++)
        for (k = 0; k < SMP_TAPS; k++) {
            const unsigned char *b = raw + 2 * (p * SMP_TAPS + k);

            table[p][k] = (int16_t)(b[0] << 8 | b[1]);
        }
    return 1;
}

int main(int argc, char **argv)
{
    static int16_t samples[SMP_PHASES][SMP_TAPS], trial[SMP_PHASES][SMP_TAPS];
    const char *samples_path = NULL;
    const int16_t (*core)[SMP_TAPS] = smp_coeffs;
    char core_name[80] = "core (src/smp_coeffs.c)";
    int sweep = 0, i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--samples") && i + 1 < argc)
            samples_path = argv[++i];
        else if (!strcmp(argv[i], "--sweep"))
            sweep = 1;
        else if (!strcmp(argv[i], "--design") && i + 5 < argc && !strcmp(argv[i + 1], "proto")) {
            if (design_proto(atof(argv[i + 2]), atof(argv[i + 3]), atof(argv[i + 4]), atof(argv[i + 5]), trial) < 0)
                return 1;
            snprintf(core_name, sizeof core_name, "proto %s %s %s %s", argv[i + 2], argv[i + 3], argv[i + 4],
                     argv[i + 5]);
            core = (const int16_t (*)[SMP_TAPS])trial;
            i += 5;
        } else if (!strcmp(argv[i], "--design") && i + 3 < argc && !strcmp(argv[i + 1], "kaiser")) {
            if (design_kaiser(atof(argv[i + 2]), atof(argv[i + 3]), trial) < 0)
                return 1;
            snprintf(core_name, sizeof core_name, "kaiser %s %s", argv[i + 2], argv[i + 3]);
            core = (const int16_t (*)[SMP_TAPS])trial;
            i += 3;
        } else {
            fprintf(stderr, "usage: measure [--samples FILE] [--sweep | --design proto FP FS WS LIFT_DB"
                            " | --design kaiser FC BETA]\n");
            return 2;
        }
    }
    if (samples_path && !load_table(samples_path, samples))
        return 1;
    make_decimator();

    if (sweep) {
        static const double fps[] = { 0.33, 0.36, 0.40 };
        static const double fss[] = { 0.55, 0.60, 0.64 };
        static const double wss[] = { 10, 100, 1000 };
        static const double lifts[] = { -1.8, 0.0, 1.9 };
        unsigned a, b, c, d;

        printf("%-30s %6s %6s   %-23s   %-11s\n", "", "SINAD", "", "SINAD -5 st | +5 st", "gain 0 st");
        printf("%-30s %6s %6s   %5s %5s %5s %5s   %5s %5s\n", "design", "worst", "mean", "1k", "10k", "1k",
               "10k", "10k", "15k");
        if (samples_path)
            score("Model:Samples 1.13", (const int16_t (*)[SMP_TAPS])samples);
        score("built-in (src/smp_coeffs.c)", smp_coeffs);
        if (design_kaiser(0.45, 5, trial) >= 0)
            score("kaiser 0.45 5", (const int16_t (*)[SMP_TAPS])trial);
        for (c = 0; c < sizeof lifts / sizeof lifts[0]; c++)
            for (a = 0; a < sizeof fps / sizeof fps[0]; a++)
                for (b = 0; b < sizeof fss / sizeof fss[0]; b++)
                    for (d = 0; d < sizeof wss / sizeof wss[0]; d++) {
                        char name[48];

                        if (design_proto(fps[a], fss[b], wss[d], lifts[c], trial) < 0)
                            continue;
                        snprintf(name, sizeof name, "proto %.2f %.2f %4.0f %+.1f", fps[a], fss[b], wss[d],
                                 lifts[c]);
                        score(name, (const int16_t (*)[SMP_TAPS])trial);
                    }
        return 0;
    }

    table_report("zoh (one frame, held; Model-TG)", ZOH, NULL);
    table_report("linear", LINEAR, NULL);
    table_report(core_name, CORE, core);
    if (samples_path)
        table_report("Model:Samples 1.13 table", CORE, (const int16_t (*)[SMP_TAPS])samples);
    return 0;
}
