/* Kernel design, see design.h. */
#include <math.h>
#include <stdlib.h>
#include "design.h"

#define X(k, f) ((k) - (SMP_TAPS / 2 - 1) - (f))   /* tap k relative to the playhead, in frames */

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    int k;

    for (k = 1; k < 100; k++) {
        term *= q / ((double)k * k);
        sum += term;
        if (term < sum * 1e-17)
            break;
    }
    return sum;
}

/* Rounds a row (any scale) to Q15 summing to exactly 32768 and stores it. Returns the sum of
   its absolute taps, or -1 if a tap does not fit in int16. */
static int32_t store_row(const double *h, int16_t *row)
{
    double err[SMP_TAPS], sum = 0.0;
    int32_t q[SMP_TAPS], qsum = 0, abs_sum = 0;
    int k;

    for (k = 0; k < SMP_TAPS; k++)
        sum += h[k];
    for (k = 0; k < SMP_TAPS; k++) {
        double v = h[k] / sum * 32768.0;

        q[k] = (int32_t)lround(v);
        err[k] = v - q[k];
        qsum += q[k];
    }
    /* Rounding leaves the row a few units off 32768: move the taps whose rounding erred most in
       the needed direction. */
    while (qsum != 32768) {
        int dir = qsum < 32768 ? 1 : -1, best = 0;

        for (k = 1; k < SMP_TAPS; k++)
            if (err[k] * dir > err[best] * dir)
                best = k;
        q[best] += dir;
        err[best] -= dir;
        qsum += dir;
    }
    for (k = 0; k < SMP_TAPS; k++) {
        if (q[k] > 32767 || q[k] < -32768)
            return -1;
        row[k] = (int16_t)q[k];
        abs_sum += abs(q[k]);
    }
    return abs_sum;
}

int32_t design_kaiser(double fc, double beta, int16_t table[SMP_PHASES][SMP_TAPS])
{
    const double half = SMP_TAPS / 2.0;         /* the window spans -3 .. +3 frames */
    const double i0b = bessel_i0(beta);
    int32_t worst = 0;
    int p, k;

    for (p = 0; p < SMP_PHASES; p++) {
        double f = (p + 0.5) / SMP_PHASES, h[SMP_TAPS];
        int32_t abs_sum;

        for (k = 0; k < SMP_TAPS; k++) {
            double x = X(k, f), u = x / half, a = 2.0 * M_PI * fc * x;
            double w = fabs(u) < 1.0 ? bessel_i0(beta * sqrt(1.0 - u * u)) / i0b : 0.0;

            h[k] = (fabs(a) < 1e-12 ? 1.0 : sin(a) / a) * w;
        }
        abs_sum = store_row(h, table[p]);
        if (abs_sum < 0)
            return -1;
        if (abs_sum > worst)
            worst = abs_sum;
    }
    return worst;
}

/* Solves the n x n system m (row-major, n+1 columns with the right side last) in place. */
static void solve(double *m, int n, double *x)
{
    int i, j, k;

    for (i = 0; i < n; i++) {
        int piv = i;

        for (j = i + 1; j < n; j++)
            if (fabs(m[j * (n + 1) + i]) > fabs(m[piv * (n + 1) + i]))
                piv = j;
        for (k = 0; k <= n; k++) {
            double t = m[i * (n + 1) + k];

            m[i * (n + 1) + k] = m[piv * (n + 1) + k];
            m[piv * (n + 1) + k] = t;
        }
        for (j = i + 1; j < n; j++) {
            double r = m[j * (n + 1) + i] / m[i * (n + 1) + i];

            for (k = i; k <= n; k++)
                m[j * (n + 1) + k] -= r * m[i * (n + 1) + k];
        }
    }
    for (i = n - 1; i >= 0; i--) {
        double s = m[i * (n + 1) + n];

        for (k = i + 1; k < n; k++)
            s -= m[i * (n + 1) + k] * x[k];
        x[i] = s / m[i * (n + 1) + i];
    }
}

/* Integral of cos(2 pi F t) dF over a..b. */
static double cos_int(double t, double a, double b)
{
    if (fabs(t) < 1e-12)
        return b - a;
    return (sin(2.0 * M_PI * b * t) - sin(2.0 * M_PI * a * t)) / (2.0 * M_PI * t);
}

int32_t design_proto(double fp, double fs, double ws, double lift_db,
                     int16_t table[SMP_PHASES][SMP_TAPS])
{
    enum { LEN = SMP_TAPS * SMP_PHASES, HALF = LEN / 2, PGRID = 4000 };
    const double top = SMP_PHASES / 2.0;        /* the prototype's Nyquist, in source cycles */
    const double fpk = 10000.0 / 48000.0;       /* the bump peaks here and is gone at twice it */
    double *m = malloc((size_t)HALF * (HALF + 1) * sizeof *m), *a = malloc(HALF * sizeof *a);
    double t[HALF];
    int32_t worst = 0;
    int i, j, p, k;

    if (!m || !a) {
        free(m);
        free(a);
        return -1;
    }
    /* Symmetric about its middle, the prototype's response is H(F) = sum_i a_i cos(2 pi F t_i),
       t_i the distance of tap pair i from the middle in source frames. The normal equations of
       the weighted squared error, integrated in closed form over each band: */
    for (i = 0; i < HALF; i++)
        t[i] = (HALF - 0.5 - i) / SMP_PHASES;
    for (i = 0; i < HALF; i++) {
        for (j = 0; j < HALF; j++) {
            double d = t[i] - t[j], s = t[i] + t[j];

            m[i * (HALF + 1) + j] = 0.5 * (cos_int(d, 0, fp) + cos_int(s, 0, fp))
                                  + ws * 0.5 * (cos_int(d, fs, top) + cos_int(s, fs, top));
        }
        m[i * (HALF + 1) + HALF] = 0.0;
    }
    /* ... and the right side, the passband target, integrated numerically (midpoint rule) */
    for (j = 0; j < PGRID; j++) {
        double F = fp * (j + 0.5) / PGRID, s = F < 2.0 * fpk ? sin(M_PI * F / (2.0 * fpk)) : 0.0;
        double D = pow(10.0, lift_db * s * s / 20.0) * fp / PGRID;

        for (i = 0; i < HALF; i++)
            m[i * (HALF + 1) + HALF] += D * cos(2.0 * M_PI * F * t[i]);
    }
    solve(m, HALF, a);

    for (p = 0; p < SMP_PHASES; p++) {
        double f = (p + 0.5) / SMP_PHASES, h[SMP_TAPS], sum = 0, sx = 0, sxx = 0, m1 = 0, det;
        int32_t abs_sum;

        for (k = 0; k < SMP_TAPS; k++) {
            int idx = SMP_PHASES * k + SMP_PHASES - 1 - p;      /* the tap at X(k, f) */

            h[k] = a[idx < HALF ? idx : LEN - 1 - idx];
            sum += h[k];
        }
        /* Scale the row to sum 1, then centre it: the least change (alpha + beta x_k) that keeps
           the sum and puts the first moment sum h_k x_k at 0. */
        for (k = 0; k < SMP_TAPS; k++) {
            h[k] /= sum;
            sx += X(k, f);
            sxx += X(k, f) * X(k, f);
            m1 += h[k] * X(k, f);
        }
        det = SMP_TAPS * sxx - sx * sx;
        for (k = 0; k < SMP_TAPS; k++)
            h[k] -= (-sx * m1 + SMP_TAPS * m1 * X(k, f)) / det;
        abs_sum = store_row(h, table[p]);
        if (abs_sum < 0) {
            worst = -1;
            break;
        }
        if (abs_sum > worst)
            worst = abs_sum;
    }
    free(m);
    free(a);
    return worst;
}
