/* Host tests of the bridge (machine/sampler.c): the voice loop's calls, with stand-ins for the OS.
   Includes sampler.c itself to see its per-track state. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLER_HOST
#include "sampler.c"

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

/* --- stand-ins for the OS ------------------------------------------------------------------- */

_Alignas(8) char host_voices[6 * VSTRIDE];
const int32_t host_lut_decay[128] = { 0 };
static int32_t states[6][12];
static int amp_calls, vca_calls, punch_calls, dec_calls;
static int32_t last_in[8 + SMP_BLOCK2X];

static int track_of(char *v)
{
    return (int)((v - host_voices) / VSTRIDE);
}

void host_amp_env(char *v) { (void)v; amp_calls++; }
void host_vca(int32_t *out, char *v) { (void)out; (void)v; vca_calls++; }
void host_punch(int32_t *out, char *v) { (void)out; (void)v; punch_calls++; }
int32_t *host_state(char *v) { return states[track_of(v)]; }

/* Like the stock decimator's contract: history in from the voice's state, 64 frames in after it,
   32 out, the last 8 inputs back to the state. Its output here is just every other frame. */
void host_decimate(char *v, int32_t *in, int32_t *out)
{
    int32_t *st = states[track_of(v)];
    int i;

    dec_calls++;
    memcpy(in, st, 8 * sizeof *in);
    for (i = 0; i < SMP_BLOCK; i++)
        out[i] = in[8 + 2 * i];
    memcpy(st, in + SMP_BLOCK2X, 8 * sizeof *in);
    memcpy(last_in, in, sizeof last_in);
}

/* --- helpers ----------------------------------------------------------------------------------- */

static char params[0x40];

static void set_param(int off, int value)       /* 8.8 word */
{
    int16_t w = (int16_t)value;

    memcpy(params + off, &w, 2);
}

static void defaults(void)
{
    memset(params, 0, sizeof params);
    set_param(0x14, 64 << 8);                    /* PITCH */
    set_param(0x18, 127 << 8);                   /* SHAPE: length to the end */
    set_param(0x22, 64 << 8);                    /* FINE */
    params[0x24] = 127;                          /* DECAY (high byte of its word) */
}

static char *voice(int t) { return host_voices + t * VSTRIDE; }
static int32_t *v32(int t, int off) { return (int32_t *)(voice(t) + off); }

/* One block of the voice loop for track t: flags, update, render. */
static void block(int t, int trig_now, int trig_prev, int32_t pmod, int32_t *out)
{
    *v32(t, 0x34) = trig_now;
    *v32(t, 0x38) = trig_prev;
    sampler_update(pmod, voice(t), params);
    sampler_render(out, voice(t));
}

static void reset_counts(void) { amp_calls = vca_calls = punch_calls = dec_calls = 0; }

/* --- tests ------------------------------------------------------------------------------------- */

static void test_trig_flow(void)
{
    int32_t out[SMP_BLOCK];
    smp_voice ref;
    static int16_t ref_scratch[SMP_SCRATCH];
    int32_t ref_out[SMP_BLOCK2X];
    int i, blocks = 0, differ = 0;
    const int t = 2;

    defaults();
    memset(host_voices, 0, 6 * VSTRIDE);
    reset_counts();
    block(t, 0, 0, 60 << 16, out);
    CHECK(*v32(t, MARK_OFF) == MARK, "the first update marks the voice");
    CHECK(amp_calls == 0 && dec_calls == 0, "a silent voice runs nothing");
    for (i = 0; i < SMP_BLOCK; i++)
        CHECK(out[i] == 0, "silent block, frame %d = %d", i, out[i]);

    block(t, 1, 0, 60 << 16, out);              /* the trig's block: still silent, chain runs */
    CHECK(!voices[t].active, "nothing plays in the trig's own block");
    CHECK(amp_calls == 1 && vca_calls == 1 && punch_calls == 1 && dec_calls == 1, "chain in the trig block");

    for (i = 0; i < 12; i++)
        states[t][i] = 12345;                   /* a tail that must not leak into the new note */
    *v32(t, 0x34) = 0;
    *v32(t, 0x38) = 1;
    sampler_update(60 << 16, voice(t), params);
    CHECK(voices[t].active && voices[t].pos == 0 && voices[t].end == smp_test[0].length,
          "trig starts the whole pluck: active %d, pos %d, end %d", (int)voices[t].active,
          (int)voices[t].pos, (int)voices[t].end);
    for (i = 0; i < 12; i++)
        CHECK(states[t][i] == 0, "decimator state %d not cleared", i);
    CHECK(voices[t].step_int == 0 && voices[t].step_frac == 1u << 31, "note 60 plays as recorded");

    /* the frames handed to the decimator are exactly the core's */
    smp_trigger(&ref, smp_test[0].pcm, smp_test[0].length, 0, smp_test[0].length);
    smp_set_ratio(&ref, 1u << 16);
    sampler_render(out, voice(t));
    smp_render(&ref, ref_out, ref_scratch);
    for (i = 0; i < SMP_BLOCK2X; i++)
        differ += last_in[8 + i] != ref_out[i];
    CHECK(differ == 0, "%d frames differ from the core's", differ);

    /* play on to the end: then silence, and nothing runs */
    *v32(t, 0x38) = 0;
    *v32(t, 0x230) = *v32(t, 0x234) = 0x7fffffff;   /* the envelope stays open */
    while (voices[t].active && blocks < 10000) {
        block(t, 0, 0, 60 << 16, out);
        blocks++;
    }
    CHECK(!voices[t].active, "the pluck never ended");
    CHECK(blocks > smp_test[0].length / SMP_BLOCK - 2 && blocks < smp_test[0].length / SMP_BLOCK + 3,
          "the pluck took %d blocks", blocks);
    reset_counts();
    block(t, 0, 0, 60 << 16, out);
    CHECK(amp_calls == 0 && dec_calls == 0, "an ended sample runs nothing");
    for (i = 0; i < SMP_BLOCK; i++)
        CHECK(out[i] == 0, "after the end, frame %d = %d", i, out[i]);
}

static void test_knobs(void)
{
    int32_t out[SMP_BLOCK];
    int32_t len;
    const int t = 0;

    defaults();
    memset(host_voices, 0, 6 * VSTRIDE);
    set_param(0x16, 64 << 8);                   /* COLOR: start in the middle */
    set_param(0x18, 63 << 8);                   /* SHAPE: half of the rest */
    set_param(0x1a, 1 << 8);                    /* SWEEP: the hat */
    block(t, 0, 1, 60 << 16, out);
    len = smp_test[1].length;
    CHECK(voices[t].pcm == smp_test[1].pcm, "SWEEP 1 picks the hat");
    CHECK(voices[t].start == len / 2 && voices[t].end == len / 2 + (len - len / 2) / 2,
          "start %d end %d for length %d", (int)voices[t].start, (int)voices[t].end, (int)len);

    set_param(0x1a, 100 << 8);                  /* past the last sample: the last one */
    set_param(0x16, 127 << 8);
    set_param(0x18, 0);
    block(t, 0, 1, 60 << 16, out);
    len = smp_test[SMP_TEST_COUNT - 1].length;
    CHECK(voices[t].pcm == smp_test[SMP_TEST_COUNT - 1].pcm, "SWEEP clamps to the last sample");
    {
        int32_t from = (int32_t)((int64_t)len * 127 / 128), to = from + (len - from) / 128;

        CHECK(voices[t].start == from && voices[t].end == to,
              "COLOR 127, SHAPE 0: %d..%d, want %d..%d", (int)voices[t].start, (int)voices[t].end, (int)from,
              (int)to);
        CHECK(to > from, "SHAPE 0 still plays a frame");
    }
}

static void test_pitch(void)
{
    static const struct { int pitch, fine, note; uint32_t ratio_q16; } cases[] = {
        { 64, 64, 60, 65536 },                  /* as recorded */
        { 64, 64, 72, 131072 },                 /* an octave up */
        { 64, 64, 48, 32768 },                  /* an octave down */
        { 76, 64, 60, 131072 },                 /* PITCH +12 */
        { 64, 96, 60, 69433 },                  /* FINE +32 steps: +1 semitone */
        { 64, 0, 60, 58386 },                   /* FINE -64: -2 semitones */
        { 64, 64, 0, 2048 },                    /* note 0: 5 octaves down */
        { 127, 64, 127, 524288 },               /* far up: clamped to a ratio of 8 */
    };
    int32_t out[SMP_BLOCK];
    unsigned c;

    for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        smp_voice want;
        uint64_t got, exp;

        defaults();
        memset(host_voices, 0, 6 * VSTRIDE);
        set_param(0x14, cases[c].pitch << 8);
        set_param(0x22, cases[c].fine << 8);
        block(1, 0, 0, cases[c].note << 16, out);
        smp_set_ratio(&want, cases[c].ratio_q16);
        got = ((uint64_t)voices[1].step_int << 32) | voices[1].step_frac;
        exp = ((uint64_t)want.step_int << 32) | want.step_frac;
        CHECK(got == exp || (got > exp ? got - exp : exp - got) < exp / 1000,
              "PITCH %d FINE %d note %d: step %.6f, want %.6f", cases[c].pitch, cases[c].fine, cases[c].note,
              got / 4294967296.0, exp / 4294967296.0);
    }
}

static void test_machine_change(void)
{
    int32_t out[SMP_BLOCK];

    defaults();
    memset(host_voices, 0, 6 * VSTRIDE);
    block(3, 0, 1, 60 << 16, out);
    CHECK(voices[3].active, "playing");
    *v32(3, MARK_OFF) = 0;                      /* the voice reset of a machine change */
    block(3, 0, 0, 60 << 16, out);
    CHECK(!voices[3].active, "a machine change stops the sample");
    CHECK(*v32(3, MARK_OFF) == MARK, "and marks the voice again");
}

static void test_tracks(void)
{
    int32_t out[SMP_BLOCK];

    defaults();
    memset(host_voices, 0, 6 * VSTRIDE);
    block(0, 0, 1, 60 << 16, out);
    set_param(0x1a, 2 << 8);
    block(5, 0, 1, 72 << 16, out);
    CHECK(voices[0].pcm == smp_test[0].pcm && voices[5].pcm == smp_test[2].pcm, "each track its own sample");
    CHECK(voices[0].step_frac == 1u << 31 && voices[5].step_int == 1 && voices[5].step_frac == 0,
          "each track its own pitch");
}

int main(void)
{
    test_trig_flow();
    test_knobs();
    test_pitch();
    test_machine_change();
    test_tracks();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all bridge tests passed\n");
    return 0;
}
