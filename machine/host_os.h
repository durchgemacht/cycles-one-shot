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

#endif
