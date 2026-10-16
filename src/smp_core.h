/* One-shot sample voice: the sound core of a sampler machine for the Model:Cycles.

   It plays 16-bit mono PCM from a start frame to an end frame, at a pitch ratio between 1/64
   (-72 semitones) and 8 (+36 semitones). Each call renders one audio block at twice the output
   rate, 64 frames at 96 kHz; the Cycles' own 2:1 decimator (stock voice chain, 0x400a9f58 in
   OS 1.13) brings them down to the 32 frames of a 48 kHz block. The Model:Samples OS 1.13 works
   the same way: a 6-tap polyphase filter with 256 phases at twice the rate, then the same
   decimator.

   A full-scale frame (32767) comes out as 32767 << 15, half of Q31, the level the Cycles
   machines hand to that decimator. Plain C99 without libc, float or 64-bit arithmetic, for
   m68k-elf-gcc -mcpu=54418 and for the host tests alike.

   Neither edge of a window clicks: a trig whose start lies past the sample's first frame fades in
   over SMP_FADE frames, and every window fades out over the SMP_FADE frames before the playhead
   reaches its end, both along the S curve 3x^2 - 2x^3. The fades count frames at 96 kHz, so they
   last 4 ms at any pitch. A window that starts at frame 0 keeps its attack as recorded. Outside
   the fades every frame is the plain filter result. */
#ifndef SMP_CORE_H
#define SMP_CORE_H

#include <stdint.h>

#define SMP_TAPS 6                      /* frames each output frame is computed from */
#define SMP_PHASES 256                  /* table rows; the top 8 bits of the fraction pick one */
#define SMP_BLOCK 32                    /* frames per audio block at 48 kHz */
#define SMP_BLOCK2X (2 * SMP_BLOCK)     /* frames rendered per block, at 96 kHz */

#define SMP_FADE 384                    /* frames of each fade at 96 kHz: 4 ms */

#define SMP_MIN_RATIO_Q16 (1u << 10)    /* 1/64 */
#define SMP_MAX_RATIO_Q16 (8u << 16)    /* 8: a step of 4 frames at 96 kHz */

/* Source frames one block reads at most: 63 steps of 4, the taps, and one carry. */
#define SMP_SCRATCH ((SMP_BLOCK2X - 1) * (SMP_MAX_RATIO_Q16 >> 17) + SMP_TAPS + 1)

typedef struct {
    const int16_t *pcm;     /* sample data, native byte order (big-endian on the Cycles) */
    int32_t start;          /* first frame played */
    int32_t end;            /* one past the last frame played */
    int32_t pos;            /* playhead, whole frames */
    uint32_t frac;          /* playhead, fraction in 2^-32 frames */
    uint32_t step_int;      /* advance per 96 kHz frame, whole frames */
    uint32_t step_frac;     /* advance per 96 kHz frame, fraction */
    int32_t active;         /* 0 once every tap has passed the end */
    int32_t fade_in;        /* frames of the fade-in done; SMP_FADE when there is none to do */
} smp_voice;

/* Taps of each phase, rows sum to 32768 (src/smp_coeffs.c, made by tools/gen_coeffs). */
extern const int16_t smp_coeffs[SMP_PHASES][SMP_TAPS];

/* Starts playback at start; frames before start and from end on count as silence. start and
   end are clamped to 0..length. The ratio stays as it was. A start past frame 0 fades in. */
void smp_trigger(smp_voice *v, const int16_t *pcm, int32_t length, int32_t start, int32_t end);

/* Pitch as a ratio to the original, Q16.16 (65536 = as recorded at 48 kHz). Clamped to
   SMP_MIN_RATIO_Q16..SMP_MAX_RATIO_Q16. Takes effect with the next block. */
void smp_set_ratio(smp_voice *v, uint32_t ratio_q16);

/* Renders one block into out[0..SMP_BLOCK2X). scratch holds SMP_SCRATCH frames and may be
   shared by all voices. Returns 0, with out zeroed, once the voice has finished. */
int smp_render(smp_voice *v, int32_t *out, int16_t *scratch);

/* The same with another table (tests and measurements). */
int smp_render_with(smp_voice *v, int32_t *out, int16_t *scratch,
                    const int16_t (*table)[SMP_TAPS]);

#endif
