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

   One filter runs per track, whichever side the knob is on; the side only picks the output,
   lp or hp, and the states carry on through a change of side. Two weights shape the output,
   each 0 .. 1 in Q31:

       y = x + wet (filt - x)          filt = lp + mix (hp - lp)

   mix is 0 on the lowpass side and 1 on the highpass side; wet is 1 everywhere except within
   one knob step of the centre, where it is the distance to the centre over 256, as the fine
   values of the OS's smoothing or of an LFO pass through it. So the knob, a p-lock or an LFO
   can cross the centre or jump from one side to the other without a click: the coefficients,
   wet and mix all glide linearly across the block (32 frames) from the last block's targets, as
   the coefficients do on the Model:Samples, and nothing is ever reset underway. At exactly 64,
   once the glide has reached wet = 0, the filter is off: the block passes untouched, bit for
   bit, and the states are forgotten; the next step into a side starts them afresh (a lowpass
   from the block's first frame, a highpass from rest) under the glide of wet from 0.

   The loop works on half the signal and doubles its result, which gives the states the 6 dB of
   headroom the bandpass needs near the top of the range; an output beyond full scale saturates.
   On the ColdFire the loop runs on the EMAC in fractional, saturating mode (src/smp_filter_cf.S):
   two short loops for the steady lowpass and highpass (wet = 1, mix = 0 or 1, no glide of the
   weights) and a longer one for the blocks in which a weight moves or stands between. The host
   runs a C model with the same arithmetic (products truncated to 2^-39, sums floored to Q31,
   saturated), so the host tests predict the device bit for bit. Plain C99 without libc or
   float; 64-bit integers only in the host model. */
#ifndef SMP_FILTER_H
#define SMP_FILTER_H

#include <stdint.h>

#define SMP_FILTER_OFF 0        /* the knob at exactly 64 and the fade finished: untouched, bit for bit */
#define SMP_FILTER_LP  1
#define SMP_FILTER_HP  2

#define SMP_FILTER_CENTRE 16384 /* the knob word (8.8) at 64 */
#define SMP_FILTER_MAX    32512 /* at 127 */
#define SMP_FILTER_ONE    0x7fffffff    /* the weights' 1.0 */

#define SMP_FILTER_NI 129       /* table: cutoff points, every 128 of the index 0..16384 */
#define SMP_FILTER_NR 17        /* resonance points, every 2048 of the word 0..32768 */

/* The frame loops of the ColdFire (smp_filter_cf.S): */
#define SMP_FILTER_LOOP_LP  0   /* the steady lowpass */
#define SMP_FILTER_LOOP_HP  1   /* the steady highpass */
#define SMP_FILTER_LOOP_MIX 2   /* the weights at work */

/* The ColdFire loop reads this layout by offset (smp_filter_cf.S): keep the first twelve fields. */
typedef struct {
    int32_t l, b;           /* states, Q31, at half the signal's scale */
    int32_t f2, q2;         /* f/2 and q/2 at the block's start, Q31 */
    int32_t df2, dq2;       /* their change per frame within the block */
    int32_t mode;           /* SMP_FILTER_OFF, _LP or _HP: the side, or off */
    int32_t loop;           /* SMP_FILTER_LOOP_*, chosen by smp_filter_run for the block */
    int32_t wet, dwet;      /* the dry/wet weight at the block's start and its change per frame */
    int32_t mix, dmix;      /* the lowpass/highpass weight, the same */
    int32_t f2t, q2t;       /* the block's targets, the next block's start */
    int32_t wett, mixt;
    int32_t prime;          /* 1: the states start from the first frame of the next block */
} smp_filter;

/* f/2 and q/2, Q31, by resonance row then cutoff column (src/smp_filter_tab.c, generated). */
extern const int32_t smp_filter_tab[SMP_FILTER_NR][SMP_FILTER_NI][2];

/* Forgets everything: off, states cleared. */
void smp_filter_reset(smp_filter *f);

/* Sets the block's targets from the knob words (8.8, clamped to 0..SMP_FILTER_MAX): cutoff picks
   the side, the frequency and wet, reso the resonance. Call once before each block's
   smp_filter_run. Everything glides across the block from the last block's targets; a step
   into a side from off starts the states afresh, and a block at the centre lets wet glide to 0
   before the filter switches off at the next call. */
void smp_filter_set(smp_filter *f, int32_t cutoff, int32_t reso);

/* Filters one block of SMP_FILTER_BLOCK frames in place (Q31); nothing happens when off. */
#define SMP_FILTER_BLOCK 32
void smp_filter_run(smp_filter *f, int32_t *buf);

/* 1 while the filter is on and its states still carry signal (above 2^-17), so a track whose
   sound has ended keeps feeding it silence until the resonance has died away. */
int smp_filter_ringing(const smp_filter *f);

/* The frame loop itself, on the states, coefficients and weights of f, for SMP_FILTER_BLOCK
   frames, the loop picked by f->loop; the ColdFire version is in smp_filter_cf.S, the host model
   in smp_filter.c. */
void smp_filter_frames(smp_filter *f, int32_t *buf);

#endif
