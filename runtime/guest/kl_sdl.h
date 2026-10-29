// The SDL2 door, as a thing two drivers can both open.
//
// Same rationale as kl_ue4.h (which is the same rationale as kl_slink.h): the
// host driver (`build/m_boot`) and the visionOS app both start this guest, and
// a guest described differently by two drivers is a bug with no error surface.
// So the chain order, the environment the guest reads, and the SDLActivity
// boot sequence live here; the policy — the host's recon budget and gap-only
// mode, the app's lifecycle and frame pacing — stays with the driver.
//
// The first guest on this door is Half-Life 2 VR (nillerusr's srceng Quest
// port), but the kind names the DOOR, not the engine (kl_target.h's
// KL_GUEST_SDL): org.libsdl.app.SDLActivity's own convention, read off the
// APK's smali — getLibraries() = {"SDL2", "launcher"}, getMainFunction() =
// "LauncherMainAndroid". System.loadLibrary each (so libSDL2's JNI_OnLoad
// registers the SDLActivity natives), then nativeRunMain(library, function,
// args), which inside libSDL2 is a plain dlopen + dlsym + call of the entry
// library's main, the way SDL2 starts any SDL_main. Any second SDLActivity
// guest takes this door regardless of its engine.
//
// Two things do NOT come through that call, and they are half of what this
// file exists to hold in one place:
//
//   the environment. ValveActivity2.initNatives (the game's one Java class)
//   pokes VALVE_GAME_PATH / APP_DATA_PATH / APP_LIB_PATH / EXTRAS_VPK_PATH /
//   LANG through liblauncher's own exports BEFORE loadLibrary. Those natives
//   are ordinary setenv in this process, so kl_sdl_configure() is plain
//   setenv before the libraries load — the engine's static initializers are
//   entitled to read it, and on Android it was set before loadLibrary too.
//
//   the chain ORDER. srceng links thirty plain C++ libraries whose imports
//   bind at relocation time, and this door has two scars to prove order is
//   load-bearing: libfilesystem_stdio mapped before libtier0 keeps
//   g_pMemAlloc unresolved forever (SIGSEGV in CreateInterface), and libtier0
//   registered before libc++_shared wins the weak __cxxabiv1 typeinfo
//   vtables, which breaks dynamic_cast across the chain (SIGSEGV at
//   campaign-map load). So the load is libc++_shared FIRST, then
//   dependencies-first off each library's own DT_NEEDED.
#ifndef KL_SDL_H
#define KL_SDL_H

#include <stdio.h>

// Point the runtime at this guest and set the environment the guest reads.
// Paths come from the target row (kl_target_apply_host on the host,
// kl_app_configure in the app, both of which have already run); what this
// adds is the part that is the door's rather than the tree's — the activity
// class the guest asks for by name, and the ValveActivity2 env half above.
//
// `out` takes the one line that says where the game root maps, or NULL.
// Returns 0, or non-zero with kl_sdl_error() set.
int kl_sdl_configure(const char *libdir, FILE *out);

// Why the last call that returned non-zero did. Never NULL.
const char *kl_sdl_error(void);

// Map the chain: every .so in the libdir (a directory scan, not a table —
// the library set is a property of the APK), libc++_shared first, then
// Kahn over DT_NEEDED. One exception, by name: libopenxr_loader.so, which is
// REPLACED (kl_openxr_dlopen claims the dlopen), the same call §3.1 made for
// libOVRPlugin. Returns 0 on success.
int kl_sdl_load(FILE *out);

// What the chain still has unbound — kl_ue4_gap's property, over every image
// this door mapped rather than only the entry library. Returns the number of
// libraries with a non-empty list.
unsigned kl_sdl_gap(FILE *out);

// Initializers (dependencies-first, the same order the mapping used), then
// the door's own entry sequence: libSDL2's JNI_OnLoad, the three
// nativeSetupJNI natives with the jclass as the LOAD-BEARING second argument
// (SDL caches it and resolves every static callback against it), and
// SDL_SetMainReady. Finally the args half of ValveActivity2.initNatives:
// setArgs("-game hl2 -vr -language ...") through the launcher's own native.
int kl_sdl_begin(FILE *out);

// SDL's nativeRunMain itself — the REGISTERED native, not a dlsym of the
// entry function: a bare LauncherMainAndroid call skips the setup and dies
// in SDL_Init. Passed exactly what SDLActivity.java passes:
// (getMainSharedObject(), getMainFunction(), getArguments()).
//
// NEVER RETURNS on a healthy run: an SDL guest owns its frame loop inside
// this call, the way a NativeActivity guest owns its. The caller decides
// what a run is — the host arms a time budget around it, the app calls it
// on the guest thread — and the return value is only ever the guest's own
// exit path. Runs on the CALLING thread, which must be one kl_thread_init
// has touched (trap 1).
int kl_sdl_run_main(FILE *out);

// The subsystem reports this door's runs print, in one place.
void kl_sdl_report(FILE *out);

#endif
