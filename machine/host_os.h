/* For host tests only (machine/test_sampler.c): what SAMPLE takes from the OS (os.h), as stand-ins. */
#ifndef HOST_OS_H
#define HOST_OS_H

#include <stdint.h>

extern char host_voices[];
extern const int32_t host_lut_decay[128];
extern char host_targets[];
extern void *host_cur_snd[6];
extern char host_kits[];
extern char *host_play_kit;
extern char host_pool[];
extern unsigned long host_pool_size;
extern uint32_t host_fs_mounted;
extern void *host_fs_holder, *host_task;
extern int host_value_shown, host_trig_held, host_track, host_pattern, host_main_draws, host_knob_draws;
extern const void *host_font;

void host_amp_env(char *v);
void host_vca(int32_t *out, char *v);
void host_punch(int32_t *out, char *v);
void host_decimate(char *v, int32_t *in, int32_t *out);
int32_t *host_state(char *v);
char *host_sound(int t);
int32_t host_fs_entry(uint32_t ino, uint32_t *h);
int32_t host_fs_read(void *buf, uint32_t len, uint32_t *fh);
void host_fs_lock(void);
void host_fs_unlock(void);
int32_t host_find_id(uint32_t id);
uint32_t host_id_ino(int32_t i);
void host_draw_text(void *bmp, void *font, int32_t x, int32_t y, int32_t flags, const char *fmt, ...);

#define VOICE0              ((uintptr_t)host_voices)
#define AMP_ENV(v)          host_amp_env(v)
#define VCA(o, v)           host_vca(o, v)
#define PUNCH(o, v)         host_punch(o, v)
#define DECIMATE(v, in, o)  host_decimate(v, in, o)
#define LUT_DECAY           host_lut_decay
#define DECIMATOR_STATE(v)  host_state(v)
#define TARGETS(t)          ((const char *)host_targets + 66 * (t))
#define CUR_SND(t)          (host_cur_snd[t])
#define os_sound(t)         host_sound(t)
#define KIT(n)              (host_kits + 7094 * (n))
#define KITS                96
#define KIT_SOUNDS          28
#define KIT_POOL            694
#define POOL_SOUNDS         64
#define PLAY_KIT            host_play_kit
#define FS_ENTRY(ino, h)    host_fs_entry(ino, h)
#define FS_READ(b, n, fh)   host_fs_read(b, n, fh)
#define FS_LOCK()           host_fs_lock()
#define FS_UNLOCK()         host_fs_unlock()
#define FS_FIND_ID(id)      host_find_id(id)
#define FS_ID_INO(i)        host_id_ino(i)
#define FS_MOUNTED          host_fs_mounted
#define FS_LOCK_HOLDER      host_fs_holder
#define RUNNING_TASK        host_task
#define POOL_BEGIN          (host_pool)
#define POOL_END            (host_pool + host_pool_size)
#define MAIN_DRAW(v, b)     ((void)(v), (void)(b), host_main_draws++)
#define KNOB_DRAW(v, b)     ((void)(v), (void)(b), host_knob_draws++)
#define KNOB_PARAM(v)       (*(const int32_t *)((const char *)(v) + 0x230))
#define VALUE_SHOWN()       (host_value_shown != 0)
#define TRIG_HELD()         (host_trig_held != 0)
#define selected_track()    (host_track)
#define current_pattern()   (host_pattern)
#define FONT_MAKE(f, g)     ((void)(f), host_font = (g))
#define FONT_DROP(f)        ((void)(f))
extern const char host_font_small[], host_font_medium[];
typedef int32_t (*smp_formatter)(void *, int32_t, char *);
extern char host_ui_objects[];
int32_t host_fmt_int(void *self, int32_t value, char *buf);
#define UI_OBJECT(i)        (host_ui_objects + 100 * (i))
#define UI_FORMATTER(i)     (*(smp_formatter *)(UI_OBJECT(i) + 0x20))
#define FMT_INT             host_fmt_int
#define FONT_SMALL          ((const void *)host_font_small)
#define FONT_MEDIUM         ((const void *)host_font_medium)
#define DRAW_TEXT           host_draw_text

#endif
