/* SAMPLE's view on the main screen: the selected track's sample as a waveform across the whole
   screen, the start and end of what a trig plays marked on it, and above it a bar with the sound's
   name (the browser names the sound after the sample it opens).

   It takes the place of MainScreenView while that shows the track, and of the knob view while that
   shows Start or Length (COLOR or SHAPE), so the markers follow those two knobs as they turn. Any
   other knob shows its value as before, and so do Start and Length while a trig is held: the knob
   view then shows the locked value. Tracks on other machines keep the stock views. */
#include "view.h"
#include "pool.h"
#include "os.h"

#ifndef DESC_START
#define DESC_START  76                  /* the knob view's index of SAMPLE's Start; the build sets it */
#endif
#define DESC_LENGTH (DESC_START + 1)
#ifndef CUTOFF_OBJECT
#define CUTOFF_OBJECT 53                /* the parameter object Cutoff shares: SNARE's SWEEP; the build sets it */
#endif

#define SOUND_NAME  4                   /* sound: name, 16 bytes */
#define SOUND_COLOR 42                  /* sound: k 11, COLOR = Start, 8.8 */
#define SOUND_SHAPE 44                  /* sound: k 12, SHAPE = Length, 8.8 */
#define MACHINE_AT  38

/* The screen, y counted from the bottom (a set pixel is lit): the title bar of the OS's own browser
   (0x400a539c: its title in the small font at 2, 53, then 0x40070efc turns rows 51..63 over), so a
   lit bar of 128 x 13 pixels along the top, its corners rounded off, the name in dark letters on
   it; the waveform below. The OS's text is 9 rows high from the y it is given, capitals included,
   so the name has 2 rows of bar above and below. */
#define BAR_X       0
#define BAR_W       128
#define BAR_TOP     63                  /* the bar's highest row, the screen's top */
#define BAR_Y       (BAR_TOP - 12)      /* its lowest, 13 rows down */
#define NAME_X      (BAR_X + 2)
#define NAME_Y      (BAR_Y + 2)         /* the name's text, small font */
#define WAVE_MID    25                  /* the waveform's zero line */
#define WAVE_HALF   24                  /* rows above and below it: 1..49, a free row under the bar */
#define TEXT_Y      (WAVE_MID - 4)      /* a message instead of the waveform, medium font, centred */

static void pixel(smp_bitmap *b, int x, int y)
{
    if ((unsigned)x < (unsigned)b->width && (unsigned)y < (unsigned)b->height)
        b->words[x * b->pitch + (y >> 5)] |= 0x80000000u >> (y & 31);
}

static void flip(smp_bitmap *b, int x, int y)
{
    if ((unsigned)x < (unsigned)b->width && (unsigned)y < (unsigned)b->height)
        b->words[x * b->pitch + (y >> 5)] ^= 0x80000000u >> (y & 31);
}

static void blank(smp_bitmap *b)
{
    int32_t i, n = b->width * b->pitch;

    for (i = 0; i < n; i++)
        b->words[i] = 0;
}

static void label(smp_bitmap *b, const void *glyphs, int x, int y, int flags, const char *s)
{
    uint32_t font[2];

    FONT_MAKE(font, glyphs);
    DRAW_TEXT(b, font, x, y, flags, "%s", s);
    FONT_DROP(font);
}

/* The bar: the name drawn lit on the blank screen, then every pixel of the bar's rows turned over,
   so the bar is lit and the letters dark, then its four corner pixels dark again. */
static void bar(smp_bitmap *b, const char *name)
{
    int x, y, right = BAR_X + BAR_W - 1;

    if (name)
        label(b, FONT_SMALL, NAME_X, NAME_Y, 0, name);
    for (y = BAR_Y; y <= BAR_TOP; y++)
        for (x = BAR_X; x <= right; x++)
            flip(b, x, y);
    flip(b, BAR_X, BAR_Y);
    flip(b, right, BAR_Y);
    flip(b, BAR_X, BAR_TOP);
    flip(b, right, BAR_TOP);
}

static int32_t sound_span(const char *sound, int at)  /* 8.8 word -> 0..SMP_SPAN, fraction kept */
{
    int32_t k = *(const int16_t *)(sound + at);

    return k < 0 ? 0 : k > SMP_SPAN ? SMP_SPAN : k;
}

/* Outside what a trig plays, a quarter of the pixels: every other row counted from the zero line,
   every other column in those rows, one column further on from one such row to the next. No two
   dots touch, each column keeps dots, and a silent stretch still shows as a dotted zero line. */
static int quarter(int32_t c, int32_t y)
{
    int32_t dy = y - WAVE_MID;

    return !(dy % 2) && !((c + dy / 2) % 2);
}

/* The waveform scaled to its own peak: the part a trig plays in full, the rest in a quarter of
   its pixels; a solid line at the start, a dotted one at the last column played. Columns follow the
   engine's rule (SMP_SPAN in pool.h): the start at Start / 120 of the sample, the end at
   (Start + Length) / 120, at most the sample's end. */
static void waveform(smp_bitmap *b, const int16_t *lo, const int16_t *hi, int32_t start, int32_t length)
{
    int32_t peak = 1, c, y, s, e, end = start + length > SMP_SPAN ? SMP_SPAN : start + length;

    for (c = 0; c < SMP_COLUMNS; c++) {
        if (lo[c] > hi[c])
            continue;
        if (-lo[c] > peak)
            peak = -lo[c];
        if (hi[c] > peak)
            peak = hi[c];
    }
    s = start * SMP_COLUMNS / SMP_SPAN;                 /* the column of the first frame played */
    e = (end * SMP_COLUMNS + SMP_SPAN - 1) / SMP_SPAN;  /* past the column of the last one */
    for (c = 0; c < SMP_COLUMNS; c++) {
        int32_t top, bottom, in = c >= s && c < e;

        if (lo[c] > hi[c])
            continue;
        top = WAVE_MID + hi[c] * WAVE_HALF / peak;
        bottom = WAVE_MID + lo[c] * WAVE_HALF / peak;
        for (y = bottom; y <= top; y++)
            if (in || quarter(c, y))
                pixel(b, c, y);
    }
    for (y = WAVE_MID - WAVE_HALF; y <= WAVE_MID + WAVE_HALF; y++) {
        pixel(b, s < SMP_COLUMNS ? s : SMP_COLUMNS - 1, y);   /* Start 120: at the very end */
        if (!(y & 1) && e > s)
            pixel(b, e - 1, y);
    }
}

void smp_view_draw(smp_bitmap *b, const char *sound)
{
    const int16_t *lo = 0, *hi = 0;
    uint32_t id = smp_sound_id(sound);
    char name[17];
    int i, state;

    blank(b);
    for (i = 0; i < 16 && sound[SOUND_NAME + i]; i++)
        name[i] = sound[SOUND_NAME + i];
    name[i] = 0;
    bar(b, id ? name : 0);              /* without a sample, the name is still the sound's old one */
    state = smp_view_state(id, &lo, &hi);
    if (!id)
        label(b, FONT_MEDIUM, 64, TEXT_Y, 2, "NO SAMPLE");
    else if (state == SLOT_READY)
        waveform(b, lo, hi, sound_span(sound, SOUND_COLOR), sound_span(sound, SOUND_SHAPE));
    else if (state == SLOT_FAILED)
        label(b, FONT_MEDIUM, 64, TEXT_Y, 2, "NOT FOUND");
    else
        label(b, FONT_MEDIUM, 64, TEXT_Y, 2, "LOADING");
}

/* The selected track's live sound, if it plays SAMPLE. */
static const char *sample_sound(void)
{
    int32_t t = selected_track();
    const char *s;

    if (t < 0 || t > 5)
        return 0;
    s = os_sound(t);
    return s && (unsigned char)s[MACHINE_AT] == SAMPLE_MACHINE ? s : 0;
}

void smp_main_draw(void *view, smp_bitmap *b)
{
    const char *s = sample_sound();

    if (s && !VALUE_SHOWN())
        smp_view_draw(b, s);
    else
        MAIN_DRAW(view, b);
}

void smp_knob_draw(void *view, smp_bitmap *b)
{
    const char *s = sample_sound();
    int32_t p = KNOB_PARAM(view);

    if (s && VALUE_SHOWN() && !TRIG_HELD() && (p == DESC_START || p == DESC_LENGTH))
        smp_view_draw(b, s);
    else
        KNOB_DRAW(view, b);
}

/* --- Cutoff's value ------------------------------------------------------------------------- */

/* Cutoff shows -64..63 as on the Model:Samples: the lowpass side below zero, 0 off, the highpass
   side above. SAMPLE's descriptors share SNARE's parameter objects, so the formatter asks whether
   the selected track plays SAMPLE and otherwise writes the stock 0..127. value is the 8.8 word. */
int32_t smp_fmt_cutoff(void *self, int32_t value, char *buf)
{
    int32_t v = (value >> 8) - 64, n = 0;
    char digits[4];
    int i = 0;

    if (!sample_sound())
        return FMT_INT(self, value, buf);
    if (v < 0) {
        buf[n++] = '-';
        v = -v;
    }
    do {
        digits[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (i)
        buf[n++] = digits[--i];
    buf[n] = 0;
    return n;
}

/* Puts smp_fmt_cutoff into the shared object, once the OS has built it with the stock formatter
   in place (anything else there is left alone). The tick calls this; it is done after the first
   screen redraw. */
void smp_view_install(void)
{
    static int done;

    if (done)
        return;
    if (UI_FORMATTER(CUTOFF_OBJECT) == FMT_INT) {
        UI_FORMATTER(CUTOFF_OBJECT) = smp_fmt_cutoff;
        done = 1;
    }
}
