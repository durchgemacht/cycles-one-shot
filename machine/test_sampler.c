/* Host tests of SAMPLE's bridge (machine/sampler.c), sample memory (machine/pool.c) and screen view
   (machine/view.c), with stand-ins for the OS: voices, the audio side's parameter copy, the kits, the
   file system, the screen. Includes the files to see their state. With VIEW_PREVIEW set to a folder,
   also writes the views it draws there as PBM pictures. */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLER_HOST
#include "sampler.c"
#include "pool.c"
#include "view.c"

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
_Alignas(8) char host_targets[6 * 66];
void *host_cur_snd[6];
_Alignas(8) char host_kits[96 * 7094];
_Alignas(8) static char work_kit[7094];
char *host_play_kit;
_Alignas(8) char host_pool[1 << 20];
unsigned long host_pool_size = sizeof host_pool;
uint32_t host_fs_mounted = 1;
void *host_fs_holder, *host_task = (void *)0x1000;
int host_value_shown, host_trig_held, host_track, host_main_draws, host_knob_draws;
const void *host_font;
const char host_font_small[] = "small", host_font_medium[] = "medium";
static int live_kit;                            /* the pattern whose kit is live */
static int32_t states[6][12];
static int amp_calls, dec_calls;

/* The OS's text, recorded; in the pictures each character is a box (5 or 7 pixels apart, 7 high,
   from y up, centred on x with flag 2). */
typedef struct {
    const void *font;
    int x, y, flags;
    char s[32];
} text_call;
static text_call texts[8];
static int ntexts;

void host_draw_text(void *bmp, void *font, int32_t x, int32_t y, int32_t flags, const char *fmt, ...)
{
    smp_bitmap *b = bmp;
    text_call *t = &texts[ntexts < 8 ? ntexts++ : 7];
    int step = host_font == FONT_SMALL ? 5 : 7, n, i, dx, dy;
    va_list ap;

    (void)font;
    va_start(ap, fmt);
    vsnprintf(t->s, sizeof t->s, fmt, ap);
    va_end(ap);
    t->font = host_font;
    t->x = x;
    t->y = y;
    t->flags = flags;
    n = (int)strlen(t->s);
    if (flags & 2)
        x -= n * step / 2;
    for (i = 0; i < n; i++)
        if (t->s[i] != ' ')
            for (dx = 0; dx < step - 1; dx++)
                for (dy = 0; dy < 7; dy++)
                    if (dx == 0 || dy == 0 || dx == step - 2 || dy == 6)
                        pixel(b, x + i * step + dx, y + dy);
}

static int track_index(char *v) { return (int)((v - host_voices) / VSTRIDE); }
void host_amp_env(char *v) { (void)v; amp_calls++; }
void host_vca(int32_t *out, char *v) { (void)out; (void)v; }
void host_punch(int32_t *out, char *v) { (void)out; (void)v; }
int32_t *host_state(char *v) { return states[track_index(v)]; }
char *host_sound(int t) { return KIT(live_kit) + KIT_SOUNDS + 100 * t; }

void host_decimate(char *v, int32_t *in, int32_t *out)
{
    int32_t *st = states[track_index(v)];
    int i;

    dec_calls++;
    memcpy(in, st, 8 * sizeof *in);
    for (i = 0; i < SMP_BLOCK; i++)
        out[i] = in[8 + 2 * i];
    memcpy(st, in + SMP_BLOCK2X, 8 * sizeof *in);
}

/* The drive: a few files, each at an inode with an id; reads count. */
typedef struct {
    uint32_t ino, id, size;
    unsigned char *data;
} file_t;

static file_t files[8];
static int nfiles, reads, entries, finds, locked, nested;

/* The file system's lock: its own calls must not run while the tick holds it (on the device an
   inner give-back would free it). */
void host_fs_lock(void) { nested += locked; locked = 1; }
void host_fs_unlock(void) { locked = 0; }

int32_t host_find_id(uint32_t id)
{
    int i;

    finds++;
    nested += !locked;                          /* must be called under the lock */
    for (i = 0; i < nfiles; i++)
        if (files[i].id == id)
            return i;
    return -1;
}

uint32_t host_id_ino(int32_t i) { return files[i].ino; }

int32_t host_fs_entry(uint32_t ino, uint32_t *h)
{
    int i;

    entries++;
    nested += locked;
    for (i = 0; i < nfiles; i++)
        if (files[i].ino == ino) {
            h[0] = ino;
            h[1] = files[i].id;
            h[2] = files[i].size;
            h[3] = 0;
            return 0;
        }
    return -1;
}

int32_t host_fs_read(void *buf, uint32_t len, uint32_t *fh)
{
    int i;

    reads++;
    nested += locked;
    for (i = 0; i < nfiles; i++)
        if (files[i].ino == fh[0] && fh[3]) {
            uint32_t n = fh[1] >= files[i].size ? 0 : files[i].size - fh[1];

            if (n > len)
                n = len;
            memcpy(buf, files[i].data + fh[1], n);
            fh[1] += n;
            return (int32_t)n;
        }
    return -1;
}

/* --- helpers ----------------------------------------------------------------------------------- */

static void be32_put(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

/* A sample file in the device's format; PCM in the host's byte order (the target's is big-endian,
   which is its own). value(i) gives sample i (both channels interleaved when stereo). */
static file_t *add_file(uint32_t ino, uint32_t id, uint32_t samples, int stereo, uint32_t rate,
                        int16_t (*value)(uint32_t))
{
    uint32_t bytes = samples * 2, i;
    file_t *f = &files[nfiles++];

    f->ino = ino;
    f->id = id;
    f->size = 64 + bytes + 16;
    f->data = calloc(1, f->size);
    f->data[1] = stereo ? 1 : 0;
    be32_put(f->data + 4, bytes);
    be32_put(f->data + 8, rate);
    be32_put(f->data + 16, samples - 1);
    be32_put(f->data + 20, 0x7f000000);
    for (i = 0; i < samples; i++) {
        int16_t s = value(i);

        memcpy(f->data + 64 + 2 * i, &s, 2);
    }
    return f;
}

static void drop_files(void)
{
    while (nfiles)
        free(files[--nfiles].data);
}

static int16_t ramp(uint32_t i) { return (int16_t)(i * 7 - 20000); }
static int16_t lr(uint32_t i) { return (int16_t)(i & 1 ? -1000 - (int)(i >> 1) : 3000 + (int)(i >> 1)); }

/* A SAMPLE sound naming the file at inode ino with id. */
static void make_sound(char *s, uint32_t ino, uint32_t id)
{
    uint16_t *k = (uint16_t *)(s + 70);

    memset(s, 0, 100);
    s[38] = SAMPLE_MACHINE;
    k[0] = (uint16_t)ino;
    k[1] = (uint16_t)(id >> 16);
    k[2] = (uint16_t)id;
}

static void reset_all(void)
{
    drop_files();
    memset(host_kits, 0, sizeof host_kits);
    memset(host_targets, 0, sizeof host_targets);
    memset(host_cur_snd, 0, sizeof host_cur_snd);
    memset(host_voices, 0, 6 * VSTRIDE);
    memset(smp_slots, 0, sizeof smp_slots);
    memset(voices, 0, sizeof voices);
    memset(rates, 0, sizeof rates);
    memset(live_id, 0, sizeof live_id);
    host_pool_size = sizeof host_pool;
    live_kit = 0;
    busy = 0;
    reads = 0;
    entries = 0;
    finds = 0;
    retry_id = 0;
    host_fs_mounted = 1;
    host_fs_holder = 0;
    memset(work_kit, 0, sizeof work_kit);
    host_play_kit = 0;
    host_value_shown = 0;
    host_trig_held = 0;
    host_track = 0;
    host_main_draws = host_knob_draws = 0;
    ntexts = 0;
}

static int tick_until(uint32_t id, int state, int limit)
{
    int n;

    for (n = 1; n <= limit; n++) {
        smp_slot *sl;

        smp_ui_tick();
        sl = slot_of(id);
        if (sl && (int)sl->state == state)
            return n;
    }
    return 0;
}

static char params[0x40];

static void set_param(int off, int value)
{
    int16_t w = (int16_t)value;

    memcpy(params + off, &w, 2);
}

static void defaults(void)
{
    memset(params, 0, sizeof params);
    set_param(0x14, 64 << 8);                    /* PITCH */
    set_param(0x18, 120 << 8);                   /* Length: the whole sample */
    set_param(0x22, 64 << 8);                    /* FINE */
    params[0x24] = 127;                          /* DECAY */
}

static char *voice(int t) { return host_voices + t * VSTRIDE; }
static int32_t *v32(int t, int off) { return (int32_t *)(voice(t) + off); }

/* The audio side's copy of track t's sound parameters names id (k 26/27). */
static void target_names(int t, uint32_t id)
{
    uint16_t *k = (uint16_t *)(host_targets + 66 * t + 52);

    k[0] = (uint16_t)(id >> 16);
    k[1] = (uint16_t)id;
}

static void block(int t, int trig_prev, int32_t pmod, int32_t *out)
{
    *v32(t, 0x34) = 0;
    *v32(t, 0x38) = trig_prev;
    sampler_update(pmod, voice(t), params);
    sampler_render(out, voice(t));
}

/* --- tests ------------------------------------------------------------------------------------- */

static void test_browser(void)
{
    void *result[2] = { (void *)1, (void *)1 };
    uint32_t h[4];
    char *live;

    reset_all();
    add_file(10, 0x12345, 1000, 0, 48000, ramp);
    live = host_sound(1);
    make_sound(live, 0, 0);
    memcpy(live + 4, "MY SOUND", 8);
    host_fs_entry(10, h);
    CHECK(smp_browser_open(result, h, 1) == 1, "a sample on a SAMPLE track is taken");
    CHECK(result[0] == pv_src && result[1] == 0, "result {copy, no control block}");
    CHECK(smp_sound_id((char *)pv_src) == 0x12345 && smp_sound_ino((char *)pv_src) == 10, "the copy names the file");
    CHECK(memcmp((char *)pv_src + 4, "MY SOUND", 8) == 0 && ((char *)pv_src)[38] == SAMPLE_MACHINE,
          "the copy keeps the track's sound");
    CHECK(smp_find(0x12345) == 0, "the browser loads nothing itself");

    live[38] = 4;                                       /* TONE */
    CHECK(smp_browser_open(result, h, 1) == 0, "not on another machine");
    live[38] = SAMPLE_MACHINE;
    h[2] = 100;
    CHECK(smp_browser_open(result, h, 1) == 0, "a file of preset size goes to the OS");
    host_fs_entry(10, h);
    h[1] = 0x12344;
    CHECK(smp_browser_open(result, h, 1) == 0, "an even id is no file");
    be32_put(files[0].data + 8, 257);                   /* rate 257 Hz */
    host_fs_entry(10, h);
    CHECK(smp_browser_open(result, h, 1) == 0, "a header out of range goes to the OS");
    CHECK(smp_browser_open(result, h, 7) == 0, "no track 7");
}

static void test_load_mono(void)
{
    smp_slot *sl;
    int ticks, i, bad = 0;

    reset_all();
    add_file(20, 0x77, 300000, 0, 48000, ramp);          /* 600 000 bytes: 5 reads of 128 KiB */
    make_sound(KIT(5) + KIT_POOL + 100 * 3, 20, 0x77);    /* a pool sound of pattern 6 */
    ticks = tick_until(0x77, SLOT_READY, 20);
    sl = slot_of(0x77);
    CHECK(ticks >= 5 && ticks <= 8, "ready after %d ticks", ticks);
    CHECK(sl && sl->frames == 300000 && sl->rate_q16 == 1u << 16, "frames %d rate %u", sl ? sl->frames : -1,
          sl ? sl->rate_q16 : 0);
    for (i = 0; sl && i < 300000; i++)
        bad += sl->pcm[i] != ramp(i);
    CHECK(bad == 0, "%d frames differ from the file", bad);
    CHECK(sl && (char *)sl->pcm >= host_pool && (char *)sl->pcm + 600000 <= host_pool + sizeof host_pool,
          "inside the pool");
}

static void test_stereo_rate(void)
{
    smp_slot *sl;
    int i, bad = 0, ticks;

    reset_all();
    add_file(30, 0x99, 2000, 1, 24000, lr);              /* 1000 frames L/R */
    make_sound(KIT(0) + KIT_SOUNDS, 30, 0x99);
    tick_until(0x99, SLOT_READY, 5);
    sl = slot_of(0x99);
    CHECK(sl && sl->state == SLOT_READY && sl->frames == 1000, "stereo: 1000 frames");
    for (i = 0; sl && i < 1000; i++)
        bad += sl->pcm[i] != (int16_t)((lr(2 * i) + lr(2 * i + 1)) >> 1);
    CHECK(bad == 0, "%d frames are not the mix of both channels", bad);
    CHECK(sl && sl->size == 2000 && sl->rate_q16 == 1u << 15, "half the memory after mixing; 24 kHz = 0.5");

    reset_all();
    add_file(31, 0x9b, 2 * 150001, 1, 44100, lr);         /* 600 004 bytes: 5 reads, mixed as they come */
    make_sound(KIT(0) + KIT_SOUNDS, 31, 0x9b);
    ticks = tick_until(0x9b, SLOT_READY, 10);
    sl = slot_of(0x9b);
    bad = 0;
    for (i = 0; sl && i < 150001; i++)
        bad += sl->pcm[i] != (int16_t)((lr(2 * i) + lr(2 * i + 1)) >> 1);
    CHECK(sl && sl->frames == 150001 && bad == 0, "long stereo: %d frames, %d wrong, %d ticks",
          sl ? sl->frames : -1, bad, ticks);
    CHECK(sl && sl->rate_q16 == 60211, "44.1 kHz = 0.91875: %u", sl ? sl->rate_q16 : 0);
}

static void test_moved(void)
{
    smp_slot *sl;
    int ticks;

    reset_all();
    add_file(5, 0x101, 100, 0, 48000, ramp);             /* another file sits at the old inode */
    add_file(40000, 0x333, 500, 0, 48000, ramp);
    make_sound(KIT(1) + KIT_SOUNDS + 100 * 4, 5, 0x333);
    ticks = tick_until(0x333, SLOT_READY, 5);
    sl = slot_of(0x333);
    CHECK(sl && sl->state == SLOT_READY && sl->ino == 40000, "found at its new inode: %u", sl ? sl->ino : 0);
    CHECK(ticks == 2 && finds == 1 && entries == 3, "through the index: %d ticks, %d finds, %d asks",
          ticks, finds, entries);
}

static void test_missing(void)
{
    int32_t out[SMP_BLOCK];
    int i;

    reset_all();
    defaults();
    add_file(9, 0x557, 100, 0, 48000, ramp);             /* another file at its inode */
    make_sound(KIT(0) + KIT_SOUNDS + 100 * 2, 9, 0x555);
    CHECK(tick_until(0x555, SLOT_FAILED, 1), "a file neither at its inode nor in the index fails at once");
    entries = finds = 0;
    for (i = 0; i < 50; i++)
        smp_ui_tick();
    CHECK(entries == 0 && finds == 0, "and is not looked for again: %d asks, %d finds", entries, finds);
    target_names(2, 0x555);
    block(2, 1, 60 << 16, out);
    CHECK(!voices[2].active, "its notes stay silent");
}

static void test_retry(void)
{
    char *s = KIT(0) + KIT_SOUNDS;
    void *result[2];
    uint32_t h[4];

    reset_all();
    make_sound(s, 77, 0x7701);
    tick_until(0x7701, SLOT_FAILED, 2);
    CHECK(slot_of(0x7701) && slot_of(0x7701)->state == SLOT_FAILED, "missing: failed");
    add_file(77, 0x7701, 1000, 0, 48000, ramp);           /* back, at the same inode */
    host_fs_entry(77, h);
    smp_browser_open(result, h, 0);                      /* chosen again */
    CHECK(tick_until(0x7701, SLOT_READY, 5), "chosen again in the browser, it loads");
}

static void test_trig(void)
{
    int32_t out[SMP_BLOCK];
    const smp_slot *sl;
    smp_voice want;

    reset_all();
    defaults();
    add_file(40, 0x4441, 9600, 0, 24000, ramp);
    make_sound(host_sound(2), 40, 0x4441);
    tick_until(0x4441, SLOT_READY, 5);
    sl = smp_find(0x4441);
    CHECK(sl != 0, "loaded");
    set_param(0x16, 30 << 8);                           /* Start 30 of 120: a quarter in */
    target_names(2, 0x4441);
    block(2, 1, 60 << 16, out);
    CHECK(sl && voices[2].active && voices[2].pcm == sl->pcm && voices[2].start == 2400 && voices[2].end == 9600,
          "plays from 2400 to 9600: active %d start %d end %d", (int)voices[2].active, (int)voices[2].start,
          (int)voices[2].end);
    smp_set_ratio(&want, 1u << 15);                     /* a 24 kHz sample at note 60: half speed */
    CHECK(voices[2].step_int == want.step_int && voices[2].step_frac == want.step_frac,
          "its rate keeps its pitch: step %u.%08x", voices[2].step_int, voices[2].step_frac);
    CHECK(dec_calls > 0 && amp_calls > 0, "the chain runs");

    target_names(3, 0x4443);                            /* not loaded */
    block(3, 1, 60 << 16, out);
    CHECK(!voices[3].active, "a sample not loaded stays silent");
}

/* Start and Length as on the Model:Samples: the end at Start + Length, so moving Start keeps the
   length; at most the sample's end; fractions count; 0.2's values past 120 count as 120. */
static void test_start_length(void)
{
    static const struct {
        int16_t start, length;
        int32_t from, to;
    } cases[] = {
        { 0 << 8, 30 << 8, 0, 2400 },                   /* a quarter */
        { 60 << 8, 30 << 8, 4800, 7200 },               /* moved on: the same 2400 frames */
        { 100 << 8, 30 << 8, 8000, 9600 },              /* up to the end, no further */
        { 0x0780, 0x0780, 600, 1200 },                  /* 7.5 and 7.5 */
        { 0, 127 << 8, 0, 9600 },                       /* 0.2's default Length: all of it */
        { 127 << 8, 120 << 8, 9600, 9600 },             /* 0.2's top Start: at the end, silent */
        { 40 << 8, 0, 3200, 3200 },                     /* Length 0: silent */
    };
    int32_t out[SMP_BLOCK];
    unsigned i;

    reset_all();
    defaults();
    add_file(41, 0x4451, 9600, 0, 48000, ramp);
    make_sound(host_sound(1), 41, 0x4451);
    tick_until(0x4451, SLOT_READY, 5);
    target_names(1, 0x4451);
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int on = cases[i].from < cases[i].to;

        set_param(0x16, cases[i].start);
        set_param(0x18, cases[i].length);
        block(1, 1, 60 << 16, out);
        CHECK(voices[1].active == on && (!on || (voices[1].start == cases[i].from && voices[1].end == cases[i].to)),
              "Start %d.%02d Length %d.%02d: active %d, %d to %d, want %d to %d", cases[i].start >> 8,
              (cases[i].start & 255) * 100 / 256, cases[i].length >> 8, (cases[i].length & 255) * 100 / 256,
              (int)voices[1].active, (int)voices[1].start, (int)voices[1].end, (int)cases[i].from,
              (int)cases[i].to);
    }
}

static void test_play_kit(void)
{
    char *s = work_kit + KIT_POOL + 100 * 63;

    reset_all();
    add_file(55, 0x5501, 1000, 0, 48000, ramp);
    make_sound(s, 55, 0x5501);                         /* a pool sound only the working kit has */
    smp_ui_tick();
    CHECK(slot_of(0x5501) == 0, "unknown while the audio side plays no kit");
    host_play_kit = work_kit;
    CHECK(tick_until(0x5501, SLOT_READY, 4), "loaded from the kit the audio side plays");
}

static void test_retire(void)
{
    int32_t out[SMP_BLOCK];
    smp_slot *sl;
    char *s = KIT(7) + KIT_SOUNDS;

    reset_all();
    defaults();
    add_file(50, 0x5001, 48000, 0, 48000, ramp);
    make_sound(s, 50, 0x5001);
    tick_until(0x5001, SLOT_READY, 5);
    target_names(0, 0x5001);
    block(0, 1, 60 << 16, out);                         /* a voice plays it */
    memset(s, 0, 100);                                  /* nothing names it any more */
    smp_ui_tick();
    sl = slot_of(0x5001);
    CHECK(sl && sl->state == SLOT_READY, "kept while a voice plays it");
    voices[0].active = 0;
    smp_ui_tick();
    CHECK(slot_of(0x5001) == 0, "freed once no voice plays it");
}

static void test_nomem(void)
{
    char *a = KIT(0) + KIT_SOUNDS, *b = KIT(0) + KIT_SOUNDS + 100;

    reset_all();
    host_pool_size = 100000;
    add_file(60, 0x6001, 30000, 0, 48000, ramp);         /* 60 000 bytes */
    add_file(61, 0x6003, 30000, 0, 48000, ramp);
    make_sound(a, 60, 0x6001);
    tick_until(0x6001, SLOT_READY, 5);
    make_sound(b, 61, 0x6003);
    CHECK(tick_until(0x6003, SLOT_NOMEM, 5) && !tick_until(0x6003, SLOT_READY, 5), "no room for the second");
    memset(a, 0, 100);
    CHECK(tick_until(0x6003, SLOT_READY, 5), "it loads once the first is gone");
}

static void test_records(void)
{
    _Alignas(4) char sound[100], rec[100], back[100];

    reset_all();
    make_sound(sound, 321, 0xabcdef);
    memset(rec, 0, sizeof rec);
    *(int32_t *)(rec + 4) = 2;
    smp_after_save(rec, sound);
    CHECK(*(uint16_t *)(rec + 76) == 321 && *(uint16_t *)(rec + 78) == 0xab && *(uint16_t *)(rec + 80) == 0xcdef,
          "save puts inode and id in record slots 24..26");
    memset(back, 0, sizeof back);
    back[38] = SAMPLE_MACHINE;
    smp_after_load(back, rec);
    CHECK(smp_sound_id(back) == 0xabcdef && smp_sound_ino(back) == 321, "load gets them back");
    memset(back, 0, sizeof back);
    back[38] = 2;
    smp_after_load(back, rec);
    CHECK(smp_sound_id(back) == 0, "only into a SAMPLE sound");
    *(int32_t *)(rec + 4) = 1;
    back[38] = SAMPLE_MACHINE;
    smp_after_load(back, rec);
    CHECK(smp_sound_id(back) == 0, "only from a version 2 record");
    memset(rec, 0, sizeof rec);
    sound[38] = 3;
    smp_after_save(rec, sound);
    CHECK(*(uint16_t *)(rec + 78) == 0, "a sound of another machine saves nothing extra");
}

static void test_live(void)
{
    reset_all();
    host_cur_snd[1] = (void *)0x1234;
    host_cur_snd[2] = (void *)0x5678;
    make_sound(host_sound(1), 3, 0x31);
    smp_ui_tick();
    CHECK(host_cur_snd[1] == 0 && host_cur_snd[2] == (void *)0x5678, "a new sample on track 2 makes its next note copy the sound");
    host_cur_snd[1] = (void *)0x1234;
    smp_ui_tick();
    CHECK(host_cur_snd[1] == (void *)0x1234, "and only when it changes");
}

static void test_lock(void)
{
    reset_all();
    add_file(70, 0x7001, 100, 0, 48000, ramp);
    make_sound(host_sound(0), 70, 0x7001);
    busy = 1;
    smp_ui_tick();
    CHECK(slot_of(0x7001) == 0 && reads == 0, "a tick that finds the lock taken does nothing");
    busy = 0;
}

static void test_drive_guard(void)
{
    void *result[2];
    uint32_t h[4];

    reset_all();
    add_file(90, 0x9001, 1000, 0, 48000, ramp);
    make_sound(host_sound(0), 90, 0x9001);
    host_fs_holder = host_task;                 /* inside the file system's own work */
    smp_ui_tick();
    smp_ui_tick();
    host_fs_entry(90, h);
    entries = 0;
    reads = 0;
    CHECK(smp_browser_open(result, h, 0) == 0, "the browser leaves it to the OS");
    CHECK(entries == 0 && reads == 0 && slot_of(0x9001) && slot_of(0x9001)->state == SLOT_WANT,
          "no file system call from within its own work: %d asks, %d reads", entries, reads);
    host_fs_holder = (void *)0x2000;            /* another task holds it: wait for it, as the OS does */
    CHECK(tick_until(0x9001, SLOT_READY, 3), "loads while another task holds the lock");

    reset_all();
    add_file(91, 0x9101, 1000, 0, 48000, ramp);
    make_sound(host_sound(0), 91, 0x9101);
    host_fs_mounted = 0;
    smp_ui_tick();
    smp_ui_tick();
    CHECK(entries == 0 && slot_of(0x9101)->state == SLOT_WANT, "nothing before the drive is mounted");
    host_fs_mounted = 1;
    CHECK(tick_until(0x9101, SLOT_READY, 3), "then it loads");
}

/* A sine of 440 Hz at the rate of the file being made. */
static uint32_t sine_rate;
static int16_t sine440(uint32_t i)
{
    return (int16_t)lrint(16383.0 * sin(2 * 3.14159265358979323846 * 440.0 * i / sine_rate));
}

/* Plays track t's sample from a trig at note 60 for a second and returns the frequency of what
   comes out, from its rising zero crossings. */
static double played_hz(int t)
{
    int32_t out[SMP_BLOCK], prev = 0;
    long frame = 0, first = -1, last = -1;
    int b, i, crossings = 0;

    *v32(t, 0x230) = 0x7fffffff;                /* the amp envelope's stand-in stays open */
    block(t, 1, 60 << 16, out);
    for (b = 0; b < 1500; b++) {
        block(t, 0, 60 << 16, out);
        for (i = 0; i < SMP_BLOCK; i++, frame++) {
            if (prev < 0 && out[i] >= 0) {
                if (first < 0)
                    first = frame;
                else {
                    last = frame;
                    crossings++;
                }
            }
            prev = out[i];
        }
    }
    return crossings && last > first ? crossings * 48000.0 / (double)(last - first) : 0;
}

static void test_rate_pitch(void)
{
    static const uint32_t rates_hz[] = { 48000, 44100, 46875 };
    int k;

    for (k = 0; k < 3; k++) {
        uint32_t id = 0xa001 + 2 * k;
        double hz;

        reset_all();
        defaults();
        sine_rate = rates_hz[k];
        add_file(100 + k, id, 3 * sine_rate, 0, sine_rate, sine440);
        make_sound(host_sound(1), 100 + k, id);
        tick_until(id, SLOT_READY, 10);
        target_names(1, id);
        hz = played_hz(1);
        CHECK(fabs(hz - 440.0) < 0.2, "a 440 Hz sine recorded at %u Hz plays at %.2f Hz", rates_hz[k], hz);
    }
}

static void test_machine_change(void)
{
    int32_t out[SMP_BLOCK];

    reset_all();
    defaults();
    add_file(80, 0x8001, 4800, 0, 48000, ramp);
    make_sound(host_sound(3), 80, 0x8001);
    tick_until(0x8001, SLOT_READY, 5);
    target_names(3, 0x8001);
    block(3, 1, 60 << 16, out);
    CHECK(voices[3].active, "playing");
    *v32(3, MARK_OFF) = 0;                      /* the voice reset of a machine change */
    block(3, 0, 60 << 16, out);
    CHECK(!voices[3].active, "a machine change stops the sample");
}

/* --- the screen ------------------------------------------------------------------------------ */

static uint32_t screen_words[128 * 2];
static smp_bitmap screen = { 0, 128, 64, 2, screen_words };

static int pix(int x, int y) { return (int)((screen_words[x * 2 + (y >> 5)] >> (31 - (y & 31))) & 1); }

/* a 220 Hz tone that swells for 30 ms and dies away over hit_frames; loudest past column 0 */
static uint32_t hit_frames;
static int16_t hit(uint32_t i)
{
    double env = i < 1440 ? i / 1440.0 : exp(-4.0 * (i - 1440) / hit_frames);

    return (int16_t)lrint(30000.0 * env * sin(2 * 3.14159265358979 * 220.0 * i / 48000.0));
}

static void set_knob(char *sound, int at, int value)
{
    int16_t w = (int16_t)(value << 8);

    memcpy(sound + at, &w, 2);
}

/* With VIEW_PREVIEW set to a folder: the screen as a PBM picture there. */
static void preview(const char *name)
{
    const char *dir = getenv("VIEW_PREVIEW");
    char path[512];
    FILE *f;
    int x, y;

    if (!dir)
        return;
    snprintf(path, sizeof path, "%s/%s.pbm", dir, name);
    if (!(f = fopen(path, "w")))
        return;
    fprintf(f, "P1\n128 64\n");
    for (y = 63; y >= 0; y--) {
        for (x = 0; x < 128; x++)
            fputc(pix(x, y) ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
}

/* The columns of a READY slot against a count over its frames. */
static int overview_errors(const smp_slot *sl)
{
    int i = (int)(sl - smp_slots), bad = 0;
    uint32_t c, f;

    for (c = 0; c < SMP_COLUMNS; c++) {
        uint32_t a = c * (uint32_t)sl->frames / SMP_COLUMNS, b = (c + 1) * (uint32_t)sl->frames / SMP_COLUMNS;
        int16_t lo = 32767, hi = -32768;

        for (f = a; f < b; f++) {
            if (sl->pcm[f] < lo)
                lo = sl->pcm[f];
            if (sl->pcm[f] > hi)
                hi = sl->pcm[f];
        }
        bad += ov_lo[i][c] != lo || ov_hi[i][c] != hi;
    }
    return bad;
}

static void test_overview(void)
{
    smp_slot *sl;
    int i, empty = 0;

    reset_all();
    add_file(110, 0xb101, 1000, 0, 48000, ramp);
    make_sound(host_sound(0), 110, 0xb101);
    tick_until(0xb101, SLOT_READY, 5);
    sl = slot_of(0xb101);
    CHECK(sl && overview_errors(sl) == 0, "a short mono sample's columns");

    reset_all();
    add_file(111, 0xb103, 2 * 150001, 1, 48000, lr);     /* five reads, mixed as they come */
    make_sound(host_sound(0), 111, 0xb103);
    tick_until(0xb103, SLOT_READY, 10);
    sl = slot_of(0xb103);
    CHECK(sl && overview_errors(sl) == 0, "a long stereo sample's columns, after mixing");

    reset_all();
    add_file(112, 0xb105, 50, 0, 48000, ramp);           /* fewer frames than columns */
    make_sound(host_sound(0), 112, 0xb105);
    tick_until(0xb105, SLOT_READY, 5);
    sl = slot_of(0xb105);
    for (i = 0; sl && i < SMP_COLUMNS; i++)
        empty += ov_lo[sl - smp_slots][i] > ov_hi[sl - smp_slots][i];
    CHECK(sl && overview_errors(sl) == 0 && empty == SMP_COLUMNS - 50, "50 frames fill 50 columns: %d empty", empty);
}

static void test_view(void)
{
    char *s, view[0x240];
    int32_t p;
    int x, y, ok, any, below, pairs;

    reset_all();
    hit_frames = 48000;
    add_file(120, 0xc101, 48000, 0, 48000, hit);
    s = host_sound(2);
    make_sound(s, 120, 0xc101);
    memcpy(s + 4, "KICK 01", 8);
    tick_until(0xc101, SLOT_READY, 5);
    host_track = 2;

    set_knob(s, 42, 0);
    set_knob(s, 44, 120);
    smp_main_draw(0, &screen);
    CHECK(host_main_draws == 0, "the main screen of a SAMPLE track is ours");
    CHECK(ntexts == 1 && !strcmp(texts[0].s, "KICK 01") && texts[0].font == FONT_SMALL && texts[0].x == 1 &&
          texts[0].y == NAME_Y && texts[0].flags == 0, "its name, small, top left");
    CHECK(pix(0, RULE_Y) && !pix(1, RULE_Y) && pix(126, RULE_Y), "a dotted line under the name");
    for (ok = 1, y = WAVE_MID - WAVE_HALF; y <= WAVE_MID + WAVE_HALF; y++)
        ok &= pix(0, y);
    CHECK(ok, "the start in column 0, a solid line");
    for (any = 0, below = 0, x = 1; x < 127; x++) {
        any |= pix(x, WAVE_MID + WAVE_HALF) || pix(x, WAVE_MID - WAVE_HALF);
        below |= pix(x, WAVE_MID - WAVE_HALF - 1);
    }
    CHECK(any && !below, "drawn to its own peak, no further");
    for (ok = 1, x = 0; x < 128; x++)
        for (y = RULE_Y + 1; y < NAME_Y; y++)
            ok &= !pix(x, y);
    CHECK(ok, "nothing between the line and the name");
    preview("view-full");

    /* Start 30, Length 45 of 120: columns 32 to 79 play (75 / 120 * 128 = 80) */
    set_knob(s, 42, 30);
    set_knob(s, 44, 45);
    smp_main_draw(0, &screen);
    for (ok = 1, y = WAVE_MID - WAVE_HALF; y <= WAVE_MID + WAVE_HALF; y++)
        ok &= pix(32, y) && (y & 1 || pix(79, y));
    CHECK(ok, "a solid line at 32, a dotted one at 79");
    for (pairs = 0, y = 0; y < RULE_Y - 1; y++)
        pairs += pix(10, y) && pix(10, y + 1);
    CHECK(pairs == 0, "before the start every other pixel");
    for (pairs = 0, y = 0; y < RULE_Y - 1; y++)
        pairs += pix(50, y) && pix(50, y + 1);
    CHECK(pairs > 0, "the played part in full");
    preview("view-start-length");

    /* Start 60: the same 48 columns, 64 to 111 */
    set_knob(s, 42, 60);
    smp_main_draw(0, &screen);
    for (ok = 1, y = WAVE_MID - WAVE_HALF; y <= WAVE_MID + WAVE_HALF; y++)
        ok &= pix(64, y) && (y & 1 || pix(111, y));
    CHECK(ok, "moving Start keeps the length: a solid line at 64, a dotted one at 111");
    for (pairs = 0, y = 0; y < RULE_Y - 1; y++)
        pairs += pix(40, y) && pix(40, y + 1);
    CHECK(pairs == 0, "column 40 is before the start now");

    /* Start 120: the start at the very end, nothing plays */
    set_knob(s, 42, 120);
    smp_main_draw(0, &screen);
    for (ok = 1, y = WAVE_MID - WAVE_HALF; y <= WAVE_MID + WAVE_HALF; y++)
        ok &= pix(127, y);
    for (pairs = 0, x = 0; x < 127; x++)
        for (y = 0; y < RULE_Y - 1; y++)
            pairs += pix(x, y) && pix(x, y + 1);
    CHECK(ok && pairs == 0, "Start 120: the line at the last column, nothing in full");

    /* turning Start or Length shows the view, other knobs their value */
    memset(view, 0, sizeof view);
    host_value_shown = 1;
    for (p = DESC_START; p <= DESC_LENGTH; p++) {
        memcpy(view + 0x230, &p, 4);
        smp_knob_draw(view, &screen);
    }
    CHECK(host_knob_draws == 0, "Start and Length move the markers");
    p = 42;
    memcpy(view + 0x230, &p, 4);
    smp_knob_draw(view, &screen);
    CHECK(host_knob_draws == 1, "another knob shows its value");
    p = DESC_START;
    memcpy(view + 0x230, &p, 4);
    host_trig_held = 1;
    smp_knob_draw(view, &screen);
    host_trig_held = 0;
    CHECK(host_knob_draws == 2, "with a trig held, Start shows its locked value");
    smp_main_draw(0, &screen);
    CHECK(host_main_draws == 1, "while a value shows, the main screen is the stock one");

    /* tracks on other machines, and no track */
    host_value_shown = 0;
    host_track = 3;
    smp_main_draw(0, &screen);
    host_value_shown = 1;
    p = DESC_START;
    memcpy(view + 0x230, &p, 4);
    smp_knob_draw(view, &screen);
    host_track = -1;
    host_value_shown = 0;
    smp_main_draw(0, &screen);
    CHECK(host_main_draws == 3 && host_knob_draws == 3, "other machines keep the stock views");
}

static void test_view_states(void)
{
    char *s;

    reset_all();
    host_track = 1;
    s = host_sound(1);
    s[38] = SAMPLE_MACHINE;
    memcpy(s + 4, "INIT", 5);
    smp_main_draw(0, &screen);
    CHECK(ntexts == 1 && !strcmp(texts[0].s, "NO SAMPLE") && texts[0].font == FONT_MEDIUM && texts[0].x == 64 &&
          texts[0].flags == 2, "no sample yet, so no name either: %d texts, %s", ntexts, ntexts ? texts[0].s : "-");
    preview("view-no-sample");

    make_sound(s, 130, 0xd101);
    memcpy(s + 4, "GONE", 5);
    ntexts = 0;
    smp_main_draw(0, &screen);
    CHECK(ntexts == 2 && !strcmp(texts[1].s, "LOADING"), "named but not loaded: %s", ntexts > 1 ? texts[1].s : "-");
    tick_until(0xd101, SLOT_FAILED, 3);
    ntexts = 0;
    smp_main_draw(0, &screen);
    CHECK(ntexts == 2 && !strcmp(texts[1].s, "NOT FOUND"), "not on the drive: %s", ntexts > 1 ? texts[1].s : "-");
    preview("view-not-found");
}

int main(void)
{
    test_browser();
    test_load_mono();
    test_stereo_rate();
    test_moved();
    test_missing();
    test_retry();
    test_trig();
    test_play_kit();
    test_retire();
    test_nomem();
    test_records();
    test_live();
    test_lock();
    test_drive_guard();
    test_rate_pitch();
    test_start_length();
    test_overview();
    test_view();
    test_view_states();
    test_machine_change();
    drop_files();
    CHECK(nested == 0, "file system calls under the lock, or the index without it: %d", nested);
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all bridge, pool and view tests passed\n");
    return 0;
}
