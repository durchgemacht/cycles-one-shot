/* The filter of the sampler machine: a 2-pole lowpass or highpass with resonance, after the
   Model:Samples. Its CUTOFF knob is one bipolar control: below the centre a lowpass that closes
   towards the left (64 = open, 0 = 5 Hz), above it a highpass that closes towards the right
   (127 = 19 kHz); RESONANCE raises a peak at the cutoff, up to about +27 dB. The laws (cutoff
   frequency, resonance, the loss of resonance towards the top, the pole angle) were measured on
   the Model:Samples OS 1.13 and are reproduced by tools/gen_filter_table.py with its own
   formulas; nothing of Elektron's code or tables is used.

   The two poles are placed directly (no zeros): the lowpass is G / (1 - 2 r cos th z^-1 + r^2 z^-2)
   with unity gain at DC, the highpass is the input minus that lowpass, as on the Model:Samples.
   Both run as the Chamberlin state-variable form, whose f and q are solved from the poles
   (f = |1 - r e^(j th)|, q = (1 - r^2) / f), so any pair, real or complex, is placed exactly and
   low cutoffs keep their precision in Q31. Per frame, with the states l (lowpass) and b (bandpass):

       l' = l + f b
       h  = x - l' - q b
       b' = b + f h
       lp = l' + f b'          hp = x - lp

   Coefficients come from the table once per block of 32 frames and glide linearly across the
   block. The loop works on half the signal and doubles its result, which gives the states the
   6 dB of headroom the bandpass needs near the top of the range; an output beyond full scale
   saturates. On the ColdFire the loop runs on the EMAC in fractional, saturating mode
   (src/smp_filter_cf.S); the host runs a C model with the same arithmetic (products truncated to
   2^-39, sums floored to Q31, saturated), so the host tests predict the device bit for bit. Plain
   C99 without libc or float; 64-bit integers only in the host model. */
#ifndef SMP_FILTER_H
#define SMP_FILTER_H

#include <stdint.h>

#define SMP_FILTER_OFF 0        /* the knob at exactly 64: untouched, bit for bit */
#define SMP_FILTER_LP  1
#define SMP_FILTER_HP  2

#define SMP_FILTER_CENTRE 16384 /* the knob word (8.8) at 64 */
#define SMP_FILTER_MAX    32512 /* at 127 */

#define SMP_FILTER_NI 129       /* table: cutoff points, every 128 of the index 0..16384 */
#define SMP_FILTER_NR 17        /* resonance points, every 2048 of the word 0..32768 */

/* The ColdFire loop reads this layout by offset (smp_filter_cf.S): keep the first seven fields. */
typedef struct {
    int32_t l, b;           /* states, Q31, at half the signal's scale */
    int32_t f2, q2;         /* f/2 and q/2 at the block's start, Q31 */
    int32_t df2, dq2;       /* their change per frame within the block */
    int32_t mode;           /* SMP_FILTER_OFF, _LP or _HP */
    int32_t f2t, q2t;       /* the block's targets, the next block's start */
    int32_t prime;          /* 1: the states start from the first frame of the next block */
} smp_filter;

/* f/2 and q/2, Q31, by resonance row then cutoff column (src/smp_filter_tab.c, generated). */
extern const int32_t smp_filter_tab[SMP_FILTER_NR][SMP_FILTER_NI][2];

/* Forgets everything: off, states cleared. */
void smp_filter_reset(smp_filter *f);

/* Sets the block's targets from the knob words (8.8, clamped to 0..SMP_FILTER_MAX): cutoff picks
   the side and the frequency, reso the resonance. Call once before each block's smp_filter_run.
   Entering a side (from off or from the other side) starts the states afresh: a lowpass from the
   block's first frame, a highpass from rest, so neither clicks. */
void smp_filter_set(smp_filter *f, int32_t cutoff, int32_t reso);

/* Filters one block of SMP_FILTER_BLOCK frames in place (Q31); nothing happens when off. */
#define SMP_FILTER_BLOCK 32
void smp_filter_run(smp_filter *f, int32_t *buf);

/* 1 while the filter is on and its states still carry signal (above 2^-17), so a track whose
   sound has ended keeps feeding it silence until the resonance has died away. */
int smp_filter_ringing(const smp_filter *f);

/* The frame loop itself, on the states and coefficients of f, for SMP_FILTER_BLOCK frames; the
   ColdFire version is in smp_filter_cf.S, the host model in smp_filter.c. */
void smp_filter_frames(smp_filter *f, int32_t *buf);

#endif
