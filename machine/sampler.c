/* SAMPLE: a sampler machine for the Elektron Model:Cycles, OS 1.13 - version 0.1, a test build.

   It plays one of a few built-in test samples once per trig (one shot) through the core in
   src/smp_core.c and hands the result to the Cycles' own chain: its 2:1 decimator (0x400a9f58),
   then the amp envelope, VCA and PUNCH, set up the way TONE (0x400aa7b8) and Modded-Cycles'
   MACRO set them. Samples from the +Drive come later.

   Contract of the voice loop (Modded-Cycles notes/14 and notes/43): sampler_update(pmod, v, p),
   then sampler_render(out, v), once per block of 32 frames at 48 kHz. pmod is the trig's note in
   semitones << 16, v the track's voice (0x31c bytes), p its parameters as 8.8 words (+0x14 PITCH,
   +0x16 COLOR, +0x18 SHAPE, +0x1a SWEEP, +0x1c CONTOUR, +0x1e PUNCH, +0x22 FINE, +0x24 DECAY);
   v+0x34 / v+0x38 hold the trig of this block / of the previous one; out takes 32 int32 (Q31).
   Like every machine, the sound starts in the block after the trig: the VCA keeps the trig's own
   block silent.

   Knobs: PITCH, FINE and the trig's note set the pitch as on every machine; note 60 at PITCH 64
   and FINE 64 plays the sample as recorded. COLOR = start, SHAPE = length (127: to the end),
   SWEEP = which test sample. CONTOUR does nothing yet. DECAY, GATE and PUNCH are the stock amp
   chain. */
#include <stdint.h>
#include "smp_core.h"
#include "test_samples.h"

#ifdef SAMPLER_HOST
#include "host_os.h"                            /* machine/test_sampler.c stands in for the OS */
#else
#define VOICE0      ((uintptr_t)0x42308828)     /* voice of track 0; one per track, VSTRIDE apart */
#define AMP_ENV(v)          ((void (*)(char *))0x400a9252)(v)
#define VCA(o, v)           ((void (*)(int32_t *, char *))0x400a9430)(o, v)
#define PUNCH(o, v)         ((void (*)(int32_t *, char *))0x400a967a)(o, v)
#define DECIMATE(v, in, o)  ((void (*)(char *, int32_t *, int32_t *))0x400a9f58)(v, in, o)
#define LUT_DECAY           ((const int32_t *)0x4011d84c)       /* DECAY -> factor per block (TONE's) */
#define DECIMATOR_STATE(v)  (*(int32_t **)((v) + 0x318))        /* its 8 + 4 frames for this voice */
#endif

#define VSTRIDE     0x31c
#define MARK_OFF    0x2c                        /* cleared by the voice reset when the machine changes */
#define MARK        0x534d5031                  /* 'SMP1' */
#define ROOT        (60 << 16)                  /* the note that plays a sample as recorded */
#define IDLE_LEVEL  (1 << 14)                   /* amp envelope below -102 dB */

#define V32(o)      (*(int32_t *)(v + (o)))
#define P16(o)      (*(const int16_t *)(p + (o)))

static smp_voice voices[6];
static int16_t scratch[SMP_SCRATCH];
/* The decimator reads 8 frames of history and then the block at 96 kHz; it fills the history in
   itself from the voice's state (v+0x318) and stores it back. */
static int32_t buf[8 + SMP_BLOCK2X];

/* 2^(i/12) as Q16.16 */
static const uint32_t SEMITONE[13] = {
    65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715, 131072,
};

static smp_voice *voice_of(const char *v)
{
    return &voices[((uintptr_t)v - VOICE0) / VSTRIDE];
}

static uint32_t mag(int32_t x)
{
    return x < 0 ? 0u - (uint32_t)x : (uint32_t)x;
}

static int32_t knob(const char *p, int off)    /* 8.8 word -> 0..127 */
{
    int32_t k = P16(off) >> 8;

    return k < 0 ? 0 : k > 127 ? 127 : k;
}

/* x * k / 128 for k = 0..128, without overflow for any frame count */
static int32_t part(int32_t x, int32_t k)
{
    return (x >> 7) * k + (((x & 127) * k) >> 7);
}

/* 2^(d/12) as Q16.16 for d semitones (Q16); linear within a semitone (0.7 cents at most). */
static uint32_t ratio_of(int32_t d)
{
    int32_t oct = 0;
    uint32_t s, f, r;

    while (d < 0) {
        d += 12 << 16;
        oct--;
    }
    while (d >= 12 << 16) {
        d -= 12 << 16;
        oct++;
    }
    s = (uint32_t)d >> 16;
    f = (uint32_t)d & 0xffff;
    r = SEMITONE[s] + (((SEMITONE[s + 1] - SEMITONE[s]) * f) >> 16);
    return oct >= 0 ? r << oct : r >> -oct;
}

/* A trig: play the chosen sample from COLOR's start for SHAPE's length. */
static void start(smp_voice *sv, char *v, const char *p)
{
    int32_t which = knob(p, 0x1a), len, from, i;
    int32_t *state = DECIMATOR_STATE(v);

    if (which >= SMP_TEST_COUNT)
        which = SMP_TEST_COUNT - 1;
    len = smp_test[which].length;
    from = part(len, knob(p, 0x16));
    smp_trigger(sv, smp_test[which].pcm, len, from, from + part(len - from, knob(p, 0x18) + 1));
    for (i = 0; i < 12; i++)                   /* no tail of the previous sound */
        state[i] = 0;
}

void sampler_update(int32_t pmod, char *v, const char *p)
{
    smp_voice *sv = voice_of(v);
    int32_t dec, n;

    if (V32(MARK_OFF) != MARK) {                /* first block since the track changed machine */
        sv->active = 0;
        V32(MARK_OFF) = MARK;
    }
    /* the stock amp chain, set up like TONE and MACRO */
    dec = (int8_t)p[0x24];
    dec = dec < 0 ? 0 : dec > 127 ? 127 : dec;
    V32(0x250) = LUT_DECAY[dec];
    if (V32(0x38)) {
        V32(0x274) = V32(0x278) = 0x012c0f26;
        V32(0x280) = V32(0x284) = 0x001f7c5c;
        V32(0x290) = V32(0x294) = 0x7fffffff;
        V32(0x27c) = (int32_t)0x82581e4b;
        V32(0x288) = (int32_t)0x803ef8b8;
        if (P16(0x1e)) {
            V32(0x29c) = 0x20000000;
            V32(0x2a0) = 0x60000000;
            V32(0x2a4) = 0x20000000;
            V32(0x48) = 1;
        } else {
            V32(0x29c) = 0x7fffffff;
            V32(0x2a0) = 0x20000000;
            V32(0x2a4) = 0x20000000;
            V32(0x48) = 0;
        }
        start(sv, v, p);
    }
    /* note = PITCH - 64 + the trig's note + FINE (+-2 semitones), as on every machine */
    n = ((int32_t)P16(0x14) << 8) + (((int32_t)P16(0x22) - 0x4000) << 3) - (64 << 16) + pmod;
    n = n < 0 ? 0 : n > 127 << 16 ? 127 << 16 : n;
    smp_set_ratio(sv, ratio_of(n - ROOT));
}

void sampler_render(int32_t *out, char *v)
{
    smp_voice *sv = voice_of(v);
    int i;

    /* nothing to play: no trig, and the sample has ended or the amp envelope is closed */
    if (!V32(0x34) && !V32(0x38)
        && (!sv->active || (mag(V32(0x230)) < IDLE_LEVEL && mag(V32(0x234)) < IDLE_LEVEL))) {
        for (i = 0; i < SMP_BLOCK; i++)
            out[i] = 0;
        return;
    }
    smp_render(sv, buf + 8, scratch);
    DECIMATE(v, buf, out);
    AMP_ENV(v);
    VCA(out, v);
    PUNCH(out, v);
}
