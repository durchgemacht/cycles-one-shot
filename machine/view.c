/* SAMPLE's view on the main screen: the selected track's sample as a waveform across the whole
   screen, the start and end of what a trig plays marked on it, the sound's name above (the
   browser names the sound after the sample it opens).

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

#define SOUND_NAME  4                   /* sound: name, 16 bytes */
#define SOUND_COLOR 42                  /* sound: k 11, COLOR = Start, 8.8 */
#define SOUND_SHAPE 44                  /* sound: k 12, SHAPE = Length, 8.8 */
#define MACHINE_AT  38

/* The screen, y counted from the bottom: the name in the top rows, a dotted line under it, the
   waveform below. */
#define NAME_Y      54                  /* the name's text, small font */
#define RULE_Y      52                  /* the dotted line */
#define WAVE_MID    26                  /* the waveform's zero line */
#define WAVE_HALF   25                  /* rows above and below it */
#define TEXT_Y      21                  /* a message instead of the waveform, medium font */

static void pixel(smp_bitmap *b, int x, int y)
{
    if ((unsigned)x < (unsigned)b->width && (unsigned)y < (unsigned)b->height)
        b->words[x * b->pitch + (y >> 5)] |= 0x80000000u >> (y & 31);
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

static int32_t sound_span(const char *sound, int at)  /* 8.8 word -> 0..SMP_SPAN, fraction kept */
{
    int32_t k = *(const int16_t *)(sound + at);

    return k < 0 ? 0 : k > SMP_SPAN ? SMP_SPAN : k;
}

/* The waveform scaled to its own peak: the part a trig plays in full, the rest in every other
   pixel; a solid line at the start, a dotted one at the last column played. Columns follow the
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
            if (in || !((c + y) & 1))
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
    if (id) {                           /* without a sample, the name is still the sound's old one */
        for (i = 0; i < 16 && sound[SOUND_NAME + i]; i++)
            name[i] = sound[SOUND_NAME + i];
        name[i] = 0;
        label(b, FONT_SMALL, 1, NAME_Y, 0, name);
    }
    for (i = 0; i < b->width; i += 2)
        pixel(b, i, RULE_Y);
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
