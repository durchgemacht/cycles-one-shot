/* Prints the magnitude response of the filter's fixed-point loop (the host model) at one setting:
   filter_probe <cutoff word> <reso word>, then lines "Hz dB" from 10 Hz to 22 kHz, 24 per decade.
   Each point is a settled sine (a hundredth of full scale), fitted by least squares. For checking
   the implementation against measurements of a Model:Samples. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/smp_filter.h"

#define FS 48000.0
#define PI 3.14159265358979323846

static double measure(int32_t cutoff, int32_t reso, double f)
{
    smp_filter flt;
    double amp = 0.01 * 2147483648.0, ws = 0, wc = 0;
    long settle = 400000, cycles, n, k;
    int32_t buf[SMP_FILTER_BLOCK];

    cycles = (long)ceil(4096 * f / FS);
    if (cycles < 2)
        cycles = 2;
    n = (long)(cycles * FS / f);
    smp_filter_reset(&flt);
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

int main(int argc, char **argv)
{
    int32_t cutoff, reso;
    int k;

    if (argc != 3) {
        fprintf(stderr, "usage: filter_probe <cutoff word 0..32512> <reso word 0..32512>\n");
        return 2;
    }
    cutoff = atoi(argv[1]);
    reso = atoi(argv[2]);
    for (k = 0; k <= 24 * 11; k++) {
        double f = 10 * pow(2, k / 24.0), m;

        if (f > 22000)
            break;
        m = measure(cutoff, reso, f);
        printf("%.3f %.3f\n", f, m > 0 ? 20 * log10(m) : -200.0);
    }
    return 0;
}
