/* Kernel design for the one-shot core (host only; the Cycles never runs this). */
#ifndef DESIGN_H
#define DESIGN_H

#include <stdint.h>
#include "../src/smp_core.h"

/* Both designs fill one row of SMP_TAPS taps per phase. Row p serves the fractions p/256 ..
   (p+1)/256 and is computed for the middle one. Every row is rounded to Q15 and sums to exactly
   32768, so DC passes unchanged at every phase. Both return the largest sum of absolute taps of
   a row (the core's overflow bound needs it below 65536), or -1 if a tap does not fit in int16. */

/* One prototype for all rows: a linear-phase FIR of SMP_TAPS * SMP_PHASES taps at SMP_PHASES
   times the source rate, least squares over its whole band, then cut into rows. Passband
   0..fp (cycles per source frame) with target 1; stopband from fs up, where the images of the
   source spectrum fall, weighted ws against the passband; nothing asked in between. lift_db
   adds a bump to the passband target that peaks at 10 kHz (48 kHz source) and is gone at DC and
   20 kHz; 0 asks for flat. Each row is then centred on the playhead (its first moment set to 0
   with the least change), so low tones see no timing jitter from phase to phase. */
int32_t design_proto(double fp, double fs, double ws, double lift_db,
                     int16_t table[SMP_PHASES][SMP_TAPS]);

/* For comparison: a Kaiser-windowed sinc, cutoff fc in cycles per source frame (0.5 = Nyquist),
   window shape beta. */
int32_t design_kaiser(double fc, double beta, int16_t table[SMP_PHASES][SMP_TAPS]);

#endif
