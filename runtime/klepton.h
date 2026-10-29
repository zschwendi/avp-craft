// Klepton runtime — load an Android ARM64 ELF .so into a Darwin arm64 process.
//
// Images are mmap'd and self-relocated at runtime, on macOS and on
// development-signed visionOS builds. klepton-ld's Mach-O emitter reuses this
// exact relocation logic offline.
#ifndef KLEPTON_H
#define KLEPTON_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "kl_env.h"

typedef struct kl_image kl_image;

// Load and fully relocate an ELF .so. Returns NULL on error; kl_error() explains.
kl_image *kl_load(const char *path);
// Load a klepton-ld-translated Mach-O dylib instead. dyld maps the guest
// text — nothing is RWX and no guest byte is written at runtime, because the
// TLS and x18 rewrites happened offline. This is the path that has to work on
// visionOS; kl_load() above is the development one. Same kl_image afterwards.
kl_image *kl_load_dylib(const char *path);
// Pick between the two: with KL_DYLIB_DIR set, prefer <dir>/<name>.dylib when a
// translation exists, else the ELF. Lets the boot chain run mixed while some
// libraries are still untranslatable. Every guest-library load goes through it.
kl_image *kl_load_auto(const char *path);
// A GLIBC guest's translated dylib (the Alyx payload, linked with no host): the
// same load, with imports bound in glibc's order — the glibc personality first,
// then interposes, the other glibc images, the kept bionic ones, and a replaced
// soname's runtime module. See sym_value_glibc in kl_image.c.
kl_image *kl_load_dylib_glibc(const char *path);
// Map a translation and read its symbol table WITHOUT relocating anything: a
// replaced library's real file (libopenvr_api, libsteam_api, libSDL2), kept so
// its export list can name what belongs to it. Never register it, never call
// into it — its GOT is unrelocated.
kl_image *kl_map_dylib_unbound(const char *path);
// ...and the other half: relocate an image it mapped (glibc mode). 0, or -1
// with kl_error(). Mapping a closure first and binding it second is glibc's own
// order — it is what lets a NEEDED cycle bind both ways.
int         kl_bind_mapped(kl_image *img);
// The file an undefined import's symbol version names (`x@GLIBC_2.34` ->
// "libc.so.6"); NULL when the import is unversioned.
const char *kl_import_version_file(const kl_image *img, const char *name);
int         kl_image_is_glibc(const kl_image *img);
const char *kl_image_soname(const kl_image *img);           // DT_SONAME or NULL
const char *const *kl_image_needed(const kl_image *img, unsigned *count);
// The TP offset of the image's static TLS block (R_AARCH64_TLS_TPREL64), 0 if none.
int64_t     kl_image_static_tls(const kl_image *img);
// Every import a glibc image binds, as it binds (one call per relocation of an
// UNDEFINED symbol): `provider` is "glibc", "interpose", the providing image's
// soname, a replaced soname's module (kl_glibc_claim_sym), or NULL when
// unresolved. For the native door's
// census; NULL (the default) costs nothing.
typedef void (*kl_bind_observer)(kl_image *img, const char *name, const char *provider,
                                 uint64_t value, int weak);
void kl_set_bind_observer(kl_bind_observer fn);
// The lookup scope for a glibc image's name (both its imports and, through
// symbol preemption, its own default-visibility definitions): the first
// definition, and the providing image's label in `who`. Unset, every
// registered glibc image in load order; the native Alyx door installs ld.so's
// scopes (global, then the image's dlopen group).
typedef void *(*kl_glibc_scope_fn)(kl_image *img, const char *name, const char **who);
void kl_set_glibc_scope(kl_glibc_scope_fn fn);
// ...and the last word on a STRONG import nothing answered: a named stand-in
// (or NULL for the ordinary unresolved stub that aborts by name).
typedef void *(*kl_glibc_unresolved_fn)(kl_image *img, const char *name, const char **who);
void kl_set_glibc_unresolved(kl_glibc_unresolved_fn fn);
// DT_INIT and DT_INIT_ARRAY, for a loader that runs them itself (glibc order,
// with argc/argv/envp). Either may be NULL/0.
void kl_image_initializers(const kl_image *img, void (**init)(void),
                           void (***array)(void), size_t *count);
int  kl_image_symbolic(const kl_image *img);      // DT_SYMBOLIC / DF_SYMBOLIC
// The registered interposer for `name`, or NULL.
void *kl_interpose_lookup(const char *name);

// Could kl_load_auto load a guest library at `path`? Ask this — never
// stat(path) — whenever answering an existence question about a guest library on
// the guest's behalf: with translations embedded, the .so path is a name the
// loader resolves and not a file on disk. See the implementation comment.
int kl_can_load(const char *path);
// ...and the wider question the guest actually asks: could klb_dlopen() open it?
// True for everything kl_can_load covers PLUS the libraries this shim serves
// synthetically, which have no file anywhere. Answering an existence check for
// the guest wants THIS one — see the implementation comment in kl_dl.c for the
// black screen the difference caused. Defined in kl_dl.c.
int kl_can_dlopen(const char *path);
// Look up an exported symbol. NULL if absent.
void      *kl_sym(kl_image *img, const char *name);
// Answer a guest symbol with a function of ours, at relocation time. This
// reaches every call the compiler routed through the PLT — which for a guest
// built with default visibility is every cross-translation-unit call — so it is
// the one way to watch a guest-internal function that imports nothing. Register
// before the image loads; kl_sym still answers the guest's real address, which
// is how an interposer calls through. Diagnostic: nothing in a shipping path
// registers one.
void       kl_interpose(const char *name, void *fn);
// Run DT_INIT_ARRAY. Separate from kl_load so tests can inspect first.
void       kl_run_init(kl_image *img);
void       kl_unload(kl_image *img);
const char *kl_error(void);

// Base address the image was mapped at, and its total vaddr span.
void      *kl_base(kl_image *img);
size_t     kl_span(kl_image *img);

// Stats, for tests and the load gate.
typedef struct {
    unsigned relative, abs64, glob_dat, jump_slot;   // relocations applied
    // R_AARCH64_TLSDESC descriptors pointed at our resolver (S0.1b). Counted
    // apart from the others because it is the one relocation that installs a
    // CALL rather than an address: every access to that variable, forever,
    // goes through runtime/kl_tls_thunk.S.
    unsigned tlsdesc;
    unsigned tprel;                                  // R_AARCH64_TLS_TPREL64 (static TLS)
    // glibc mode: relocations against a DEFINED STB_GNU_UNIQUE symbol that bound
    // to an earlier image's definition rather than this image's own copy.
    unsigned unique_shared;
    // glibc mode: relocations against a symbol the image DEFINES that bound to
    // an earlier definition in the lookup scope (ld.so's symbol preemption).
    unsigned preempted;
    unsigned tls_rewrites;                           // mrs tpidr_el0 -> tpidrro_el0
    unsigned tp_writes;                              // msr tpidr_el0, xN -> nop
    // ...and TLS sites the data test refused. Counted apart from
    // x18_data_words because the two mean opposite things: a refused x18 site
    // costs nothing, a refused TLS site leaves the guest reading a register the
    // kernel clobbers. Nonzero
    // here is a finding, not a statistic — rewrite_tls names each one.
    unsigned tls_refused;
    unsigned svc_sites;                              // inline syscalls found
    unsigned imports_bound, imports_missing;
    // ...and the undefined imports that are WEAK, which is a different answer
    // rather than a subset of imports_missing. A weak undefined is how a guest
    // asks "does this platform have X?": the call site null-tests the slot, so
    // resolving one to a non-null abort trampoline answers YES and then aborts
    // when the guest takes the branch it was offered. Left NULL and counted
    // here. Nonzero is normal (memfd_create, getentropy, __cxa_thread_atexit_impl,
    // jemalloc's mallctl, TSAN's __google_potentially_blocking_region_*), but a
    // NULL-pointer fault with no name in the log starts by reading this list.
    unsigned imports_weak_null;
    // Guest instructions naming x18, and how many were redirected to a veneer.
    // Anything refused reads Darwin's reserved x18.
    unsigned x18_sites, x18_patched, x18_refused;
    // Words that decode as an x18 site but sit in DATA inside an
    // executable section. Not sites and not refusals — reported
    // separately because a number moving here means the second detector
    // changed its mind about what is code.
    unsigned x18_data_words;
    // `mrs Xt, CTR_EL0`, which EL0 may not execute on Darwin. Same
    // veneer machinery, counted apart because the x18 numbers above are exact
    // and `make check` gates on them.
    unsigned ctr_sites, ctr_patched, ctr_refused;
} kl_stats;
const kl_stats *kl_get_stats(kl_image *img);

// Unique names of imports the shim could not resolve. Valid until kl_unload().
const char *const *kl_missing_imports(kl_image *img, unsigned *count);
// ...and the WEAK undefined ones, which are left NULL rather than stubbed.
// See stats.imports_weak_null: a different answer, not a subset of the above.
const char *const *kl_weak_imports(kl_image *img, unsigned *count);

// ---- shim ----
// Resolve a bionic/NDK import by name. Returns NULL if unimplemented.
void *kl_shim_lookup(const char *name);
// A raw process_vm_readv on this process (kl_libc.c) hands each copied chunk
// to this filter (remote address, local copy, length). One door at a time.
void  kl_set_selfread_filter(void (*fn)(const void *remote, void *local, size_t n));
// Install the bionic stack-guard canary into Darwin TSD slot 5 for this thread.
// Must be called on every thread that will execute guest code.
void  kl_thread_init(void);
// Run `fn` at the end of every later kl_thread_init (idempotent per fn; 4 max).
void  kl_thread_init_hook(void (*fn)(void));
// Restore default fatal-signal disposition and flush stdio. Call before abort()
// in any diagnostic path: the guest installs handlers that expect a Linux
// ucontext_t, and they swallow our abort and hang instead of dying.
void  kl_fatal_prepare(void);
// Map a guest FILE* (an offset into our fake bionic __sF block) to a host stream.
FILE *kl_host_file(void *guest);
// A second guest FILE * vocabulary (kl_glibc.c's glibc-shaped streams): `fn`
// maps a pointer it made to its host stream and answers NULL for any other.
void  kl_host_file_resolver(FILE *(*fn)(void *guest));

// Build a code stub that tail-calls `handler` with `name` in x0, so whatever the
// guest reached for names itself instead of arriving as one anonymous abort.
// Used for unresolved ELF imports and for eglGetProcAddress's GL entry points.
void *kl_named_stub(const char *name, void *handler);
// The forwarding variant: a stub that branches to `tramp` with `desc` in x16 and
// nothing else disturbed, so the trampoline can log and then let the original call
// run with its arguments intact. `desc` must be a stable { const char *name;
// void *real; } pair. See runtime/gfx/kl_gl_trace.S for the register contract.
void *kl_trace_stub(const char *name, void *desc, void *tramp);

// ---- image registry (backs guest dlopen/dlsym/dladdr) ----
void      kl_set_library_path(const char *dir);
void      kl_register_image(const char *soname, kl_image *img);
kl_image *kl_find_image(const char *soname);
// Load an .so plus its DT_NEEDED chain, dependencies first (kl_dl.c).
kl_image *kl_load_recursive(const char *path);
// Read a file's DT_NEEDED sonames without loading it (kl_image.c).
int       kl_list_needed(const char *path, char names[][128], int max);
// ...and DT_SONAME / DT_RUNPATH (DT_RPATH when no RUNPATH) beside it; either
// buffer may be NULL, `cap` is each one's size. Returns the NEEDED count, -1 if
// the file cannot be read.
int       kl_list_dynamic(const char *path, char names[][128], int max,
                          char *soname, char *runpath, size_t cap);

// Every guest library the bundle's translations carry, as .so names. The device
// half of "list the guest libdir": with translations embedded, the ELF tree is
// not in the bundle at all, so a guest whose library set is the WHOLE directory
// (rather than a named chain) has nothing to enumerate. Returns the count.
unsigned  kl_list_translations(char names[][64], unsigned max);
// The file that backs a guest library path, when the bundle carries it as a
// translation. Existence questions asked ON the guest's behalf go through
// kl_can_load; this is for the ones that need the file itself. Returns 1 and
// fills `out` on a hit, 0 otherwise.
int       kl_library_file(const char *path, char *out, size_t cap);
// Search every registered guest image for an export (kl_dl.c); used by the
// loader's import binding after the shim misses.
void     *kl_guest_sym_global(const char *name);
// The same search restricted to glibc images (glibc=1) or bionic ones (0), in
// registration order; `soname_out` names the image that answered.
void     *kl_guest_sym_ordered(const char *name, int glibc, const char **soname_out);
// A glibc image's DT_NEEDED soname that a runtime module REPLACES (kl_dl.c's
// claims table): the module's owner label, or NULL when nothing claims it.
const char *kl_glibc_soname_claim(const char *soname);
// ...and that module's answer for `name`, for the claims that have a module
// (libvulkan -> kl_vulkan, libopenxr_loader -> kl_openxr, libopenvr_api ->
// kl_openvr, and whatever a port provides). `who_out` is
// the module's owner label on a hit.
void     *kl_glibc_claim_sym(const char *soname, const char *name, const char **who_out);
// A port's own module for a claimed soname (hla: kl_steamapi for
// libsteam_api.so, its kept SDL2 for libSDL2*): `owner` relabels the row when
// non-NULL. Before the images that need it are bound. -1 when no row claims it.
int       kl_glibc_claim_provide(const char *soname, const char *owner,
                                 void *(*sym)(const char *));
// The image's program headers, as mapped. Every guest lib's first PT_LOAD maps
// file offset 0 at vaddr 0, so the phdr array is live at base + e_phoff and the
// load bias is simply the base. This is what dl_iterate_phdr hands the guest's
// unwinder so it can find PT_GNU_EH_FRAME (kl_dl.c).
const void *kl_phdrs(kl_image *img, unsigned *count);

// Which guest image an address is in, and its offset within it. NULL if none.
// Guest libraries land wherever the kernel put them, so this is what makes a
// raw pc from a fault mean anything.
const char *kl_addr_image(const void *addr, size_t *offset);

// Every guest image and its load address, one line each. A guest pc means
// nothing without the base: subtract it to match a disassembly of the .so, to
// set a breakpoint on a guest function, or to read a sampling profile.
void kl_dl_report_images(FILE *f);


// Rewrite a guest file path into the synthetic /proc and /sys tree when we serve
// it; returns `path` unchanged otherwise. Every guest file entry point goes
// through this — Unity reads its CPU and memory configuration out of procfs.
const char *kl_guest_path(const char *path, char *buf, size_t cap);
// What the synthetic /proc/self/exe names (a symlink to a guest path). A door
// that knows the guest's executable says so; unset, the entry is absent.
void kl_proc_set_exe(const char *guest_path);
// Register the APK -> unpacked-tree mapping for raw "<apk>/assets/..." opens
// (split assets). See the implementation comment in kl_libc.c.
void kl_guest_path_map(const char *apk, const char *unpacked_dir);
// ...and the guest's public external storage ("/sdcard" and its aliases), for a
// guest that writes there by literal path instead of asking Java. `dir` must be
// what Environment.getExternalStorageDirectory() answers, or the two doors onto
// one directory disagree. See the implementation comment in kl_libc.c.
void kl_guest_extstorage_map(const char *dir);

// How much memory the guest asked us to decommit, and how we served it. Zero
// bytes here on a Unity guest means its VirtualMemory layer's decommit is doing
// nothing and the footprint is a high-water mark (see klb_madvise).
void kl_madv_report(void);

// Memory pressure. The guest is told the room ran out through
// UnityPlayer.nativeLowMemory, which is where a title drops its caches; nothing
// delivered it before, so every cache a Quest build releases under pressure was
// held here forever. _init subscribes to the OS's own signal, _poll checks our
// footprint against the reported budget (cheap enough for once a frame), and
// _fire is the on-demand knob — the way to A/B this without a headset.
void kl_mem_pressure_init(void);
void kl_mem_pressure_poll(void);
void kl_mem_pressure_fire(const char *why);
// Trim before a scene load (KL_TRIM_ON_LOAD=1). Unity logs `Loading:<scene>`
// when a scene load starts, and on HARD BULLET the next scene is loaded on top
// of the old one (+700-750 MB within a second — round 26's Vulkan OOM) — far
// too fast for the edge-triggered OS pressure door. kl_mem_scene_load is called
// by the guest-log funnel on whatever guest thread logged the banner and only
// RECORDS the request; kl_mem_pressure_poll (the thread the trim door is
// already knocked from) delivers it, so the guest is never re-entered from
// inside its own __android_log_print.
void kl_mem_scene_load(const char *scene);
// KL_TRIM_ON_LOAD read once (default 0); the log doors format a Unity line
// for kl_mem_log_banner only when it is on.
int  kl_mem_trim_on_load(void);
void kl_mem_log_banner(const char *tag, const char *msg);
// `Loading:<scene>` parser (trailing newline/whitespace stripped): 0 = not a
// banner, 1 = named load, 2 = bare end marker. Unit-tested by t_trimload.
int kl_unity_load_banner(const char *msg, char *scene, size_t n);
void kl_mem_report(void);

// The budget and what is left of it, for the other doors a guest asks the same
// question through — ActivityManager.getMemoryInfo is /proc/meminfo asked in
// Java, and the two must not answer differently.
uint64_t kl_mem_budget_bytes(void);
uint64_t kl_mem_available_bytes(void);

// errno numbering diverges above 34 between Linux and Darwin, and guest code
// compares against Linux's values (see kl_libc.c).
int kl_errno_to_linux(int e);
int kl_errno_from_linux(int e);

// KL_TRACE_FS reporting for guest file operations (see kl_libc.c).
void kl_fs_trace_open(const char *path, int flags, int fd);
// The inverse of kl_guest_path: a host path in the guest's own spelling, for
// the one libc call that hands a path back OUT (getcwd).
const char *kl_host_path_as_guest(const char *host, char *buf, size_t cap);
// The guest's own spelling of its working directory, remembered at chdir so
// getcwd can hand back what the guest asked for rather than the host's layout.
void kl_guest_cwd_note(const char *guest_path);

// A diagnostic's view of the guest's file operations, so RUNTIME_SHIP does not
// have to know that RUNTIME_DIAG exists. NULL unless something installs one;
// `kl_metadump_watch_install` is the only caller today, and it wants exactly
// two events — which fd a path was opened on, and where that fd got mapped.
//
// `path` is set for "open" and NULL for "mmap"; `caller` is the GUEST's return
// address where one is available, which is the whole point of the hook (an
// obfuscated library's call sites are not findable any other way).
typedef void (*kl_file_watch_fn)(const char *op, const char *path, int fd,
                                 void *addr, size_t len, long long off,
                                 void *caller);
extern kl_file_watch_fn kl_file_watch;

// ...and the same idea one layer down: a diagnostic's answer for a shim symbol,
// consulted by kl_shim_lookup before any tier. NULL unless installed. Return
// NULL for a name the diagnostic does not want, which is all of them.
extern void *(*kl_shim_override)(const char *name);

// A guest library that IS the GL implementation, consulted for `gl*`/`egl*`
// before the shim's own graphics tiers. NULL for every target but JKXR.
//
// It has to be NAMED by the door rather than found by rule. The rule looks
// obvious — "prefer a loaded guest image that exports the symbol" — and it is
// wrong three times over in this tree: libvrlink_scene defines 150 gl* names as
// its OWN loader's wrappers (each calling through an fnptr it initialises
// itself), libAVProMovieCaptureNative defines glActiveTexture and friends as
// weak DATA pointers, and libUE4 defines a hundred more. Binding another
// library's import to any of those is a null call or a jump into a pointer
// variable, and it would happen on the two targets that currently work best.
//
// JKXR is the case that needs it: libgl4es translates fixed-function GL 1.x
// into GLES, its renderer's DT_NEEDED puts it before libGLESv3, and gl4es only
// works if it sees the WHOLE stream — the fixed-function state it is
// translating is what decides which generated shader a draw uses. Split the
// stream and every draw runs with no program bound.
void kl_shim_set_guest_gl(kl_image *img);

// Mutex owner table dump (kl_pthread.c): who holds each translated guest
// mutex and from where it was locked. Called from t_boot's fault handler so
// a hang names the contested mutex's owner, not just its waiters.
void kl_pthread_report(FILE *out);
// Guest (Linux) signal number / sigset -> Darwin. One swapped pair,
// SIGUSR2 <-> SIGSYS; everything else passes through. See kl_libc.c.
int      kl_sig_to_darwin(int linux_sig);
uint64_t kl_sigmask_to_darwin(uint64_t linux_mask);

#endif
