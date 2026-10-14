/* What SAMPLE uses of the Model:Cycles OS 1.13. Each address was read in the OS's code; host tests
   (machine/test_sampler.c) replace this file with machine/host_os.h. */
#ifndef OS_H
#define OS_H

#include <stdint.h>

#ifdef SAMPLER_HOST
#include "host_os.h"
#else

/* --- voices and the audio side --------------------------------------------------------------- */
#define VOICE0      ((uintptr_t)0x42308828)     /* voice of track 0; one per track, 0x31c apart */
#define AMP_ENV(v)          ((void (*)(char *))0x400a9252)(v)
#define VCA(o, v)           ((void (*)(int32_t *, char *))0x400a9430)(o, v)
#define PUNCH(o, v)         ((void (*)(int32_t *, char *))0x400a967a)(o, v)
#define DECIMATE(v, in, o)  ((void (*)(char *, int32_t *, int32_t *))0x400a9f58)(v, in, o)
#define LUT_DECAY           ((const int32_t *)0x4011d84c)       /* DECAY -> factor per block (TONE's) */
#define DECIMATOR_STATE(v)  (*(int32_t **)((v) + 0x318))        /* its 8 + 4 frames for this voice */
/* The 66 bytes of parameters (k 0..32) of the sound a track plays, as the audio side copies them
   when a note's sound changes (0x40058a0a), before any smoothing. */
#define TARGETS(t)          ((const char *)0x800015ae + 66 * (t))
/* The sound whose parameters a track holds (0x800015a0 + (147 + t) * 4); 0 makes the next note
   copy its sound again (the OS does that itself after a preview note, 0x4005910e). */
#define CUR_SND(t)          (((void *volatile *)0x800017ec)[t])

/* --- sounds and kits --------------------------------------------------------------------------- */
/* A track's live sound: owner = the project singleton + 212 + t * 68, then the function at +40 of
   its vtable (the firmware's own derivation at 0x40023f0a; Model-TG and Modded-Cycles use the same
   walk). */
static inline char *os_sound(int t)
{
    char *owner = ((char *(*)(void))0x400cf866)();
    char *track;
    void **vt;

    if (!owner)
        return 0;
    track = owner + 212 + t * 68;
    vt = *(void ***)track;
    return vt && vt[10] ? ((char *(*)(char *))vt[10])(track) : 0;
}
/* The kits of the 96 patterns: 6 track sounds at +28, then the pool of 64 sounds at +694. */
#define KIT(n)              ((char *)0x409c9c80 + 7094 * (n))
#define KITS                96
#define KIT_SOUNDS          28
#define KIT_POOL            694
#define POOL_SOUNDS         64
/* The kit the audio side plays (0x800017e4, set by its kit message at 0x40058ddc), apart from the 96:
   the working copy of the current pattern's kit. Its pool holds the sounds that sound locks play. */
#define PLAY_KIT            (*(char *volatile *)0x800017e4)

/* --- the file system on the +Drive ------------------------------------------------------------ */
/* Set once the file system is mounted (0x40f07cf4; its calls check it). */
#define FS_MOUNTED          (*(volatile uint32_t *)0x40f07cf4)
/* Its lock (0x40fe6df0, taken by 0x40001cc4, given back by 0x40001df6): +0 the task that holds it;
   0x422fef30 is the running task. The lock lets its holder in again but does not count, so an inner
   give-back would free it while the outer work goes on. */
#define FS_LOCK_HOLDER      (*(void *volatile *)0x40fe6df0)
#define RUNNING_TASK        (*(void *volatile *)0x422fef30)
/* Fills {inode, id, size, inode word 16} from an inode number, under the file system's lock;
   0 if it is a file, -1 otherwise (0x4007de78). */
#define FS_ENTRY(ino, h)    ((int32_t (*)(uint32_t, uint32_t *))0x4007de78)(ino, h)
/* Reads len bytes at the handle's position {inode, position, -, open != 0} into buf and moves the
   position on; follows the file's chunk map through the file system's cache, under its lock.
   Returns the bytes read, or -1 (0x4007baaa). */
#define FS_READ(buf, len, fh)  ((int32_t (*)(void *, uint32_t, uint32_t *))0x4007baaa)(buf, len, fh)
/* Its index of files by id (the id is a hash of the file's content, bit 0 set; inode word 12): pairs
   {id, inode} at 0x401fb8a4, sorted by id, built at mount from the drive's table of ids (0x4007e042)
   and built again whenever a file is finished. FS_FIND_ID gives the pair's number, or -1
   (0x4007dc30, a binary search); call it under the lock. The OS's own resolver of file references
   (0x4007e3a0) looks there for a file that left its inode. */
#define FS_LOCK()           ((void (*)(void *))0x40001cc4)((void *)0x40fe6df0)
#define FS_UNLOCK()         ((void (*)(void *))0x40001df6)((void *)0x40fe6df0)
#define FS_FIND_ID(id)      ((int32_t (*)(uint32_t))0x4007dc30)(id)
#define FS_ID_INO(i)        (((const volatile uint32_t *)0x401fb8a8)[2 * (i)])

#endif
#endif
