/* For host tests only (machine/test_sampler.c): what the bridge takes from the OS, as stand-ins. */
#ifndef HOST_OS_H
#define HOST_OS_H

#include <stdint.h>

extern char host_voices[];
extern const int32_t host_lut_decay[128];

void host_amp_env(char *v);
void host_vca(int32_t *out, char *v);
void host_punch(int32_t *out, char *v);
void host_decimate(char *v, int32_t *in, int32_t *out);
int32_t *host_state(char *v);

#define VOICE0              ((uintptr_t)host_voices)
#define AMP_ENV(v)          host_amp_env(v)
#define VCA(o, v)           host_vca(o, v)
#define PUNCH(o, v)         host_punch(o, v)
#define DECIMATE(v, in, o)  host_decimate(v, in, o)
#define LUT_DECAY           host_lut_decay
#define DECIMATOR_STATE(v)  host_state(v)

#endif
