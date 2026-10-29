#ifndef MC_AUDIO_OPENAL_H
#define MC_AUDIO_OPENAL_H
#ifdef __cplusplus
extern "C" {
#endif
enum { MC_AUDIO_PAUSE=0, MC_AUDIO_RESET=1, MC_AUDIO_REOPEN=2 };
enum {
    MC_AUDIO_NOT_LOADED=0, MC_AUDIO_NO_CONTEXT=1, MC_AUDIO_PAUSED=2,
    MC_AUDIO_RECOVERED=3, MC_AUDIO_REOPENED=4,
    MC_AUDIO_MISSING_API=-1, MC_AUDIO_UNEXPECTED_TLS=-2,
    MC_AUDIO_BAD_ATTRIBUTES=-3, MC_AUDIO_RECOVERY_FAILED=-4,
    MC_AUDIO_BAD_ACTION=-5
};
/* Operates only on Minecraft's process-global context, never all devices.
 * exact_library_path must name the already-loaded signed Frameworks dylib.
 * No OpenDevice/CreateContext/MakeContextCurrent or guest pointer caching. */
int mc_openal_audio_control(const char *exact_library_path, int action);
#ifdef __cplusplus
}
#endif
#endif
