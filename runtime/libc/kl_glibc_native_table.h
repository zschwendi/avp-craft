// The glibc personality's NATIVE rows — included by kl_glibc.c inside
// g_native_rows. Same macros as kl_glibc_table.h (see the table in kl_glibc.c).
//
// These are the names the Alyx jailbreak host answered with its own AlyxGuest*
// functions (hla/recon/hla_glibc_host_intercepted.txt) — so they were never in
// the stand-in, and must not be: the reference build still runs the host, which
// binds them before the stand-in is looked at. The native boot (no host) gets
// them from here, consulted BEFORE the main table (kl_glibc_lookup_native).
// Only names the native sweep measured as imported are listed
// (runtime/guest/kl_alyx_native.c; hla/NATIVE-PORT.md §1b).

// ---- the allocator (NATIVE-PORT.md R6). libmimalloc.so — `hlvr`'s FIRST
// DT_NEEDED — exports this whole family, and these rows are what keep it from
// winning: the personality is searched before any image (kl_image.c
// sym_value_glibc), so every image's malloc and free meet the same heap (the
// bionic table's, Darwin's) as the strdup / aligned_alloc / reallocarray rows
// and everything the bionic table allocates on the guest's behalf. ----
    GL_SAME(malloc) GL_SAME(free) GL_SAME(calloc) GL_SAME(realloc)
    GL_SAME(posix_memalign) GL_SAME(malloc_usable_size)

// ---- process / cxa / errno / auxv ----
    GL_AS(__errno_location, __errno)
    GL_AS(__getauxval, getauxval)
    GL_SAME(__cxa_atexit) GL_SAME(__cxa_finalize) GL_SAME(__cxa_thread_atexit_impl)
    GL_SAME(exit) GL_SAME(_exit) GL_SAME(abort)
    // libstdc++'s __gthread_active_p tests this WEAK import's address to decide
    // whether threads exist at all; NULL would turn its mutexes into no-ops.
    GL_AS(__pthread_key_create, pthread_key_create)
    GL_SAME(pthread_create) GL_SAME(__sched_cpucount)
    GL_FN(sysconf, gl_sysconf) GL_FN(__sysconf, gl_sysconf)
    // `hlvr`'s _start calls it with main. The native Alyx door answers it
    // (kl_glibc_override: hlvr's constructors, then main); with no door
    // installed a call aborts naming itself rather than running anything.
    GL_ABORT(__libc_start_main)

// ---- dl (the runtime's image registry). The native Alyx door overrides all
// of these with its own glibc loader's (kl_alyx_native.c: ld.so's scopes,
// payload paths to translations); these rows are what any other glibc guest
// would get. ----
    GL_SAME(dlopen) GL_SAME(dlsym) GL_SAME(dlclose) GL_SAME(dladdr)
    GL_FN(dlmopen, gl_dlmopen)

// ---- console stdio (glibc va_list is bionic's on AArch64; the stream
// argument is resolved through kl_host_file, which knows gl_file) ----
    GL_SAME(printf) GL_SAME(fprintf) GL_SAME(vfprintf) GL_SAME(puts)

// ---- files ----
    GL_SAME(faccessat) GL_SAME(flock) GL_SAME(pread64) GL_SAME(fstat64)

// ---- SysV semaphores: libtier0's named CThreadSyncObject semaphores (Source
// 2's game-instance mutex among them). Emulated in-process (gl_semget):
// visionOS has no SysV IPC — the kernel answers SIGSYS. ----
    GL_FN(semget, gl_semget) GL_FN(semop, gl_semop) GL_FN(semtimedop, gl_semtimedop)
    GL_FN(semctl, gl_semctl)
