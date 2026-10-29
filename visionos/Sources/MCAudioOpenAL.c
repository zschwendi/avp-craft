/* SPDX-License-Identifier: MIT
 * Host lifecycle recovery using public OpenAL Soft APIs, no engine replacement.
 * Matching source: OpenAL Soft1.25.2 alc/alc.cpp2916,3515,3536,3615,3651.
 */
#include "MCAudioOpenALInternal.h"
#include <dlfcn.h>
#include <string.h>

enum { ATTRIBUTES_SIZE=0x1002, ALL_ATTRIBUTES=0x1003, CONNECTED=0x313 };
typedef char (*ReopenDevice)(void *,const char *,const int *);

int mc_openal_audio_with_api(const MCAudioOpenALAPI *api,int action) {
    if(action<MC_AUDIO_PAUSE || action>MC_AUDIO_REOPEN) return MC_AUDIO_BAD_ACTION;
    if(!api || !api->current || !api->thread_context || !api->device || !api->error
       || !api->integers || !api->pause || !api->resume || !api->reset || !api->procedure)
        return MC_AUDIO_MISSING_API;
    /* The actual26.2 Library uses alcMakeContextCurrent. This control thread
     * must have no private context; never steal Java/voicechat thread state. */
    if(api->thread_context()!=0) return MC_AUDIO_UNEXPECTED_TLS;
    void *context=api->current();
    if(!context) return MC_AUDIO_NO_CONTEXT;
    /* Both device(context) and subsequent device APIs validate and retain
     * their handle internally. A concurrent game shutdown is a normal failure,
     * not a reason to cache or dereference one of these borrowed pointers. */
    void *device=api->device(context);
    if(!device) return MC_AUDIO_NO_CONTEXT;
    (void)api->error(device); /* isolate this recovery's ALC status */
    if(action==MC_AUDIO_PAUSE) {
        api->pause(device);
        return api->error(device)==0 ? MC_AUDIO_PAUSED : MC_AUDIO_RECOVERY_FAILED;
    }
    int count=0,attributes[129]={0};
    api->integers(device,ATTRIBUTES_SIZE,1,&count);
    if(api->error(device)!=0 || count<1 || count>129 || count%2!=1)
        return MC_AUDIO_BAD_ATTRIBUTES;
    api->integers(device,ALL_ATTRIBUTES,count,attributes);
    if(api->error(device)!=0 || attributes[count-1]!=0) return MC_AUDIO_BAD_ATTRIBUTES;
    for(int i=0;i<count-1;i+=2) if(attributes[i]==0) return MC_AUDIO_BAD_ATTRIBUTES;
    /* Resume alone is a no-op unless DevicePaused is set. The OS can stop
     * CoreAudio without setting that flag, so always establish the pair. */
    api->pause(device);
    if(api->error(device)!=0) return MC_AUDIO_RECOVERY_FAILED;
    int reopened=0;
    char ok=0;
    if(action==MC_AUDIO_RESET) {
        ok=api->reset(device,attributes);
        if(api->error(device)!=0) ok=0;
    }
    if(action==MC_AUDIO_REOPEN || !ok) {
        ReopenDevice reopen=0;
        void *symbol=api->procedure(device,"alcReopenDeviceSOFT");
        _Static_assert(sizeof(reopen)==sizeof(symbol),"Darwin function pointer width");
        memcpy(&reopen,&symbol,sizeof(reopen));
        if(reopen) { ok=reopen(device,0,attributes); reopened=ok!=0; }
    }
    /* Reopen may return true even if its internal reset disconnected. Check
     * connected after resuming; never claim this return proves audible PCM. */
    api->resume(device);
    int connected=0;
    api->integers(device,CONNECTED,1,&connected);
    if(!ok || !connected || api->error(device)!=0) return MC_AUDIO_RECOVERY_FAILED;
    return reopened ? MC_AUDIO_REOPENED : MC_AUDIO_RECOVERED;
}

int mc_openal_audio_control(const char *path,int action) {
    if(!path || !*path) return MC_AUDIO_NOT_LOADED;
    /* NOLOAD is essential: starting a second engine, or loading before Java
     * owns one, would conceal rather than recover the missing device. */
    void *library=dlopen(path,RTLD_NOW|RTLD_LOCAL|RTLD_NOLOAD);
    if(!library) return MC_AUDIO_NOT_LOADED;
    MCAudioOpenALAPI api={0};
#define LOAD(member,name) do { void *s=dlsym(library,name); \
    _Static_assert(sizeof(api.member)==sizeof(s),"Darwin function pointer width"); \
    memcpy(&api.member,&s,sizeof(s)); } while(0)
    LOAD(current,"alcGetCurrentContext");
    LOAD(thread_context,"alcGetThreadContext");
    LOAD(device,"alcGetContextsDevice");
    LOAD(error,"alcGetError");
    LOAD(integers,"alcGetIntegerv");
    LOAD(pause,"alcDevicePauseSOFT");
    LOAD(resume,"alcDeviceResumeSOFT");
    LOAD(reset,"alcResetDeviceSOFT");
    LOAD(procedure,"alcGetProcAddress");
#undef LOAD
    int result=mc_openal_audio_with_api(&api,action);
    dlclose(library);
    return result;
}
