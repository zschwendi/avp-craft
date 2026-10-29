// C handlers behind the asm thunks in kl_va_thunks.S. Each receives the guest's
// named arguments normally plus an AAPCS64 kl_va*, re-marshals into Darwin layout,
// and calls the host implementation. The host's printf/scanf engine does the
// actual formatting -- we never reimplement it.
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "kl_va.h"
#include "klepton.h"

// Darwin arm64's va_list is a bare char*, so a marshalled buffer *is* a va_list.
#define KL_MARSHAL(fmt, va, mode)                                        \
    char _m[512] __attribute__((aligned(16)));                           \
    if (kl_va_marshal((fmt), (va), _m, sizeof _m, (mode)) == (size_t)-1)  \
        return -1;                                                       \
    va_list _ap = (va_list)_m

int klh_printf(const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    return vprintf(fmt, _ap);
}
int klh_fprintf(void *f, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    return vfprintf(kl_host_file(f), fmt, _ap);
}
int klh_sprintf(char *buf, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    return vsprintf(buf, fmt, _ap);
}
int klh_snprintf(char *buf, size_t n, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    return vsnprintf(buf, n, fmt, _ap);
}
int klh_asprintf(char **out, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    return vasprintf(out, fmt, _ap);
}
int klh_dprintf(int fd, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    return vdprintf(fd, fmt, _ap);
}
int klh_syslog(int pri, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    fprintf(stderr, "[guest syslog %d] ", pri);
    int n = vfprintf(stderr, fmt, _ap);
    fputc('\n', stderr);
    return n;
}
int klh_android_log_print(int prio, const char *tag, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    static const char lv[] = "??VDIWEF";
    // KL_TRIM_ON_LOAD: a Unity line is formatted once more (from the marshalled
    // copy, so _ap below is untouched) to look for the `Loading:<scene>`
    // banner. Off by default, and only "Unity"-tagged lines pay for it.
    if (tag && tag[0] == 'U' && strcmp(tag, "Unity") == 0 && kl_mem_trim_on_load()) {
        char line[96];
        va_list ap2 = (va_list)_m;
        vsnprintf(line, sizeof line, fmt, ap2);
        kl_mem_log_banner(tag, line);
    }
    fprintf(stderr, "%c/%s: ", (prio >= 0 && prio < 8) ? lv[prio] : '?', tag ? tag : "");
    int n = vfprintf(stderr, fmt, _ap);
    fputc('\n', stderr);
    return n;
}

// __android_log_assert(cond, tag, fmt, ...) is what LOG_ALWAYS_FATAL and every
// bionic assert() macro land on, and it does NOT return: liblog writes the line
// at FATAL and aborts the process. A guest that gets here has already decided it
// cannot continue, so the only thing we owe it is the message — printed the way
// the other klepton fatals are, with kl_fatal_prepare() first, because an assert
// that dies silently reads as a crash with no cause and costs a whole round.
//
// `fmt` is allowed to be NULL, and then bionic composes the text from the
// condition string alone; that branch is not a fallback, it is a documented
// calling convention some callers use.
__attribute__((noreturn))
void klh_android_log_assert(const char *cond, const char *tag,
                            const char *fmt, kl_va *va) {
    fprintf(stderr, "F/%s: ", tag ? tag : "");
    if (fmt) {
        char _m[512] __attribute__((aligned(16)));
        if (kl_va_marshal(fmt, va, _m, sizeof _m, KL_VA_PRINTF) != (size_t)-1)
            vfprintf(stderr, fmt, (va_list)_m);
        else
            // Marshalling failed (a format we cannot describe). The raw format
            // string still names the assertion, which is more than nothing.
            fputs(fmt, stderr);
    } else {
        fprintf(stderr, "assertion \"%s\" failed", cond ? cond : "");
    }
    fputc('\n', stderr);
    fprintf(stderr, "[klepton] fatal: guest __android_log_assert (%s)\n",
            cond ? cond : "no condition given");
    kl_fatal_prepare();
    abort();
}

// BSD err.h, which bionic has and the generated table cannot reach: Darwin
// declares warnx in <err.h>, kl_shim.c does not include it, and the generator's
// answer to "does Darwin declare this" is therefore no. It would be a variadic
// anyway, so a direct forward was never available (AAPCS64 registers vs
// Darwin's stack slots).
//
// The real warnx prefixes the program name and appends a newline, and does NOT
// append strerror the way warn does. OpenJK uses it for its own diagnostics, so
// the prefix says which side of the shim the line came from rather than
// restating a host program name the guest never chose. QuakeQuest's engine
// diagnostics take the same door.
int klh_warnx(const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    fputs("[guest warnx] ", stderr);
    int n = vfprintf(stderr, fmt, _ap);
    fputc('\n', stderr);
    return n;
}
// ---- a guest's own release logger, forwarded ----
// Some guests ship a printf-shaped diagnostic of their own whose body formats
// the line and then throws it away (the retail build of the engine keeps the
// call sites and drops the output). Those call sites are free instrumentation:
// interposing the function and forwarding the formatted text costs nothing and
// says what the game thinks it is doing, in the game's own words.
//
// The variadic shape is why it needs a thunk: the guest calls AAPCS64-style,
// and no host function can read those registers as varargs directly. `fmt` is
// the guest's, so the host printf engine formats it exactly as the guest meant.
//
// The sink is a pointer rather than a fixed fprintf so the per-target file that
// installs the interposer decides what a line means (prefix, timestamps, any
// state it wants to track). NULL sink = formatted and dropped, as before.
void (*kl_guest_trace_line)(const char *text);

int klh_guest_trace_printf(const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    char line[1024];
    int n = vsnprintf(line, sizeof line, fmt, _ap);
    if (kl_guest_trace_line) kl_guest_trace_line(line);
    return n;
}

int klh_sscanf(const char *s, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_SCANF);
    return vsscanf(s, fmt, _ap);
}
int klh_fscanf(void *f, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_SCANF);
    return vfscanf(kl_host_file(f), fmt, _ap);
}

// ---- non-printf variadics: exactly one trailing argument, no format string ----
int kl_open_flags(int lx);          // kl_libc.c
int klh_open(const char *path, int flags, kl_va *va) {
    // Guest passes Linux O_* values, which differ from Darwin's; translate.
    // The mode argument only matters with O_CREAT, but reading it is harmless.
    // The path goes through the /proc rewrite for the same reason fopen does.
    char kp[1024];
    int fd = open(kl_guest_path(path, kp, sizeof kp), kl_open_flags(flags), (int)kl_va_gp(va));
    kl_fs_trace_open(path, flags, fd);
    return fd;
}
// cmd/req and the trailing argument are TRANSLATED, never forwarded raw: the
// two numberings are close enough that a wrong one succeeds meaning something
// else. F_SETFL takes the
// same flag word open() takes — Linux O_NONBLOCK is Darwin O_EXCL — so the
// translation lives next to kl_open_flags in kl_libc.c rather than here.
int kl_fcntl(int fd, int cmd, uintptr_t arg);         // kl_libc.c
int kl_ioctl(int fd, unsigned long req, uintptr_t arg);
int klh_fcntl(int fd, int cmd, kl_va *va) {
    return kl_fcntl(fd, cmd, (uintptr_t)kl_va_gp(va));
}
int klh_ioctl(int fd, unsigned long req, kl_va *va) {
    return kl_ioctl(fd, req, (uintptr_t)kl_va_gp(va));
}

// ---- bionic's variadic fortify pair (HL2VR's srceng batch) ----
// __sprintf_chk / __snprintf_chk are the VARIADIC forms, so they ride the
// thunk machinery like sprintf itself; the *_chk part is only the bounds
// contract. bionic's argument order is (dest, [supplied_size,] flags,
// compiler_size, fmt, ...) — note flags and compiler_size are SWAPPED relative
// to the __*_chk order Darwin's three-underscore spellings use, which is why
// these cannot forward to ___sprintf_chk.
int klh___sprintf_chk(char *d, int flags, size_t dl, const char *fmt, kl_va *va) {
    (void)flags;
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    // dl is what the compiler proved about the destination; exceed it and
    // bionic aborts, so do the same rather than silently truncating.
    int r = vsnprintf(d, dl, fmt, _ap);
    if (dl && (size_t)r >= dl) {
        fprintf(stderr, "[klepton] fatal: __sprintf_chk overflow (%d >= %zu)\n", r, dl);
        abort();
    }
    return r;
}
int klh___snprintf_chk(char *d, size_t n, int flags, size_t dl, const char *fmt, kl_va *va) {
    (void)flags;
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    // The check is on the SUPPLIED size against the compiler's proof — a
    // caller passing n > dl is already wrong before any formatting happens.
    if (n > dl) {
        fprintf(stderr, "[klepton] fatal: __snprintf_chk overflow (%zu > %zu)\n", n, dl);
        abort();
    }
    return vsnprintf(d, n, fmt, _ap);
}
