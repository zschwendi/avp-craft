#ifndef MC_AUDIO_OPENAL_INTERNAL_H
#define MC_AUDIO_OPENAL_INTERNAL_H
#include "MCAudioOpenAL.h"
/* Exact OpenAL C API widths. Opaque handles are not dereferenced here. */
typedef struct {
    void *(*current)(void);
    void *(*thread_context)(void);
    void *(*device)(void *context);
    int (*error)(void *device);
    void (*integers)(void *device,int name,int count,int *values);
    void (*pause)(void *device);
    void (*resume)(void *device);
    char (*reset)(void *device,const int *attributes);
    void *(*procedure)(void *device,const char *name);
} MCAudioOpenALAPI;
/* Same production algorithm with an injected API table for CPU fixtures. */
int mc_openal_audio_with_api(const MCAudioOpenALAPI *api, int action);
#endif
