// The glibc personality. See kl_glibc.h for where it sits.
//
// A glibc guest is not a bionic guest with different names. Most of what it
// imports IS the same call with the same ABI — aarch64 Linux fixed the struct
// layouts for both C libraries, so stat, dirent, statvfs, utsname, timespec,
// pthread_* sizes (glibc's are larger, never smaller) and the errno/open/mmap
// constants agree — and for those the bionic table's answer is the answer.
// What differs is small and specific, and every difference is a place where a
// forwarded call would be silently wrong rather than loudly missing:
//
//   * struct layouts that disagree: `struct sigaction` (glibc keeps a 128-byte
//     sa_mask BEFORE sa_flags; bionic LP64 leads with the flags), `struct
//     addrinfo` (glibc puts ai_addr before ai_canonname), `stack_t` against
//     Darwin (flags and size swap places, and SS_DISABLE is 2 against 4).
//   * FILE. glibc's FILE is not opaque to its callers: getc_unlocked and
//     putc_unlocked are inline, reading _IO_read_ptr/_IO_write_ptr, and the
//     jailbreak host's console identifies stdout/stderr by glibc's `_flags`
//     magic and `_fileno` at +0x70. So a glibc guest's streams are glibc-SHAPED
//     objects that stand for a host stream (gl_file below), with every buffer
//     pointer NULL so every inline fast path falls through to a call.
//   * the GNU spellings: strerror_r returns char *, the XSI one is
//     __xpg_strerror_r; basename's POSIX form is __xpg_basename.
//   * `_l` functions taking a locale_t: glibc guests get their locale objects
//     from the host library (it intercepts newlocale/uselocale), which Darwin's
//     `_l` functions cannot read. They are rebuilt on the process locale.
//   * names glibc has and bionic does not: the fortified *_chk printf family,
//     __xstat and the LFS *64 aliases, __isoc23_*, _dl_find_object, the data
//     objects (__libc_single_threaded, __progname, timezone...).
//
// Anything else a glibc guest reaches for is refused BY NAME: a stub that logs
// "[glibc] <name>: refused" once and returns the call's failure value with
// ENOSYS — never an invented success. The stand-in exports only what this file
// lists, so a name that is in none of the families below stays unresolved in
// the host's linker and is reported there.
#include "kl_glibc.h"

#include <TargetConditionals.h>
#include <complex.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <fenv.h>
#include <inttypes.h>
#include <libgen.h>
#include <pthread.h>
#include <setjmp.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <xlocale.h>

#include "klepton.h"
#include "kl_va.h"

int kl_open_flags(int linux_flags);        // kl_libc.c
const char *kl_guest_path(const char *path, char *buf, size_t cap);
void kl_errno_set(int darwin_errno);        // kl_libc.c
__attribute__((noreturn)) void kl_unresolved_named(const char *name);  // kl_shim.c

// ---------------------------------------------------------------- plumbing

static void *(*g_host_resolve)(const char *name);
static int (*g_console)(void *fp, const char *fmt, void *va);

void kl_glibc_set_host_resolver(void *(*fn)(const char *)) { g_host_resolve = fn; }
void kl_glibc_set_console(int (*fn)(void *, const char *, void *)) { g_console = fn; }

// The bionic table's answer, looked up once per call site through a cached
// pointer — the table is the same for the life of the process.
#define BIONIC(name, type) \
    static __typeof__(type) _b; if (!_b) _b = (__typeof__(type))kl_shim_lookup(name);
// The host library's answer where it owns the objects (see the header).
static void *host_or(const char *name, void *fallback) {
    void *p = g_host_resolve ? g_host_resolve(name) : NULL;
    return p ? p : fallback;
}

// ---- refusals ----
// One log line per name, then the call's own failure value with ENOSYS.
static const char *g_refused[256];
static unsigned    g_refused_n;
static pthread_mutex_t g_refused_mu = PTHREAD_MUTEX_INITIALIZER;
static void refused(const char *name) {
    pthread_mutex_lock(&g_refused_mu);
    unsigned i = 0;
    for (; i < g_refused_n; i++) if (g_refused[i] == name) break;
    int first = i == g_refused_n;
    if (first && g_refused_n < 256) g_refused[g_refused_n++] = name;
    pthread_mutex_unlock(&g_refused_mu);
    if (first)
        fprintf(stderr, "  [glibc] %s: refused — the personality does not implement it "
                        "(ENOSYS to the guest)\n", name);
    kl_errno_set(ENOSYS);
}
// kl_named_stub puts the name in x0 and tail-calls these; the guest's own
// return address is untouched, so they return straight to the caller.
static long  refuse_int(const char *name) { refused(name); return -1; }
static void *refuse_ptr(const char *name) { refused(name); return NULL; }
static long  refuse_err(const char *name) { refused(name); return 38; /* Linux ENOSYS */ }

void kl_glibc_report(FILE *out) {
    if (!out || !g_refused_n) return;
    fprintf(out, "  [glibc] %u name(s) refused this run:", g_refused_n);
    for (unsigned i = 0; i < g_refused_n; i++) fprintf(out, " %s", g_refused[i]);
    fprintf(out, "\n");
}

// ---------------------------------------------------------------- images
//
// _dl_find_object and dl_iterate_phdr answer from the images the loader
// registered: the payload the host mapped, each with its in-memory phdrs.
typedef struct {
    char        name[256];
    uint8_t    *base;
    size_t      span;
    const void *phdrs;
    unsigned    phnum;
    void       *eh_frame_hdr;
} gl_image;
static gl_image        g_img[512];
static unsigned        g_nimg;
static pthread_rwlock_t g_img_lock = PTHREAD_RWLOCK_INITIALIZER;

typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr,
                 p_filesz, p_memsz, p_align; } gl_phdr;
#define GL_PT_LOAD         1
#define GL_PT_GNU_EH_FRAME 0x6474e550

void kl_glibc_register_image(const char *name, void *base, size_t span,
                             const void *phdrs, unsigned phnum) {
    pthread_rwlock_wrlock(&g_img_lock);
    gl_image *im = NULL;
    for (unsigned i = 0; i < g_nimg; i++)
        if (g_img[i].base == base) { im = &g_img[i]; break; }
    if (!im && g_nimg < sizeof g_img / sizeof *g_img) im = &g_img[g_nimg++];
    if (im) {
        snprintf(im->name, sizeof im->name, "%s", name ? name : "");
        im->base = base; im->span = span; im->phdrs = phdrs; im->phnum = phnum;
        im->eh_frame_hdr = NULL;
        const gl_phdr *ph = phdrs;
        for (unsigned i = 0; ph && i < phnum; i++)
            if (ph[i].p_type == GL_PT_GNU_EH_FRAME)
                im->eh_frame_hdr = (uint8_t *)base + ph[i].p_vaddr;
    }
    pthread_rwlock_unlock(&g_img_lock);
}

// glibc 2.35's struct dl_find_object: what libgcc_s's unwinder (and the 40
// payload objects that link its unwinder statically) asks for a PC — the
// image's bounds and its PT_GNU_EH_FRAME. -1 for a PC in no registered image,
// which is the "not found" the unwinder handles.
struct gl_dl_find_object {
    unsigned long long dlfo_flags;
    void *dlfo_map_start, *dlfo_map_end;
    void *dlfo_link_map;
    void *dlfo_eh_frame;
    unsigned long long dlfo_reserved[7];
};
static int gl_dl_find_object(void *pc, struct gl_dl_find_object *r) {
    int rc = -1;
    pthread_rwlock_rdlock(&g_img_lock);
    for (unsigned i = 0; i < g_nimg; i++) {
        gl_image *im = &g_img[i];
        if ((uint8_t *)pc < im->base || (uint8_t *)pc >= im->base + im->span) continue;
        memset(r, 0, sizeof *r);
        r->dlfo_map_start = im->base;
        r->dlfo_map_end   = im->base + im->span;
        r->dlfo_eh_frame  = im->eh_frame_hdr;
        rc = 0;
        break;
    }
    pthread_rwlock_unlock(&g_img_lock);
    return rc;
}

// glibc's struct dl_phdr_info (the bionic one is the same through dlpi_phnum,
// and both carry adds/subs/tls after it).
struct gl_dl_phdr_info {
    uint64_t dlpi_addr; const char *dlpi_name; const void *dlpi_phdr; uint16_t dlpi_phnum;
    unsigned long long dlpi_adds, dlpi_subs; size_t dlpi_tls_modid; void *dlpi_tls_data;
};
static int gl_dl_iterate_phdr(int (*cb)(struct gl_dl_phdr_info *, size_t, void *), void *data) {
    // A snapshot, so the callback runs without the lock held (it may load).
    gl_image snap[512];
    pthread_rwlock_rdlock(&g_img_lock);
    unsigned n = g_nimg;
    memcpy(snap, g_img, n * sizeof *snap);
    pthread_rwlock_unlock(&g_img_lock);
    int r = 0;
    for (unsigned i = 0; i < n && !r; i++) {
        struct gl_dl_phdr_info info = {0};
        info.dlpi_addr  = (uint64_t)(uintptr_t)snap[i].base;
        info.dlpi_name  = snap[i].name;
        info.dlpi_phdr  = snap[i].phdrs;
        info.dlpi_phnum = (uint16_t)snap[i].phnum;
        info.dlpi_adds  = n;
        r = cb(&info, sizeof info, data);
    }
    return r;
}

// ---------------------------------------------------------------- FILE
//
// struct _IO_FILE on aarch64 glibc is 0xd8 bytes and _IO_FILE_plus adds the
// vtable pointer at +0xd8. The fields a caller outside libc reads:
//   +0x00 int _flags            (_IO_MAGIC 0xFBAD0000 in the high half)
//   +0x08.. +0x30 the read/write buffer pointers the unlocked macros compare
//   +0x70 int _fileno
//   +0xc0 int _mode             (-1: byte-oriented)
// Every buffer pointer stays NULL: NULL >= NULL, so getc_unlocked/putc_unlocked
// always take their slow path (__uflow / __overflow), which lands here.
#define GL_IO_MAGIC        0xFBAD0000u
#define GL_IO_UNBUFFERED   0x0002u
#define GL_IO_NO_READS     0x0004u
#define GL_IO_NO_WRITES    0x0008u
#define GL_IO_LINKED       0x0080u
#define GL_IO_IS_FILEBUF   0x2000u

typedef struct gl_file {
    uint8_t io[0xe0];        // the glibc-visible object
    FILE   *host;            // the stream it stands for
} gl_file;

static void gl_file_init(gl_file *g, FILE *host, int fd, uint32_t flags) {
    memset(g->io, 0, sizeof g->io);
    *(uint32_t *)(g->io + 0x00) = GL_IO_MAGIC | GL_IO_IS_FILEBUF | GL_IO_LINKED | flags;
    *(int32_t  *)(g->io + 0x70) = fd;
    *(int32_t  *)(g->io + 0xc0) = -1;
    g->host = host;
}

static gl_file g_stdin, g_stdout, g_stderr;
static void   *g_stdin_p = &g_stdin, *g_stdout_p = &g_stdout, *g_stderr_p = &g_stderr;

// Live streams this personality made, so kl_host_file can map one back. A
// guest FILE * is either one of these or not ours at all — never a pointer we
// have to read to find out.
#define GL_FILES 4096
static gl_file *g_files[GL_FILES];
static pthread_mutex_t g_files_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned gl_slot(const void *p) { return (unsigned)(((uintptr_t)p >> 4) * 2654435761u) % GL_FILES; }
static void gl_track(gl_file *g) {
    pthread_mutex_lock(&g_files_mu);
    unsigned s = gl_slot(g);
    for (unsigned k = 0; k < GL_FILES; k++, s = (s + 1) % GL_FILES)
        if (!g_files[s] || g_files[s] == (gl_file *)1) { g_files[s] = g; break; }
    pthread_mutex_unlock(&g_files_mu);
}
static void gl_untrack(gl_file *g) {
    pthread_mutex_lock(&g_files_mu);
    unsigned s = gl_slot(g);
    for (unsigned k = 0; k < GL_FILES && g_files[s]; k++, s = (s + 1) % GL_FILES)
        if (g_files[s] == g) { g_files[s] = (gl_file *)1; break; }   // tombstone
    pthread_mutex_unlock(&g_files_mu);
}
static FILE *gl_resolve(void *p) {
    if (p == &g_stdin)  return stdin;
    if (p == &g_stdout) return stdout;
    if (p == &g_stderr) return stderr;
    FILE *h = NULL;
    pthread_mutex_lock(&g_files_mu);
    unsigned s = gl_slot(p);
    for (unsigned k = 0; k < GL_FILES && g_files[s]; k++, s = (s + 1) % GL_FILES)
        if (g_files[s] == p) { h = ((gl_file *)p)->host; break; }
    pthread_mutex_unlock(&g_files_mu);
    return h;
}
static FILE *gl_host(void *p) { return kl_host_file(p); }   // resolver installed below

static void *gl_wrap(FILE *h, const char *mode) {
    if (!h) return NULL;
    gl_file *g = calloc(1, sizeof *g);
    if (!g) { fclose(h); errno = ENOMEM; return NULL; }
    uint32_t fl = 0;
    if (mode && mode[0] == 'r' && !strchr(mode, '+')) fl |= GL_IO_NO_WRITES;
    if (mode && (mode[0] == 'w' || mode[0] == 'a') && !strchr(mode, '+')) fl |= GL_IO_NO_READS;
    gl_file_init(g, h, fileno(h), fl);
    gl_track(g);
    return g;
}
static FILE *gl_unwrap_close(void *p) {
    FILE *h = gl_resolve(p);
    if (h && p != &g_stdin && p != &g_stdout && p != &g_stderr) {
        gl_untrack(p);
        free(p);
    }
    return h;
}

static void *gl_fopen(const char *path, const char *mode) {
    BIONIC("fopen", void *(*)(const char *, const char *));
    // A failed open stays NULL. kl_host_file() answers NULL with the runtime's
    // bogus-FILE sink (round 4, for a guest that READS through a NULL FILE *),
    // so asking it first turned every failed fopen into a "success" whose
    // fseek/ftell then fail EBADF — V8 sized a missing icudtl.dat as -1 and
    // died in new[SIZE_MAX] (round 5).
    void *raw = _b(path, mode);
    void *r = raw ? gl_wrap(kl_host_file(raw), mode) : NULL;
    // KL_GLIBC_TRACE_FOPEN=<substring> ("" = every open): what a guest opened
    // through the personality, and what it got.
    const char *t = getenv("KL_GLIBC_TRACE_FOPEN");
    if (t && path && strstr(path, t))
        fprintf(stderr, "  [glibc] fopen(\"%s\", \"%s\") -> %p (host FILE %p, fd %d)\n", path,
                mode ? mode : "", r, r ? (void *)gl_host(r) : NULL,
                r && gl_host(r) ? fileno(gl_host(r)) : -1);
    return r;
}
// A failing seek/tell is named (the first 20): a guest sizing a file with
// fseek(END)+ftell turns -1 into new[SIZE_MAX] (round 5: V8's ICU data).
static void gl_seek_failed(const char *op, void *f, long r) {
    static int n;
    if (n++ >= 20) return;
    FILE *h = gl_host(f);
    fprintf(stderr, "  [glibc] %s(%p -> host %p, fd %d) -> %ld errno %d\n", op, f, (void *)h,
            h ? fileno(h) : -1, r, errno);
}
static void *gl_fdopen(int fd, const char *mode) { return gl_wrap(fdopen(fd, mode), mode); }
static void *gl_freopen(const char *path, const char *mode, void *fp) {
    FILE *h = gl_resolve(fp);
    if (!h) { errno = EBADF; return NULL; }
    char buf[1024];
    const char *hp = path ? kl_guest_path(path, buf, sizeof buf) : NULL;
    FILE *n;
    if (h == stderr || h == stdout) {
        // The host's stdout/stderr are this runtime's log too: reopening them
        // (tier0 freopen64s its standard streams) would take every later
        // runtime line — fault reports included — with them. The guest's
        // stream is pointed at a fresh FILE instead; the host's is untouched.
        n = hp ? fopen(hp, mode) : NULL;
        fprintf(stderr, "  [glibc] freopen(%s, \"%s\", %s) — the guest's stream now "
                        "writes %s; the runtime's %s is left alone\n", path ? path : "NULL",
                mode, h == stderr ? "stderr" : "stdout", n ? hp : "(open failed)",
                h == stderr ? "stderr" : "stdout");
    } else
        n = freopen(hp, mode, h);
    if (!n) return NULL;
    ((gl_file *)fp)->host = n;
    *(int32_t *)(((gl_file *)fp)->io + 0x70) = fileno(n);
    return fp;
}
static void *gl_popen(const char *cmd, const char *mode) {
    BIONIC("popen", void *(*)(const char *, const char *));
    void *raw = _b(cmd, mode);                 // NULL stays NULL (see gl_fopen)
    return raw ? gl_wrap(kl_host_file(raw), mode) : NULL;
}
static int gl_pclose(void *fp) { FILE *h = gl_unwrap_close(fp); return h ? pclose(h) : -1; }
static void *gl_tmpfile(void) { return gl_wrap(tmpfile(), "w+"); }
static void *gl_open_memstream(char **buf, size_t *len) { return gl_wrap(open_memstream(buf, len), "w"); }
// The host's standard streams are also this runtime's log (see gl_setvbuf):
// a guest that closes one — fclose(stderr), close(2) — frees the descriptor,
// the next socket()/open() takes it, and every later runtime line (fault
// reports included) goes into that socket. Closing them is declined, once per
// stream, by name; the guest is told it worked.
static int gl_std_close_declined(int fd, const char *how) {
    if (fd < 0 || fd > 2) return 0;
    static int said[3];
    if (!said[fd]++)
        fprintf(stderr, "  [glibc] %s of fd %d declined — the runtime's standard "
                        "streams stay open (its log lives there)\n", how, fd);
    return 1;
}
static int gl_fclose(void *fp) {
    FILE *h = gl_resolve(fp);
    if (h && (h == stdin || h == stdout || h == stderr) &&
        gl_std_close_declined(fileno(h), "fclose")) { fflush(h); return 0; }
    h = gl_unwrap_close(fp);
    if (!h) { errno = EBADF; return EOF; }
    return fclose(h);
}
static int gl_close(int fd) {
    if (gl_std_close_declined(fd, "close")) return 0;
    BIONIC("close", int (*)(int));
    return _b(fd);
}
static void   gl_clearerr(void *f)                 { clearerr(gl_host(f)); }
static int    gl_feof(void *f)                     { return feof(gl_host(f)); }
static int    gl_ferror(void *f)                   { return ferror(gl_host(f)); }
static int    gl_fgetc(void *f)                    { return fgetc(gl_host(f)); }
static char  *gl_fgets(char *s, int n, void *f)    { return fgets(s, n, gl_host(f)); }
static int    gl_fileno(void *f)                   { return fileno(gl_host(f)); }
static void   gl_flockfile(void *f)                { flockfile(gl_host(f)); }
static void   gl_funlockfile(void *f)              { funlockfile(gl_host(f)); }
static int    gl_fputc(int c, void *f)             { return fputc(c, gl_host(f)); }
static wint_t gl_fputwc(wchar_t c, void *f)        { return fputwc(c, gl_host(f)); }
static size_t gl_fread(void *p, size_t a, size_t b, void *f) { return fread(p, a, b, gl_host(f)); }
static int    gl_fseek(void *f, long o, int w)     {
    int r = fseek(gl_host(f), o, w);
    if (r < 0) gl_seek_failed("fseek", f, r);
    return r;
}
static int    gl_fseeko(void *f, off_t o, int w)   { return fseeko(gl_host(f), o, w); }
static long   gl_ftell(void *f)                    {
    long r = ftell(gl_host(f));
    if (r < 0) gl_seek_failed("ftell", f, r);
    return r;
}
static off_t  gl_ftello(void *f)                   { return ftello(gl_host(f)); }
static int    gl_getc(void *f)                     { return getc(gl_host(f)); }
static wint_t gl_getwc(void *f)                    { return getwc(gl_host(f)); }
static int    gl_putc(int c, void *f)              { return putc(c, gl_host(f)); }
static wint_t gl_putwc(wchar_t c, void *f)         { return putwc(c, gl_host(f)); }
static int    gl_setvbuf(void *f, char *b, int m, size_t n);
static void   gl_setbuf(void *f, char *b)          { gl_setvbuf(f, b, b ? _IOFBF : _IONBF, BUFSIZ); }
static int    gl_setvbuf(void *f, char *b, int m, size_t n) {
    // _IOFBF/_IOLBF/_IONBF are 0/1/2 in glibc and 0/1/2 in Darwin.
    // The host's stderr is ALSO this runtime's log: a guest that buffers it
    // (Valve's tier0 does) makes every later runtime line — a fault report
    // included — sit in a buffer the crash then throws away. Its buffering is
    // left alone, and said so once.
    FILE *h = gl_host(f);
    if (h == stderr && (m != _IONBF || b)) {
        static int said;
        if (!said++) fprintf(stderr, "  [glibc] setvbuf(stderr, mode %d, %zu) — declined: "
                                     "stderr stays unbuffered (it is the runtime's log)\n", m, n);
        return 0;
    }
    return setvbuf(h, b, m, n);
}
static int    gl_ungetc(int c, void *f)            { return ungetc(c, gl_host(f)); }
static wint_t gl_ungetwc(wint_t c, void *f)        { return ungetwc(c, gl_host(f)); }
static char  *gl_fgets_unlocked(char *s, int n, void *f) { return fgets(s, n, gl_host(f)); }
static ssize_t gl_getdelim(char **l, size_t *n, int d, void *f) { return getdelim(l, n, d, gl_host(f)); }
// __uflow: the slow path of getc_unlocked — one character, or EOF.
static int    gl_uflow(void *f)                    { return getc(gl_host(f)); }
// __fpending: bytes buffered and not yet written. Darwin does not expose its
// count, so the buffer is flushed first — after which the true answer is 0.
static size_t gl_fpending(void *f)                 { fflush(gl_host(f)); return 0; }
// __fsetlocking: Darwin stdio always locks internally (FSETLOCKING_INTERNAL, 1).
static int    gl_fsetlocking(void *f, int type)    { (void)f; (void)type; return 1; }

// ---------------------------------------------------------------- printf family
//
// The fortified entry points. Text for stdout/stderr goes to the console hook
// (the Alyx host's own console, see kl_glibc_set_console) so it lands where the
// guest's printf/fprintf — which the host intercepts — already lands.
static int gl_vfprintf_to(void *fp, const char *fmt, kl_va *va) {
    if (g_console && (fp == &g_stdout || fp == &g_stderr)) return g_console(fp, fmt, va);
    char m[512] __attribute__((aligned(16)));
    if (kl_va_marshal(fmt, va, m, sizeof m, KL_VA_PRINTF) == (size_t)-1) return -1;
    return vfprintf(gl_host(fp), fmt, (va_list)m);
}
int klh_gl_printf_chk(int flag, const char *fmt, kl_va *va) {
    (void)flag;
    return gl_vfprintf_to(&g_stdout, fmt, va);
}
int klh_gl_fprintf_chk(void *fp, int flag, const char *fmt, kl_va *va) {
    (void)flag;
    return gl_vfprintf_to(fp, fmt, va);
}
int klh_gl_asprintf_chk(char **out, int flag, const char *fmt, kl_va *va) {
    (void)flag;
    char m[512] __attribute__((aligned(16)));
    if (kl_va_marshal(fmt, va, m, sizeof m, KL_VA_PRINTF) == (size_t)-1) return -1;
    return vasprintf(out, fmt, (va_list)m);
}
int klh_gl_syslog_chk(int pri, int flag, const char *fmt, kl_va *va) {
    (void)flag;
    char line[1024], m[512] __attribute__((aligned(16)));
    if (kl_va_marshal(fmt, va, m, sizeof m, KL_VA_PRINTF) == (size_t)-1) return -1;
    vsnprintf(line, sizeof line, fmt, (va_list)m);
    fprintf(stderr, "  [glibc] syslog(%d): %s\n", pri, line);
    return 0;
}
// __vfprintf_chk(fp, flag, fmt, va_list): the AAPCS64 va_list arrives by
// reference, which is exactly a kl_va *.
static int gl_vfprintf_chk(void *fp, int flag, const char *fmt, kl_va *va) {
    (void)flag;
    return gl_vfprintf_to(fp, fmt, va);
}
static int gl_vasprintf_chk(char **out, int flag, const char *fmt, kl_va *va) {
    (void)flag;
    char m[512] __attribute__((aligned(16)));
    if (kl_va_marshal(fmt, va, m, sizeof m, KL_VA_PRINTF) == (size_t)-1) return -1;
    return vasprintf(out, fmt, (va_list)m);
}
extern int klv___printf_chk(void), klv___fprintf_chk(void), klv___asprintf_chk(void),
           klv___syslog_chk(void);

// __isoc23_vfscanf(fp, fmt, va_list): the scanf family's va_list form, which
// the bionic table has no entry for.
static int gl_vfscanf(void *fp, const char *fmt, kl_va *va) {
    char m[512] __attribute__((aligned(16)));
    if (kl_va_marshal(fmt, va, m, sizeof m, KL_VA_SCANF) == (size_t)-1) return -1;
    return vfscanf(gl_host(fp), fmt, (va_list)m);
}
// qsort_r: glibc's comparator takes the context LAST, BSD's (Darwin's) FIRST,
// and the two put the context argument in different places too.
typedef struct { int (*cmp)(const void *, const void *, void *); void *arg; } gl_qsr;
static int gl_qsr_tramp(void *c, const void *a, const void *b) {
    gl_qsr *q = c; return q->cmp(a, b, q->arg);
}
static void gl_qsort_r(void *base, size_t n, size_t sz,
                       int (*cmp)(const void *, const void *, void *), void *arg) {
    gl_qsr q = { cmp, arg };
    qsort_r(base, n, sz, &q, gl_qsr_tramp);
}
static int gl_pthread_getaffinity_np(pthread_t t, size_t sz, void *set) {
    (void)t;
    BIONIC("sched_getaffinity", int (*)(int, size_t, void *));
    return _b(0, sz, set) ? errno : 0;
}
static int gl_fflush(void *f) { return fflush(f ? gl_host(f) : NULL); }
static size_t gl_fwrite(const void *p, size_t a, size_t b, void *f) { return fwrite(p, a, b, gl_host(f)); }
static int gl_fputs(const char *str, void *f) { return fputs(str, gl_host(f)); }
static void gl_rewind(void *f) { rewind(gl_host(f)); }

// ---------------------------------------------------------------- signals
//
// glibc aarch64: { handler; sigset_t sa_mask (128 bytes); int sa_flags; restorer }.
// bionic LP64 (what kl_libc's sigaction speaks): { int flags; pad; handler;
// unsigned long mask; restorer }. The first 64 signals are the first word of
// either mask; nothing past it has a Darwin number.
typedef struct { void *handler; uint64_t mask[16]; int32_t flags; int32_t _pad; void *restorer; } gl_sigaction_t;
typedef struct { int32_t flags; int32_t _pad; void *handler; uint64_t mask; void *restorer; } gl_bionic_sigaction_t;
static int gl_sigaction(int sig, const gl_sigaction_t *in, gl_sigaction_t *old) {
    BIONIC("sigaction", int (*)(int, const gl_bionic_sigaction_t *, gl_bionic_sigaction_t *));
    gl_bionic_sigaction_t bi, bo;
    if (in) {
        memset(&bi, 0, sizeof bi);
        bi.handler = in->handler; bi.flags = in->flags; bi.mask = in->mask[0];
        bi.restorer = in->restorer;
    }
    int r = _b(sig, in ? &bi : NULL, old ? &bo : NULL);
    if (!r && old) {
        memset(old, 0, sizeof *old);
        old->handler = bo.handler; old->flags = bo.flags; old->mask[0] = bo.mask;
        old->restorer = bo.restorer;
    }
    return r;
}
// Linux stack_t is { ss_sp; int ss_flags; size_t ss_size } and SS_DISABLE is 2;
// Darwin's is { ss_sp; size_t ss_size; int ss_flags } and SS_DISABLE is 4.
typedef struct { void *sp; int32_t flags; int32_t _pad; size_t size; } gl_stack_t;
static int gl_ss_to_darwin(int f) { return (f & 1 ? SS_ONSTACK : 0) | (f & 2 ? SS_DISABLE : 0); }
static int gl_ss_from_darwin(int f) { return (f & SS_ONSTACK ? 1 : 0) | (f & SS_DISABLE ? 2 : 0); }
static int gl_sigaltstack(const gl_stack_t *in, gl_stack_t *old) {
    stack_t d, o;
    if (in) { d.ss_sp = in->sp; d.ss_size = in->size; d.ss_flags = gl_ss_to_darwin(in->flags); }
    int r = sigaltstack(in ? &d : NULL, old ? &o : NULL);
    if (!r && old) {
        memset(old, 0, sizeof *old);
        old->sp = o.ss_sp; old->size = o.ss_size; old->flags = gl_ss_from_darwin(o.ss_flags);
    }
    return r;
}

// ---------------------------------------------------------------- netdb
//
// glibc's struct addrinfo: { flags, family, socktype, protocol, addrlen,
// ai_addr, ai_canonname, ai_next } — addr and canonname the other way round
// from bionic/Darwin. The list comes back from the bionic answer (which also
// converts each sockaddr to the Linux shape) and is swapped node by node in
// place; freeaddrinfo swaps it back before handing it to the allocator that
// made it.
typedef struct gl_ai { int flags, family, socktype, protocol; uint32_t addrlen; uint32_t _pad;
                       void *p1, *p2; struct gl_ai *next; } gl_ai;
static void gl_ai_swap(gl_ai *a) {
    for (; a; a = a->next) { void *t = a->p1; a->p1 = a->p2; a->p2 = t; }
}
static int gl_getaddrinfo(const char *node, const char *serv, const gl_ai *hints, gl_ai **res) {
    BIONIC("getaddrinfo", int (*)(const char *, const char *, const void *, gl_ai **));
    gl_ai h;
    if (hints) {
        h = *hints; h.next = NULL;
        void *t = h.p1; h.p1 = h.p2; h.p2 = t;
        // AF_INET6 is 10 on Linux and 30 on Darwin; the hint goes to Darwin.
        if (h.family == 10) h.family = AF_INET6;
    }
    int r = _b(node, serv, hints ? &h : NULL, res);
    if (!r && res) gl_ai_swap(*res);
    return r;
}
static void gl_freeaddrinfo(gl_ai *a) {
    gl_ai_swap(a);
    freeaddrinfo((struct addrinfo *)a);
}
static int *gl_h_errno_location(void) { return &h_errno; }
static void gl_res_nclose(void *state) { (void)state; }   // nothing was opened

// ---------------------------------------------------------------- strings
static char *gl_strerror_r(int e, char *buf, size_t n) {
    // GNU: returns a string, which may or may not be `buf`.
    BIONIC("strerror", char *(*)(int));
    const char *s = _b(e);
    if (buf && n) { snprintf(buf, n, "%s", s ? s : "Unknown error"); return buf; }
    return (char *)s;
}
static int gl_xpg_strerror_r(int e, char *buf, size_t n) {
    return strerror_r(kl_errno_from_linux(e), buf, n) ? 34 /* ERANGE */ : 0;
}
static char *gl_xpg_basename(char *p) { return basename(p); }
static int gl_bcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
// strverscmp: glibc's version ordering — runs of digits compare as numbers,
// with leading zeros meaning a fraction (so "1.010" < "1.09"). The state
// machine is glibc's own (string/strverscmp.c), transcribed.
static int gl_strverscmp(const char *s1, const char *s2) {
    const unsigned char *p1 = (const unsigned char *)s1, *p2 = (const unsigned char *)s2;
    enum { S_N = 0x0, S_I = 0x3, S_F = 0x6, S_Z = 0x9 };
    enum { CMP = 2, LEN = 3 };
    static const uint8_t next_state[] = {
        /* S_N */ S_N, S_I, S_Z,
        /* S_I */ S_N, S_I, S_I,
        /* S_F */ S_N, S_F, S_F,
        /* S_Z */ S_N, S_F, S_Z };
    static const int8_t result_type[] = {
        CMP, CMP, CMP, CMP, LEN, CMP, CMP, CMP, CMP,
        CMP, -1,  -1,  +1,  LEN, LEN, +1,  LEN, LEN,
        CMP, CMP, CMP, CMP, CMP, CMP, CMP, CMP, CMP,
        CMP, +1,  +1,  -1,  CMP, CMP, -1,  CMP, CMP };
    if (p1 == p2) return 0;
    unsigned char c1 = *p1++, c2 = *p2++;
    int state = S_N + ((c1 == '0') + (isdigit(c1) != 0));
    int diff;
    while ((diff = c1 - c2) == 0) {
        if (c1 == '\0') return diff;
        state = next_state[state];
        c1 = *p1++; c2 = *p2++;
        state += (c1 == '0') + (isdigit(c1) != 0);
    }
    state = result_type[state * 3 + (((c2 == '0') + (isdigit(c2) != 0)))];
    switch (state) {
    case CMP: return diff;
    case LEN:
        while (isdigit(*p1++)) if (!isdigit(*p2++)) return 1;
        return isdigit(*p2) ? -1 : diff;
    default: return state;
    }
}
static int gl_versionsort(const struct dirent **a, const struct dirent **b) {
    return gl_strverscmp((*a)->d_name, (*b)->d_name);
}
static char *gl_secure_getenv(const char *n) {
    BIONIC("getenv", char *(*)(const char *));
    return _b(n);                 // not setuid: secure_getenv is getenv
}

// ---------------------------------------------------------------- stat family
static int gl_xstat(int ver, const char *p, void *st) {
    (void)ver; BIONIC("stat", int (*)(const char *, void *)); return _b(p, st);
}
static int gl_lxstat(int ver, const char *p, void *st) {
    (void)ver; BIONIC("lstat", int (*)(const char *, void *)); return _b(p, st);
}
static int gl_fxstat(int ver, int fd, void *st) {
    (void)ver; BIONIC("fstat", int (*)(int, void *)); return _b(fd, st);
}
// Linux struct statfs (asm-generic, 64-bit): f_type, f_bsize, f_blocks,
// f_bfree, f_bavail, f_files, f_ffree (8 bytes each), f_fsid (2 ints),
// f_namelen, f_frsize, f_flags, f_spare[4].
typedef struct { int64_t type, bsize; uint64_t blocks, bfree, bavail, files, ffree;
                 int32_t fsid[2]; int64_t namelen, frsize, flags, spare[4]; } gl_statfs;
static int gl_fstatfs(int fd, gl_statfs *o) {
    struct statfs s;
    if (fstatfs(fd, &s) != 0) return -1;
    memset(o, 0, sizeof *o);
    o->type = 0xEF53;                 // what the payload expects of a local disk: ext4's magic
    o->bsize = s.f_bsize; o->frsize = s.f_bsize;
    o->blocks = s.f_blocks; o->bfree = s.f_bfree; o->bavail = s.f_bavail;
    o->files = s.f_files; o->ffree = s.f_ffree;
    o->fsid[0] = s.f_fsid.val[0]; o->fsid[1] = s.f_fsid.val[1];
    o->namelen = 255;
    return 0;
}
static int gl_creat(const char *p, unsigned mode) {
    // O_CREAT|O_WRONLY|O_TRUNC in Linux numbering; the bionic open is the
    // variadic thunk, called here with its three arguments in registers —
    // exactly the AAPCS64 shape it expects.
    BIONIC("open", int (*)(const char *, int, unsigned));
    return _b(p, 0100 | 01 | 01000, mode);
}
static int gl_openat_2(int dirfd, const char *p, int flags) {
    BIONIC("openat", int (*)(int, const char *, int, unsigned));
    return _b(dirfd, p, flags, 0);
}
static int gl_mkostemps(char *tmpl, int suffixlen, int linux_flags) {
    char buf[1024];
    const char *hp = kl_guest_path(tmpl, buf, sizeof buf);
    int d = kl_open_flags(linux_flags) & (O_CLOEXEC | O_APPEND | O_SYNC);
    int fd;
    if (hp != tmpl) {
        // The template names a guest path: make the file at the host path and
        // write the chosen name back into the guest's template, suffix intact.
        char host[1024];
        snprintf(host, sizeof host, "%s", hp);
        fd = mkostemps(host, suffixlen, d);
        if (fd >= 0) {
            size_t tl = strlen(tmpl), hl = strlen(host);
            if (hl >= tl) memcpy(tmpl, host + hl - tl, tl);
        }
    } else {
        fd = mkostemps(tmpl, suffixlen, d);
    }
    return fd;
}
static int gl_mkostemp(char *t, int f) { return gl_mkostemps(t, 0, f); }
static int gl_posix_fadvise(int fd, off_t o, off_t l, int a) { (void)fd; (void)o; (void)l; (void)a; return 0; }
static int gl_posix_madvise(void *p, size_t l, int a) { (void)p; (void)l; (void)a; return 0; }
static int gl_posix_fallocate(int fd, off_t off, off_t len) {
    // posix_fallocate guarantees the range is backed; on Darwin, extending the
    // file is what guarantees a later write there cannot fail for want of size.
    struct stat st;
    if (fstat(fd, &st) != 0) return errno == EBADF ? 9 : 5;
    if (off + len > st.st_size && ftruncate(fd, off + len) != 0) return 28; // ENOSPC
    return 0;
}

// ---------------------------------------------------------------- locale `_l`
// The locale argument is a HOST-library object (see the header); these answer
// on the process locale, which is the "C" locale the payload runs in.
static int    gl_iswalpha_l(wint_t c, void *l)   { (void)l; return iswalpha(c); }
static int    gl_iswblank_l(wint_t c, void *l)   { (void)l; return iswblank(c); }
static int    gl_iswcntrl_l(wint_t c, void *l)   { (void)l; return iswcntrl(c); }
static int    gl_iswdigit_l(wint_t c, void *l)   { (void)l; return iswdigit(c); }
static int    gl_iswlower_l(wint_t c, void *l)   { (void)l; return iswlower(c); }
static int    gl_iswprint_l(wint_t c, void *l)   { (void)l; return iswprint(c); }
static int    gl_iswpunct_l(wint_t c, void *l)   { (void)l; return iswpunct(c); }
static int    gl_iswspace_l(wint_t c, void *l)   { (void)l; return iswspace(c); }
static int    gl_iswupper_l(wint_t c, void *l)   { (void)l; return iswupper(c); }
static int    gl_iswxdigit_l(wint_t c, void *l)  { (void)l; return iswxdigit(c); }
static int    gl_tolower_l(int c, void *l)       { (void)l; return tolower(c); }
static int    gl_toupper_l(int c, void *l)       { (void)l; return toupper(c); }
static wint_t gl_towlower_l(wint_t c, void *l)   { (void)l; return towlower(c); }
static wint_t gl_towupper_l(wint_t c, void *l)   { (void)l; return towupper(c); }
static int    gl_strcoll_l(const char *a, const char *b, void *l) { (void)l; return strcoll(a, b); }
static size_t gl_strxfrm_l(char *d, const char *s, size_t n, void *l) { (void)l; return strxfrm(d, s, n); }
static int    gl_wcscoll_l(const wchar_t *a, const wchar_t *b, void *l) { (void)l; return wcscoll(a, b); }
static size_t gl_wcsxfrm_l(wchar_t *d, const wchar_t *s, size_t n, void *l) { (void)l; return wcsxfrm(d, s, n); }
static double gl_strtod_l(const char *s, char **e, void *l) { (void)l; return strtod(s, e); }
static float  gl_strtof_l(const char *s, char **e, void *l) { (void)l; return strtof(s, e); }
static long long gl_strtoll_l(const char *s, char **e, int b, void *l) { (void)l; return strtoll(s, e, b); }
static unsigned long long gl_strtoull_l(const char *s, char **e, int b, void *l) { (void)l; return strtoull(s, e, b); }
// long double is IEEE quad on aarch64 Linux and double on Darwin: the bionic
// table's strtold (a register-level conversion) is the only right answer.
static void *gl_strtold_l_addr(void) { return kl_shim_lookup("strtold"); }
static size_t gl_strftime_l(char *s, size_t n, const char *f, const void *tm, void *l) {
    (void)l; return strftime(s, n, f, tm);
}
static size_t gl_wcsftime_l(wchar_t *s, size_t n, const wchar_t *f, const void *tm, void *l) {
    (void)l; return wcsftime(s, n, f, tm);
}
static char *gl_nl_langinfo(int item);
static char *gl_nl_langinfo_l(int item, void *l) { (void)l; return gl_nl_langinfo(item); }
// wctype_t: whatever __wctype_l returns, __iswctype_l is given back — both are
// Darwin's here, so the pair agrees.
static unsigned long gl_wctype_l(const char *name, void *l) { (void)l; return wctype(name); }
static int gl_iswctype_l(wint_t c, unsigned long t, void *l) { (void)l; return iswctype(c, (wctype_t)t); }
static size_t gl_mbrlen(const char *s, size_t n, mbstate_t *ps) { return mbrlen(s, n, ps); }

// ---------------------------------------------------------------- locale
//
// glibc's locale objects are not opaque to their callers either. libstdc++'s
// "gnu" locale model reads three fields of a `__locale_t` directly —
// __ctype_b, __ctype_tolower, __ctype_toupper — and the ctype macros read
// through __ctype_b_loc(). So the objects handed out here are glibc-shaped:
//
//   struct __locale_struct { struct __locale_data *__locales[13];   +0x00
//                            const unsigned short *__ctype_b;       +0x68
//                            const int *__ctype_tolower;            +0x70
//                            const int *__ctype_toupper;            +0x78
//                            const char *__names[13]; }             +0x80
//
// with the "C" locale's tables (the only locale the payload's glibc could
// load on the Quest either: it ships no locale archive). A name other than
// C / POSIX / C.UTF-8 / "" gets glibc's own answer for a locale it does not
// have: NULL and ENOENT.
//
// The jailbreak host has its own versions of all of these, and they reach
// INTO Valve's libc.so.6 (its static-TLS locale pointers) — which is never
// loaded here. The Alyx door therefore routes the host's AlyxGuestCtype* /
// AlyxGuest*Locale / Strftime shims to these.
#define GL_ISbit(b) ((b) < 8 ? ((1 << (b)) << 8) : ((1 << (b)) >> 8))
enum { GL_upper = GL_ISbit(0), GL_lower = GL_ISbit(1), GL_alpha = GL_ISbit(2),
       GL_digit = GL_ISbit(3), GL_xdigit = GL_ISbit(4), GL_space = GL_ISbit(5),
       GL_print = GL_ISbit(6), GL_graph = GL_ISbit(7), GL_blank = GL_ISbit(8),
       GL_cntrl = GL_ISbit(9), GL_punct = GL_ISbit(10), GL_alnum = GL_ISbit(11) };
static uint16_t g_ctype_b[384];
static int32_t  g_ctype_lower[384], g_ctype_upper[384];
static const uint16_t *g_ctype_b_p = g_ctype_b + 128;
static const int32_t  *g_ctype_lower_p = g_ctype_lower + 128, *g_ctype_upper_p = g_ctype_upper + 128;

struct gl_locale {
    void *locales[13];
    const uint16_t *ctype_b;
    const int32_t *ctype_tolower, *ctype_toupper;
    const char *names[13];
};
static uint8_t          g_locale_data[1024];      // what __locales[] point at: never read here
static struct gl_locale g_c_locale;
static __thread struct gl_locale *t_locale;       // NULL: the global locale
#define GL_GLOBAL_LOCALE ((struct gl_locale *)-1)

static void gl_ctype_init_tables(void) {
    for (int i = -128; i < 256; i++) {
        int c = i & 0xff;               // -128..-1 alias 128..255 (EOF is -1 = 255's slot... in C, 0)
        uint16_t m = 0;
        if (i >= 0 && i < 128) {
            if (isupper(c)) m |= GL_upper;
            if (islower(c)) m |= GL_lower;
            if (isalpha(c)) m |= GL_alpha;
            if (isdigit(c)) m |= GL_digit;
            if (isxdigit(c)) m |= GL_xdigit;
            if (isspace(c)) m |= GL_space;
            if (isprint(c)) m |= GL_print;
            if (isgraph(c)) m |= GL_graph;
            if (c == ' ' || c == '\t') m |= GL_blank;
            if (iscntrl(c)) m |= GL_cntrl;
            if (ispunct(c)) m |= GL_punct;
            if (isalnum(c)) m |= GL_alnum;
        }
        g_ctype_b[i + 128] = m;
        // glibc's C tables map the high half (and EOF) to themselves.
        g_ctype_lower[i + 128] = (i >= 'A' && i <= 'Z') ? i + 32 : i;
        g_ctype_upper[i + 128] = (i >= 'a' && i <= 'z') ? i - 32 : i;
    }
    for (int k = 0; k < 13; k++) { g_c_locale.locales[k] = g_locale_data; g_c_locale.names[k] = "C"; }
    g_c_locale.ctype_b = g_ctype_b_p;
    g_c_locale.ctype_tolower = g_ctype_lower_p;
    g_c_locale.ctype_toupper = g_ctype_upper_p;
}
static const uint16_t **gl_ctype_b_loc(void)       { return &g_ctype_b_p; }
static const int32_t  **gl_ctype_tolower_loc(void) { return &g_ctype_lower_p; }
static const int32_t  **gl_ctype_toupper_loc(void) { return &g_ctype_upper_p; }
static size_t gl_ctype_get_mb_cur_max(void)        { return 1; }   // the C locale
static void   gl_ctype_init(void)                  { }
static int gl_locale_known(const char *n) {
    return !n || !*n || !strcmp(n, "C") || !strcmp(n, "POSIX") ||
           !strcasecmp(n, "C.UTF-8") || !strcasecmp(n, "C.utf8");
}
static void *gl_newlocale(int mask, const char *name, void *base) {
    (void)mask;
    if (!gl_locale_known(name)) { kl_errno_set(ENOENT); return NULL; }
    struct gl_locale *l = base && base != GL_GLOBAL_LOCALE && base != &g_c_locale ? base : NULL;
    if (!l) { l = malloc(sizeof *l); if (!l) { kl_errno_set(ENOMEM); return NULL; } }
    *l = g_c_locale;
    return l;
}
static void *gl_duplocale(void *l) {
    struct gl_locale *d = malloc(sizeof *d);
    if (!d) { kl_errno_set(ENOMEM); return NULL; }
    *d = (l && l != GL_GLOBAL_LOCALE) ? *(struct gl_locale *)l : g_c_locale;
    return d;
}
static void gl_freelocale(void *l) {
    if (l && l != GL_GLOBAL_LOCALE && l != &g_c_locale) free(l);
}
static void *gl_uselocale(void *l) {
    struct gl_locale *old = t_locale ? t_locale : GL_GLOBAL_LOCALE;
    if (l) t_locale = l == GL_GLOBAL_LOCALE ? NULL : l;
    return old;
}
// nl_langinfo in glibc's numbering ((category << 16) | index), answered for
// the C locale. An item this does not know gets "", which is glibc's own
// answer for an invalid item.
static const char *const gl_day[7]   = { "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday" };
static const char *const gl_abday[7] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
static const char *const gl_mon[12]  = { "January","February","March","April","May","June","July",
                                         "August","September","October","November","December" };
static const char *const gl_abmon[12] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
static char *gl_nl_langinfo(int item) {
    int cat = item >> 16, idx = item & 0xffff;
    switch (cat) {
    case 0:  // LC_CTYPE
        if (idx == 14) return (char *)"ANSI_X3.4-1968";            // CODESET
        break;
    case 1:  // LC_NUMERIC
        if (idx == 0) return (char *)".";                          // RADIXCHAR
        if (idx == 1) return (char *)"";                           // THOUSEP
        if (idx == 2) return (char *)"";                           // GROUPING
        break;
    case 2:  // LC_TIME
        if (idx < 7)  return (char *)gl_abday[idx];
        if (idx < 14) return (char *)gl_day[idx - 7];
        if (idx < 26) return (char *)gl_abmon[idx - 14];
        if (idx < 38) return (char *)gl_mon[idx - 26];
        switch (idx) {
        case 38: return (char *)"AM";
        case 39: return (char *)"PM";
        case 40: return (char *)"%a %b %e %H:%M:%S %Y";           // D_T_FMT
        case 41: return (char *)"%m/%d/%y";                       // D_FMT
        case 42: return (char *)"%H:%M:%S";                       // T_FMT
        case 43: return (char *)"%I:%M:%S %p";                    // T_FMT_AMPM
        }
        break;
    case 4:  // LC_MONETARY
        if (idx == 15) return (char *)"-";                         // CRNCYSTR
        break;
    case 5:  // LC_MESSAGES
        if (idx == 0) return (char *)"^[yY]";
        if (idx == 1) return (char *)"^[nN]";
        break;
    }
    return (char *)"";
}
static void *gl_localeconv(void) { return localeconv(); }    // the C locale's: same values
static size_t gl_strftime(char *s, size_t n, const char *f, const struct tm *tm) { return strftime(s, n, f, tm); }
static size_t gl_wcsftime(wchar_t *s, size_t n, const wchar_t *f, const struct tm *tm) { return wcsftime(s, n, f, tm); }

// ---------------------------------------------------------------- fortify
static long gl_fdelt_chk(long d) {
    if (d < 0 || d >= 1024) { fprintf(stderr, "  [glibc] __fdelt_chk: fd %ld outside FD_SETSIZE\n", d); abort(); }
    return d / 64;
}
static void gl_explicit_bzero_chk(void *p, size_t n, size_t dl) {
    if (n > dl) { fprintf(stderr, "  [glibc] __explicit_bzero_chk overflow\n"); abort(); }
    memset_s(p, n, 0, n);
}
static size_t gl_mbstowcs_chk(wchar_t *d, const char *s, size_t n, size_t dl) {
    if (d && n > dl) { fprintf(stderr, "  [glibc] __mbstowcs_chk overflow\n"); abort(); }
    return mbstowcs(d, s, n);
}
static char *gl_realpath_chk(const char *p, char *r, size_t rl) {
    (void)rl;
    BIONIC("realpath", char *(*)(const char *, char *));
    return _b(p, r);
}
static void gl_assert_fail(const char *expr, const char *file, unsigned line, const char *fn) {
    fprintf(stderr, "  [glibc] assertion failed: %s (%s:%u, %s)\n",
            expr ? expr : "?", file ? file : "?", line, fn ? fn : "?");
    kl_fatal_prepare();
    abort();
}

// ---------------------------------------------------------------- misc
// Not through the bionic sysconf: bionic and glibc number _SC_* differently
// (glibc _SC_NPROCESSORS_ONLN is 84, bionic's 97), so a glibc caller never
// reaches it with a raw constant.
static int gl_get_nprocs(void) { return (int)sysconf(_SC_NPROCESSORS_ONLN); }
static int gl_sched_getcpu(void) { kl_errno_set(ENOSYS); return -1; }
static int gl_sched_getscheduler(int pid) { (void)pid; return 0; /* SCHED_OTHER */ }
static int gl_sched_getparam(int pid, int *prio) { (void)pid; if (prio) *prio = 0; return 0; }
static int gl_sched_setscheduler(int pid, int policy, const int *prio) {
    (void)pid; (void)prio;
    if (policy == 0) return 0;                      // SCHED_OTHER: what it already is
    kl_errno_set(EPERM);                            // what Linux says to a non-root caller
    return -1;
}
static int gl_pthread_setcanceltype(int type, int *old) {
    // Cancellation is refused (pthread_cancel below), so the type is moot:
    // it reads back DEFERRED (0) and any request is accepted.
    (void)type; if (old) *old = 0; return 0;
}
static int gl_pthread_attr_setinheritsched(void *a, int v) { (void)a; (void)v; return 0; }
static int gl_pthread_getname_np(pthread_t t, char *b, size_t n) { return pthread_getname_np(t, b, n); }
static int gl_pthread_setaffinity_np(pthread_t t, size_t sz, const void *set) {
    (void)t;
    BIONIC("sched_setaffinity", int (*)(int, size_t, const void *));
    return _b(0, sz, set) ? errno : 0;
}
static void *gl_sched_cpualloc(size_t n) { return calloc(1, ((n + 63) / 64) * 8); }
static void  gl_sched_cpufree(void *p)   { free(p); }
static void *gl_reallocarray(void *p, size_t n, size_t s) {
    size_t t;
    if (__builtin_mul_overflow(n, s, &t)) { kl_errno_set(ENOMEM); return NULL; }
    void *(*re)(void *, size_t) = (void *(*)(void *, size_t))host_or("realloc", (void *)realloc);
    return re(p, t);
}
// aligned_alloc: the host LISTS it among its intercepts but its resolver
// answers nothing for it, so without this row it bound to the first export of
// the name in the search order — Valve's libmimalloc — and the block later
// reached the host's free (Darwin's heap). Built on the host's posix_memalign,
// so it is the host's block like every other allocation here.
static void *gl_aligned_alloc(size_t a, size_t n) {
    int (*pm)(void **, size_t, size_t) =
        (int (*)(void **, size_t, size_t))host_or("posix_memalign", (void *)posix_memalign);
    if (a < sizeof(void *)) a = sizeof(void *);
    void *p = NULL;
    int e = pm(&p, a, n);
    if (e) { kl_errno_set(e); return NULL; }
    return p;
}
static void gl_malloc_stats(void) {
    fprintf(stderr, "  [glibc] malloc_stats: this allocator keeps no arena statistics\n");
}

// ---- the native rows' rebuilds (kl_glibc_native_table.h) ----
//
// sysconf. glibc numbers _SC_* by its own enum (bits/confname.h), bionic by
// another, Darwin by a third — the reason sysconf was never a stand-in row. The
// glibc name is mapped onto bionic's and answered by the bionic table's
// sysconf, so the CPU count and the free-page count come from the same place
// a bionic guest's do (kl_cpu_count, the Mach free-page count).
static long gl_sysconf(int name) {
    BIONIC("sysconf", long (*)(int))
    static const struct { int gl, bionic; } map[] = {
        {   0, 0x00 },   // _SC_ARG_MAX
        {   1, 0x05 },   // _SC_CHILD_MAX
        {   2, 0x06 },   // _SC_CLK_TCK
        {   3, 0x0a },   // _SC_NGROUPS_MAX
        {   4, 0x0b },   // _SC_OPEN_MAX
        {   5, 0x1b },   // _SC_STREAM_MAX
        {   6, 0x1c },   // _SC_TZNAME_MAX
        {  30, 0x27 },   // _SC_PAGESIZE (== _SC_PAGE_SIZE)
        {  43, 0x09 },   // _SC_LINE_MAX
        {  60, 0x26 },   // _SC_IOV_MAX
        {  70, 0x48 },   // _SC_GETPW_R_SIZE_MAX
        {  71, 0x49 },   // _SC_LOGIN_NAME_MAX
        {  75, 0x4c },   // _SC_THREAD_STACK_MIN
        {  83, 0x60 },   // _SC_NPROCESSORS_CONF
        {  84, 0x61 },   // _SC_NPROCESSORS_ONLN
        {  85, 0x62 },   // _SC_PHYS_PAGES
        {  86, 0x63 },   // _SC_AVPHYS_PAGES
        {  87, 0x25 },   // _SC_ATEXIT_MAX
        { 149, 0x64 },   // _SC_MONOTONIC_CLOCK
        { 173, 0x79 },   // _SC_SYMLOOP_MAX
        { 180, 0x6f },   // _SC_HOST_NAME_MAX
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (map[i].gl == name) return _b ? _b(map[i].bionic) : -1;
    if (name == 69) return sysconf(_SC_GETGR_R_SIZE_MAX);   // no bionic row
    fprintf(stderr, "  [glibc] sysconf: glibc name %d has no mapping — -1/EINVAL\n", name);
    kl_errno_set(EINVAL);
    return -1;
}

// ---- SysV IPC: semaphores and shared memory, emulated in-process ----
//
// visionOS and iOS ship no SysV IPC: the calls exist in libSystem, but the
// kernel answers them with SIGSYS ("Bad system call: 12"), which killed the
// first headset launch inside libsteamaudio's constructors (libtier0's named
// CThreadSyncObject semaphores). macOS still has them, which is why the Mac
// host boot and the Simulator never noticed. Alyx is ONE process, so the
// objects only have to be shared between its threads: a table of semaphore
// sets and one of shared segments, one mutex, one condvar — and the SAME code
// on every platform, so the Mac host boot keeps being the correctness oracle
// for what the headset runs. KL_GLIBC_SYSV=host (macOS only) puts the old
// passthrough to Darwin's SysV back, for A/B.
//
// The guest speaks Linux numbers throughout: IPC_CREAT 0x200, IPC_EXCL 0x400,
// IPC_NOWAIT 0x800, SEM_UNDO 0x1000 (accepted and ignored — no process ever
// exits under a live set here), SHM_RDONLY 0x1000; semctl/shmctl commands
// IPC_RMID 0 / IPC_SET 1 / IPC_STAT 2, GETPID 11 ... SETALL 17; and the
// glibc-aarch64 semid_ds (88 bytes) / shmid_ds (112 bytes) layouts.
#define GL_IPC_PRIVATE 0
#define GL_IPC_CREAT   0x200
#define GL_IPC_EXCL    0x400
#define GL_IPC_NOWAIT  0x800
#define GL_SHM_RDONLY  0x1000
#define GL_SEMVMX      32767
#define GL_SEMMSL      32000
#define GL_SEMOPM      500
#define GL_NSEMSETS    256
#define GL_NSHMSEGS    64

static pthread_mutex_t g_ipc_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_ipc_cv = PTHREAD_COND_INITIALIZER;
static unsigned        g_ipc_seq = 1;

typedef struct {
    int id;                       // 0 = free slot
    int key, mode, nsems;
    unsigned short *val;
    int *pid, *ncnt, *zcnt;
    time_t otime, ctime;
} gl_semset;
static gl_semset g_semsets[GL_NSEMSETS];

typedef struct {
    int id;                       // 0 = free slot
    int key, mode, removed, nattch, cpid, lpid;
    size_t size, maplen;
    void *addr;
    time_t atime, dtime, ctime;
} gl_shmseg;
static gl_shmseg g_shmsegs[GL_NSHMSEGS];

static int gl_sysv_host(void) {
#if TARGET_OS_OSX
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("KL_GLIBC_SYSV");
        v = e && !strcmp(e, "host");
        if (v) fprintf(stderr, "  [glibc] SysV IPC: KL_GLIBC_SYSV=host — passed through to "
                               "Darwin's SysV (the old binding; macOS only)\n");
    }
    return v;
#else
    return 0;
#endif
}

// The first few calls of each name, attributable; then quiet.
static void gl_ipc_note(atomic_int *n, const char *fmt, ...) {
    if (atomic_fetch_add(n, 1) >= 8) return;
    va_list ap; va_start(ap, fmt);
    fputs("  [glibc] ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}
static atomic_int g_n_semget, g_n_semop, g_n_semctl, g_n_shmget, g_n_shmat, g_n_shmdt, g_n_shmctl;

// An id names a slot AND a generation, so a removed set's id stops working at
// the removal instead of landing on whatever reuses the slot.
static int gl_ipc_newid(unsigned slot, unsigned nslots) {
    unsigned s = g_ipc_seq++;
    if (g_ipc_seq >= (1u << 22)) g_ipc_seq = 1;
    return (int)(s * nslots + slot);
}
static gl_semset *gl_sem_find(int id) {
    if (id <= 0) return NULL;
    gl_semset *s = &g_semsets[(unsigned)id % GL_NSEMSETS];
    return s->id == id ? s : NULL;
}
static void gl_sem_free(gl_semset *s) {
    free(s->val); free(s->pid); free(s->ncnt); free(s->zcnt);
    memset(s, 0, sizeof *s);
}

static int gl_semget(int key, int nsems, int flags) {
    if (gl_sysv_host()) return semget(key, nsems, flags);
    int err = 0, id = -1;
    pthread_mutex_lock(&g_ipc_mu);
    gl_semset *hit = NULL;
    if (key != GL_IPC_PRIVATE)
        for (unsigned i = 0; i < GL_NSEMSETS; i++)
            if (g_semsets[i].id && g_semsets[i].key == key) { hit = &g_semsets[i]; break; }
    if (hit) {
        if ((flags & GL_IPC_CREAT) && (flags & GL_IPC_EXCL)) err = EEXIST;
        else if (nsems > hit->nsems)                        err = EINVAL;
        else id = hit->id;
    } else if (key != GL_IPC_PRIVATE && !(flags & GL_IPC_CREAT)) {
        err = ENOENT;
    } else if (nsems <= 0 || nsems > GL_SEMMSL) {
        err = EINVAL;
    } else {
        unsigned i = 0;
        while (i < GL_NSEMSETS && g_semsets[i].id) i++;
        if (i == GL_NSEMSETS) err = ENOSPC;
        else {
            gl_semset *s = &g_semsets[i];
            s->val  = calloc((size_t)nsems, sizeof *s->val);
            s->pid  = calloc((size_t)nsems, sizeof *s->pid);
            s->ncnt = calloc((size_t)nsems, sizeof *s->ncnt);
            s->zcnt = calloc((size_t)nsems, sizeof *s->zcnt);
            if (!s->val || !s->pid || !s->ncnt || !s->zcnt) { gl_sem_free(s); err = ENOMEM; }
            else {
                s->key = key; s->mode = flags & 0777; s->nsems = nsems;
                s->ctime = time(NULL);
                s->id = id = gl_ipc_newid(i, GL_NSEMSETS);
            }
        }
    }
    pthread_mutex_unlock(&g_ipc_mu);
    gl_ipc_note(&g_n_semget, "semget(key 0x%x, nsems %d, flags 0%o) = %d%s%s (emulated)",
                (unsigned)key, nsems, (unsigned)flags, id, err ? " errno " : "",
                err ? strerror(err) : "");
    if (err) { kl_errno_set(err); return -1; }
    return id;
}

// Linux struct sembuf — the same 6 bytes as Darwin's, read field by field.
typedef struct { unsigned short num; short op; short flg; } gl_sembuf;

static int gl_semtimedop(int id, gl_sembuf *sops, size_t nsops, const struct timespec *rel) {
    if (gl_sysv_host()) {
        if (rel) { refused("semtimedop (KL_GLIBC_SYSV=host)"); return -1; }
        return semop(id, (struct sembuf *)sops, nsops);
    }
    if (nsops == 0 || nsops > GL_SEMOPM) { kl_errno_set(nsops ? E2BIG : EINVAL); return -1; }
    if (!sops) { kl_errno_set(EFAULT); return -1; }
    gl_ipc_note(&g_n_semop, "semop(id %d, %zu op%s: num %u op %d flg 0x%x%s%s) (emulated)",
                id, nsops, nsops == 1 ? "" : "s", sops[0].num, sops[0].op,
                (unsigned)(unsigned short)sops[0].flg, nsops > 1 ? " ..." : "",
                rel ? ", timed" : "");
    struct timespec deadline;
    if (rel) {
        struct timeval now; gettimeofday(&now, NULL);
        long ns = (long)now.tv_usec * 1000 + rel->tv_nsec;
        deadline.tv_sec = now.tv_sec + rel->tv_sec + ns / 1000000000L;
        deadline.tv_nsec = ns % 1000000000L;
    }
    int err = 0;
    pthread_mutex_lock(&g_ipc_mu);
    for (;;) {
        gl_semset *s = gl_sem_find(id);
        if (!s) { err = EINVAL; break; }
        // All the ops at once, or none: find the first one that cannot go.
        size_t i = 0;
        int blocked = -1, zero_wait = 0;
        for (i = 0; i < nsops; i++) {
            if (sops[i].num >= s->nsems) { err = EFBIG; break; }
            int v = s->val[sops[i].num];
            // Ops on the same semaphore within one call compound.
            for (size_t j = 0; j < i; j++) if (sops[j].num == sops[i].num) v += sops[j].op;
            if (sops[i].op > 0) {
                if (v + sops[i].op > GL_SEMVMX) { err = ERANGE; break; }
            } else if (sops[i].op == 0) {
                if (v != 0) { blocked = (int)i; zero_wait = 1; break; }
            } else if (v < -sops[i].op) { blocked = (int)i; break; }
        }
        if (err) break;
        if (blocked < 0) {
            int pid = getpid();
            for (i = 0; i < nsops; i++) {
                s->val[sops[i].num] = (unsigned short)(s->val[sops[i].num] + sops[i].op);
                s->pid[sops[i].num] = pid;
            }
            s->otime = time(NULL);
            pthread_cond_broadcast(&g_ipc_cv);
            break;
        }
        if (sops[blocked].flg & GL_IPC_NOWAIT) { err = EAGAIN; break; }
        unsigned short n = sops[blocked].num;
        int *cnt = zero_wait ? &s->zcnt[n] : &s->ncnt[n];
        (*cnt)++;
        int rc = rel ? pthread_cond_timedwait(&g_ipc_cv, &g_ipc_mu, &deadline)
                     : pthread_cond_wait(&g_ipc_cv, &g_ipc_mu);
        s = gl_sem_find(id);
        if (!s) { err = EIDRM; break; }       // removed while we waited
        (*(zero_wait ? &s->zcnt[n] : &s->ncnt[n]))--;
        if (rc == ETIMEDOUT) { err = EAGAIN; break; }
    }
    pthread_mutex_unlock(&g_ipc_mu);
    if (err) { kl_errno_set(err); return -1; }
    return 0;
}
static int gl_semop(int id, gl_sembuf *sops, size_t nsops) {
    return gl_semtimedop(id, sops, nsops, NULL);
}

// semctl. Variadic in both C libraries, and the two calling conventions put
// the fourth argument in different places (Linux AArch64: x3; Darwin: the
// stack) — so the guest's call is taken non-variadically here. The 4th
// argument (union semun) is the int value for SETVAL, a pointer otherwise.
static int gl_semctl_host(int id, int num, int cmd, uint64_t arg) {
    switch (cmd) {
    case 0:  return semctl(id, num, IPC_RMID);
    case 11: return semctl(id, num, GETPID);
    case 12: return semctl(id, num, GETVAL);
    case 14: return semctl(id, num, GETNCNT);
    case 15: return semctl(id, num, GETZCNT);
    case 16: return semctl(id, num, SETVAL, (int)arg);
    }
    refused("semctl (KL_GLIBC_SYSV=host: IPC_STAT/IPC_SET/GETALL/SETALL: semid_ds differs)");
    return -1;
}
static void gl_ipc_perm_fill(uint8_t *p, int key, int mode) {
    // glibc aarch64 struct ipc_perm: __key, uid, gid, cuid, cgid, mode (u32 each)
    uint32_t uid = getuid(), gid = getgid();
    uint32_t f[6] = { (uint32_t)key, uid, gid, uid, gid, (uint32_t)mode };
    memcpy(p, f, sizeof f);
}
static int gl_semctl(int id, int num, int cmd, uint64_t arg) {
    if (gl_sysv_host()) return gl_semctl_host(id, num, cmd, arg);
    static const char *const names[] = { "IPC_RMID", "IPC_SET", "IPC_STAT", "IPC_INFO",
        [11] = "GETPID", "GETVAL", "GETALL", "GETNCNT", "GETZCNT", "SETVAL", "SETALL" };
    gl_ipc_note(&g_n_semctl, "semctl(id %d, sem %d, %s) (emulated)", id, num,
                cmd >= 0 && cmd <= 17 && names[cmd] ? names[cmd] : "?");
    int err = 0, ret = 0;
    void *ptr = (void *)(uintptr_t)arg;
    pthread_mutex_lock(&g_ipc_mu);
    gl_semset *s = gl_sem_find(id);
    if (!s) err = EINVAL;
    else if ((cmd >= 11 && cmd <= 16 && cmd != 13) && (num < 0 || num >= s->nsems)) err = EINVAL;
    else switch (cmd) {
    case 0:                                       // IPC_RMID: waiters wake with EIDRM
        gl_sem_free(s);
        pthread_cond_broadcast(&g_ipc_cv);
        break;
    case 1:                                       // IPC_SET: the mode bits only
        if (!ptr) { err = EFAULT; break; }
        { uint32_t m; memcpy(&m, (uint8_t *)ptr + 20, 4); s->mode = (int)(m & 0777); }
        s->ctime = time(NULL);
        break;
    case 2: {                                     // IPC_STAT: glibc aarch64 semid_ds
        if (!ptr) { err = EFAULT; break; }
        uint8_t b[88] = {0};
        gl_ipc_perm_fill(b, s->key, s->mode);
        int64_t ot = s->otime, ct = s->ctime; uint64_t ns = (uint64_t)s->nsems;
        memcpy(b + 48, &ot, 8); memcpy(b + 56, &ct, 8); memcpy(b + 64, &ns, 8);
        memcpy(ptr, b, sizeof b);
        break;
    }
    case 11: ret = s->pid[num];  break;           // GETPID
    case 12: ret = s->val[num];  break;           // GETVAL
    case 14: ret = s->ncnt[num]; break;           // GETNCNT
    case 15: ret = s->zcnt[num]; break;           // GETZCNT
    case 13:                                      // GETALL
        if (!ptr) { err = EFAULT; break; }
        memcpy(ptr, s->val, (size_t)s->nsems * sizeof *s->val);
        break;
    case 16: {                                    // SETVAL
        int v = (int)arg;
        if (v < 0 || v > GL_SEMVMX) { err = ERANGE; break; }
        s->val[num] = (unsigned short)v;
        s->ctime = time(NULL);
        pthread_cond_broadcast(&g_ipc_cv);
        break;
    }
    case 17: {                                    // SETALL
        if (!ptr) { err = EFAULT; break; }
        const unsigned short *v = ptr;
        for (int i = 0; i < s->nsems; i++) if (v[i] > GL_SEMVMX) { err = ERANGE; break; }
        if (err) break;
        memcpy(s->val, v, (size_t)s->nsems * sizeof *s->val);
        s->ctime = time(NULL);
        pthread_cond_broadcast(&g_ipc_cv);
        break;
    }
    default:
        pthread_mutex_unlock(&g_ipc_mu);
        refused("semctl (IPC_INFO/SEM_INFO/SEM_STAT: no system-wide table to report)");
        return -1;
    }
    pthread_mutex_unlock(&g_ipc_mu);
    if (err) { kl_errno_set(err); return -1; }
    return ret;
}

// Shared memory: one anonymous mapping per segment, shared by every attach
// (every shmat of a segment answers the same address — it is one process).
static gl_shmseg *gl_shm_find(int id) {
    if (id <= 0) return NULL;
    gl_shmseg *s = &g_shmsegs[(unsigned)id % GL_NSHMSEGS];
    return s->id == id ? s : NULL;
}
static void gl_shm_release(gl_shmseg *s) {      // the lock is held
    if (s->addr) munmap(s->addr, s->maplen);
    memset(s, 0, sizeof *s);
}
static int gl_shmget(int key, size_t size, int flags) {
    if (gl_sysv_host()) return shmget(key, size, flags);
    int err = 0, id = -1;
    pthread_mutex_lock(&g_ipc_mu);
    gl_shmseg *hit = NULL;
    if (key != GL_IPC_PRIVATE)
        for (unsigned i = 0; i < GL_NSHMSEGS; i++)
            if (g_shmsegs[i].id && !g_shmsegs[i].removed && g_shmsegs[i].key == key) {
                hit = &g_shmsegs[i]; break;
            }
    if (hit) {
        if ((flags & GL_IPC_CREAT) && (flags & GL_IPC_EXCL)) err = EEXIST;
        else if (size > hit->size)                          err = EINVAL;
        else id = hit->id;
    } else if (key != GL_IPC_PRIVATE && !(flags & GL_IPC_CREAT)) {
        err = ENOENT;
    } else if (size == 0) {
        err = EINVAL;
    } else {
        unsigned i = 0;
        while (i < GL_NSHMSEGS && g_shmsegs[i].id) i++;
        if (i == GL_NSHMSEGS) err = ENOSPC;
        else {
            size_t pg = (size_t)getpagesize(), len = (size + pg - 1) & ~(pg - 1);
            void *a = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (a == MAP_FAILED) err = ENOMEM;
            else {
                gl_shmseg *s = &g_shmsegs[i];
                s->key = key; s->mode = flags & 0777; s->size = size; s->maplen = len;
                s->addr = a; s->cpid = getpid(); s->ctime = time(NULL);
                s->id = id = gl_ipc_newid(i, GL_NSHMSEGS);
            }
        }
    }
    pthread_mutex_unlock(&g_ipc_mu);
    gl_ipc_note(&g_n_shmget, "shmget(key 0x%x, size %zu, flags 0%o) = %d%s%s (emulated)",
                (unsigned)key, size, (unsigned)flags, id, err ? " errno " : "",
                err ? strerror(err) : "");
    if (err) { kl_errno_set(err); return -1; }
    return id;
}
static void *gl_shmat(int id, const void *addr, int flags) {
    if (gl_sysv_host()) return shmat(id, addr, flags);
    gl_ipc_note(&g_n_shmat, "shmat(id %d, addr %p, flags 0x%x) (emulated)", id, addr,
                (unsigned)flags);
    if (addr) {
        // A caller-chosen address cannot be honoured without moving the
        // segment; nothing in the payload asks for one.
        kl_errno_set(EINVAL);
        return (void *)-1;
    }
    void *r = (void *)-1;
    pthread_mutex_lock(&g_ipc_mu);
    gl_shmseg *s = gl_shm_find(id);
    if (s) { s->nattch++; s->lpid = getpid(); s->atime = time(NULL); r = s->addr; }
    pthread_mutex_unlock(&g_ipc_mu);
    if (!s) kl_errno_set(EINVAL);
    return r;
}
static int gl_shmdt(const void *addr) {
    if (gl_sysv_host()) return shmdt(addr);
    gl_ipc_note(&g_n_shmdt, "shmdt(%p) (emulated)", addr);
    int ok = 0;
    pthread_mutex_lock(&g_ipc_mu);
    for (unsigned i = 0; i < GL_NSHMSEGS; i++) {
        gl_shmseg *s = &g_shmsegs[i];
        if (!s->id || s->addr != addr || s->nattch <= 0) continue;
        s->nattch--; s->lpid = getpid(); s->dtime = time(NULL); ok = 1;
        if (s->removed && !s->nattch) gl_shm_release(s);
        break;
    }
    pthread_mutex_unlock(&g_ipc_mu);
    if (!ok) { kl_errno_set(EINVAL); return -1; }
    return 0;
}
static int gl_shmctl(int id, int cmd, void *buf) {
    if (gl_sysv_host()) {
        if (cmd == 0 /* IPC_RMID */) return shmctl(id, IPC_RMID, NULL);
        refused("shmctl (KL_GLIBC_SYSV=host: other than IPC_RMID)");
        return -1;
    }
    gl_ipc_note(&g_n_shmctl, "shmctl(id %d, cmd %d) (emulated)", id, cmd);
    if (cmd != 0 && cmd != 2) {
        refused("shmctl (other than IPC_RMID/IPC_STAT)");
        return -1;
    }
    int err = 0;
    pthread_mutex_lock(&g_ipc_mu);
    gl_shmseg *s = gl_shm_find(id);
    if (!s) err = EINVAL;
    else if (cmd == 0) {                          // IPC_RMID: gone at the last detach
        s->removed = 1;
        if (!s->nattch) gl_shm_release(s);
    } else if (!buf) err = EFAULT;
    else {                                        // IPC_STAT: glibc aarch64 shmid_ds
        uint8_t b[112] = {0};
        gl_ipc_perm_fill(b, s->removed ? GL_IPC_PRIVATE : s->key,
                         s->mode | (s->removed ? 01000 /* SHM_DEST */ : 0));
        uint64_t sz = s->size, na = (uint64_t)s->nattch;
        int64_t at = s->atime, dt = s->dtime, ct = s->ctime;
        int32_t cp = s->cpid, lp = s->lpid;
        memcpy(b + 48, &sz, 8); memcpy(b + 56, &at, 8); memcpy(b + 64, &dt, 8);
        memcpy(b + 72, &ct, 8); memcpy(b + 80, &cp, 4); memcpy(b + 84, &lp, 4);
        memcpy(b + 88, &na, 8);
        memcpy(buf, b, sizeof b);
    }
    pthread_mutex_unlock(&g_ipc_mu);
    if (err) { kl_errno_set(err); return -1; }
    return 0;
}
// The same emulation for a BIONIC guest (kl_shim_lookup asks this first):
// bionic's sem*/shm* are the Linux kernel ABI on AArch64 — the same numbers,
// sembuf, semid64_ds and shmid64_ds as glibc's — and on visionOS Darwin's own
// SysV calls are SIGSYS. Before this, bionic semget/semop/semctl forwarded to
// Darwin directly (which on macOS also mistranslated semctl's GETVAL..SETALL
// numbers) and shm* were unresolved. KL_BIONIC_SYSV=0 restores the old binding.
void *kl_sysv_lookup(const char *name) {
    if (!name || (strncmp(name, "sem", 3) && strncmp(name, "shm", 3))) return NULL;
    static int on = -1;
    if (on < 0) { const char *e = getenv("KL_BIONIC_SYSV"); on = !(e && !strcmp(e, "0")); }
    if (!on) return NULL;
    static const struct { const char *n; void *fn; } rows[] = {
        { "semget", (void *)gl_semget }, { "semop", (void *)gl_semop },
        { "semtimedop", (void *)gl_semtimedop }, { "semctl", (void *)gl_semctl },
        { "shmget", (void *)gl_shmget }, { "shmat", (void *)gl_shmat },
        { "shmdt", (void *)gl_shmdt }, { "shmctl", (void *)gl_shmctl },
    };
    for (size_t i = 0; i < sizeof rows / sizeof *rows; i++)
        if (!strcmp(rows[i].n, name)) return rows[i].fn;
    return NULL;
}

// dlmopen(lmid, path, flags): there are no link-map namespaces here (one
// global scope, as the loader builds it), so every namespace is the base one.
static void *gl_dlmopen(long lmid, const char *path, int flags) {
    BIONIC("dlopen", void *(*)(const char *, int))
    (void)lmid;
    return _b ? _b(path, flags) : NULL;
}
// gettext with no message catalogs: every message is its own translation,
// which is what an untranslated program sees.
static char *gl_gettext(const char *m) { return (char *)m; }
static char *gl_dgettext(const char *d, const char *m) { (void)d; return (char *)m; }
static char *gl_dcgettext(const char *d, const char *m, int c) { (void)d; (void)c; return (char *)m; }
static char *gl_dcngettext(const char *d, const char *m1, const char *m2, unsigned long n, int c) {
    (void)d; (void)c; return (char *)(n == 1 ? m1 : m2);
}
static char *gl_textdomain(const char *d) { return (char *)(d ? d : "messages"); }
static char *gl_bindtextdomain(const char *d, const char *dir) { (void)d; return (char *)dir; }
static char *gl_bind_textdomain_codeset(const char *d, const char *c) { (void)d; return (char *)c; }
// catopen with no catalogs: the documented failure, and catgets then answers
// every message with the caller's default string.
static void *gl_catopen(const char *n, int f) { (void)n; (void)f; kl_errno_set(ENOENT); return (void *)-1; }
static char *gl_catgets(void *c, int s, int m, const char *d) { (void)c; (void)s; (void)m; return (char *)d; }
static int   gl_catclose(void *c) { (void)c; kl_errno_set(EBADF); return -1; }

// ---------------------------------------------------------------- data objects
static char   g_single_threaded;         // __libc_single_threaded: threads exist
static void  *g_stack_end;               // __libc_stack_end
static char  *g_progname = "hlvr";       // __progname, program_invocation_short_name
static long   g_timezone;                // __timezone, timezone
static int    g_daylight;                // daylight

static void gl_ctype_init_tables(void);
static void gl_data_init(void) {
    static int done;
    if (done) return;
    done = 1;
    gl_ctype_init_tables();
    gl_file_init(&g_stdin,  stdin,  0, GL_IO_NO_WRITES);
    gl_file_init(&g_stdout, stdout, 1, GL_IO_NO_READS);
    gl_file_init(&g_stderr, stderr, 2, GL_IO_NO_READS | GL_IO_UNBUFFERED);
    kl_host_file_resolver(gl_resolve);
    // The main thread's stack top: KERN_USRSTACK64 is where the kernel put it.
    {
        int mib[2] = { CTL_KERN, KERN_USRSTACK64 };
        uint64_t top = 0;
        size_t len = sizeof top;
        if (sysctl(mib, 2, &top, &len, NULL, 0) == 0) g_stack_end = (void *)(uintptr_t)top;
    }
    tzset();
    time_t now = time(NULL);
    struct tm jan = {0}, jul = {0};
    localtime_r(&now, &jan);
    jan.tm_mon = 0; jan.tm_mday = 1; jan.tm_isdst = -1; time_t tj = mktime(&jan);
    jul = jan; jul.tm_mon = 6; jul.tm_isdst = -1; time_t tl = mktime(&jul);
    localtime_r(&tj, &jan); localtime_r(&tl, &jul);
    g_timezone = -(jan.tm_isdst ? jul.tm_gmtoff : jan.tm_gmtoff);
    g_daylight = jan.tm_gmtoff != jul.tm_gmtoff;
}

// ---------------------------------------------------------------- the table
//
//   GL_SAME(n)       the bionic table's answer: same name, same ABI
//   GL_AS(n, b)      the bionic table's answer for `b` (a glibc alias)
//   GL_FN(n, f)      a glibc-shaped rebuild in this file (or a Darwin call
//                    with an identical signature)
//   GL_ADDR(n, e)    an address computed at lookup (e is an expression)
//   GL_OBJ(n, a, s)  a data object of `s` bytes at `a`
//   GL_NOSYS(n)      refused: -1 and ENOSYS
//   GL_NULL(n)       refused: NULL and ENOSYS
//   GL_ERR(n)        refused: the error number ENOSYS as the return value
//                    (the pthread / posix_spawn convention)
//   GL_ABORT(n)      a noreturn call (err, errx): aborts naming itself — a
//                    refusal that returned would run the code after it
typedef struct { const char *name; uint8_t kind; const char *as; void *addr; size_t size; } gl_row;
enum { K_SAME, K_AS, K_FN, K_OBJ, K_NOSYS, K_NULL, K_ERR, K_ADDR, K_ABORT };
#define GL_SAME(n)      { #n, K_SAME,  NULL, NULL, 0 },
#define GL_AS(n, b)     { #n, K_AS,    #b,   NULL, 0 },
#define GL_FN(n, f)     { #n, K_FN,    NULL, (void *)(f), 0 },
#define GL_OBJ(n, a, s) { #n, K_OBJ,   NULL, (void *)(a), (s) },
#define GL_NOSYS(n)     { #n, K_NOSYS, NULL, NULL, 0 },
#define GL_NULL(n)      { #n, K_NULL,  NULL, NULL, 0 },
#define GL_ERR(n)       { #n, K_ERR,   NULL, NULL, 0 },
#define GL_ADDR(n, f)   { #n, K_ADDR,  NULL, (void *)(f), 0 },
#define GL_ABORT(n)     { #n, K_ABORT, NULL, NULL, 0 },

static const gl_row g_rows[] = {
#include "kl_glibc_table.h"
};
#define GL_NROWS (sizeof g_rows / sizeof *g_rows)

// The NATIVE rows: names the jailbreak host used to answer itself (its 76-name
// intercept table, hla/recon/hla_glibc_host_intercepted.txt) and that the
// native boot, with no host, must answer here. A separate table on purpose:
// kl_glibc_each — and so the stand-in the reference door hands the host — never
// sees them, so the host build binds exactly what it bound before.
static const gl_row g_native_rows[] = {
#include "kl_glibc_native_table.h"
};
#define GL_NNATIVE (sizeof g_native_rows / sizeof *g_native_rows)

static void *gl_row_addr(const gl_row *r) {
    switch (r->kind) {
    case K_SAME:  return kl_shim_lookup(r->name);
    case K_AS:    return kl_shim_lookup(r->as);
    case K_FN:    return r->addr;
    case K_OBJ:   return r->addr;
    case K_ADDR:  return ((void *(*)(void))r->addr)();
    case K_NOSYS: return kl_named_stub(r->name, (void *)refuse_int);
    case K_NULL:  return kl_named_stub(r->name, (void *)refuse_ptr);
    case K_ERR:   return kl_named_stub(r->name, (void *)refuse_err);
    case K_ABORT: return kl_named_stub(r->name, (void *)kl_unresolved_named);
    }
    return NULL;
}

void *kl_glibc_lookup(const char *name) {
    if (!name) return NULL;
    gl_data_init();
    for (size_t i = 0; i < GL_NROWS; i++)
        if (!strcmp(g_rows[i].name, name)) return gl_row_addr(&g_rows[i]);
    return NULL;
}

static const gl_row *gl_find(const char *name, int native) {
    if (native)
        for (size_t i = 0; i < GL_NNATIVE; i++)
            if (!strcmp(g_native_rows[i].name, name)) return &g_native_rows[i];
    for (size_t i = 0; i < GL_NROWS; i++)
        if (!strcmp(g_rows[i].name, name)) return &g_rows[i];
    return NULL;
}

// A door's own answer for a native row (the native Alyx door's dlopen family,
// __libc_start_main, getauxval, exit): consulted before both tables, so the
// personality keeps its place first in the search while the door — which owns
// the loader scopes and the entry — answers the few names only it can.
#define GL_OVERRIDES 32
static struct { const char *name; void *fn; } g_override[GL_OVERRIDES];
static unsigned g_noverride;
void kl_glibc_override(const char *name, void *fn) {
    if (!name || !fn) return;
    for (unsigned i = 0; i < g_noverride; i++)
        if (!strcmp(g_override[i].name, name)) { g_override[i].fn = fn; return; }
    if (g_noverride == GL_OVERRIDES) {
        fprintf(stderr, "  [glibc] override table FULL — %s NOT overridden\n", name);
        return;
    }
    g_override[g_noverride].name = name;
    g_override[g_noverride].fn = fn;
    g_noverride++;
}
static void *gl_override(const char *name) {
    for (unsigned i = 0; i < g_noverride; i++)
        if (!strcmp(g_override[i].name, name)) return g_override[i].fn;
    return NULL;
}

void *kl_glibc_lookup_native(const char *name) {
    if (!name) return NULL;
    void *o = gl_override(name);
    if (o) return o;
    gl_data_init();
    const gl_row *r = gl_find(name, 1);
    return r ? gl_row_addr(r) : NULL;
}

int kl_glibc_row_kind(const char *name) {
    if (name && gl_override(name)) return KL_GLIBC_ANSWERED;
    const gl_row *r = name ? gl_find(name, 1) : NULL;
    if (!r) return KL_GLIBC_NONE;
    switch (r->kind) {
    case K_NOSYS: case K_NULL: case K_ERR: return KL_GLIBC_REFUSED;
    case K_ABORT: return KL_GLIBC_ABORTS;
    default:      return KL_GLIBC_ANSWERED;
    }
}

void kl_glibc_each(kl_glibc_each_fn fn, void *ctx) {
    gl_data_init();
    for (size_t i = 0; i < GL_NROWS; i++) {
        void *a = gl_row_addr(&g_rows[i]);
        if (!a) {
            fprintf(stderr, "  [glibc] %s: the table says %s, which answers nothing — "
                            "left out of the stand-in\n", g_rows[i].name,
                    g_rows[i].kind == K_AS ? g_rows[i].as : "the bionic table");
            continue;
        }
        fn(g_rows[i].name, a, g_rows[i].size, g_rows[i].kind == K_OBJ, ctx);
    }
}

// ---------------------------------------------------------------- the stand-in
//
// One r-- PT_LOAD over the whole file and a PT_DYNAMIC. No code, no
// relocations, no initialisers: every exported symbol is SHN_ABS, its value an
// address in this process, so whoever binds against the stand-in binds to the
// personality directly. Both DT_HASH and DT_GNU_HASH are written (a loader may
// size .dynsym from either), the GNU table with a bloom filter of all ones —
// "maybe present" for every name, which is always a correct filter.
typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx;
                 uint64_t st_value, st_size; } gl_sym;
typedef struct { const char **names; void **addrs; size_t *sizes; uint8_t *data; unsigned n, cap; } gl_exp;
static void gl_collect(const char *name, void *addr, size_t size, int is_data, void *ctx) {
    gl_exp *e = ctx;
    if (e->n == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 1024;
        e->names = realloc(e->names, e->cap * sizeof *e->names);
        e->addrs = realloc(e->addrs, e->cap * sizeof *e->addrs);
        e->sizes = realloc(e->sizes, e->cap * sizeof *e->sizes);
        e->data  = realloc(e->data,  e->cap);
    }
    e->names[e->n] = name; e->addrs[e->n] = addr; e->sizes[e->n] = size;
    e->data[e->n] = (uint8_t)is_data; e->n++;
}
static uint32_t gl_gnu_hash(const char *s) {
    uint32_t h = 5381;
    for (; *s; s++) h = h * 33 + (uint8_t)*s;
    return h;
}
static uint32_t gl_sysv_hash(const char *s) {
    uint32_t h = 0, g;
    for (; *s; s++) { h = (h << 4) + (uint8_t)*s; if ((g = h & 0xf0000000u)) h ^= g >> 24; h &= ~g; }
    return h;
}
static unsigned g_nbucket;
static int gl_bucket_cmp(const void *a, const void *b) {
    uint32_t x = gl_gnu_hash(*(const char *const *)a) % g_nbucket;
    uint32_t y = gl_gnu_hash(*(const char *const *)b) % g_nbucket;
    return x < y ? -1 : x > y;
}

int kl_glibc_write_standin(const char *path, const char *soname, char *err, size_t cap) {
    gl_exp e = {0};
    kl_glibc_each(gl_collect, &e);
    if (!e.n) { snprintf(err, cap, "the personality answers no names"); return -1; }

    // Order the symbols by GNU hash bucket (the GNU table requires it); the
    // index array rides along so each name keeps its address.
    unsigned n = e.n;
    g_nbucket = n / 4 + 1;
    typedef struct { const char *name; unsigned idx; } ent;
    ent *order = malloc(n * sizeof *order);
    for (unsigned i = 0; i < n; i++) { order[i].name = e.names[i]; order[i].idx = i; }
    qsort(order, n, sizeof *order, gl_bucket_cmp);

    // String table: "\0" soname names...
    size_t strsz = 1 + strlen(soname) + 1;
    for (unsigned i = 0; i < n; i++) strsz += strlen(order[i].name) + 1;
    char *str = calloc(1, strsz);
    size_t so_off = 1, at = 1;
    memcpy(str + at, soname, strlen(soname) + 1); at += strlen(soname) + 1;
    uint32_t *name_off = malloc(n * sizeof *name_off);
    for (unsigned i = 0; i < n; i++) {
        name_off[i] = (uint32_t)at;
        memcpy(str + at, order[i].name, strlen(order[i].name) + 1);
        at += strlen(order[i].name) + 1;
    }

    unsigned nsym = n + 1;                     // [0] is the null symbol
    // DT_HASH: nbucket, nchain, buckets, chains.
    unsigned hb = n / 2 + 1;
    size_t hash_sz = (2 + hb + nsym) * 4;
    // DT_GNU_HASH: nbuckets, symoffset(1), bloom_size(1), bloom_shift, bloom[1], buckets, chains.
    size_t gnu_sz = 16 + 8 + g_nbucket * 4 + n * 4;
    size_t sym_sz = nsym * sizeof(gl_sym);
    size_t dyn_n  = 8;
    size_t dyn_sz = dyn_n * 16;

    size_t off_ph   = 64;
    size_t off_hash = (off_ph + 2 * 56 + 7) & ~7ul;
    size_t off_gnu  = (off_hash + hash_sz + 7) & ~7ul;
    size_t off_sym  = (off_gnu + gnu_sz + 7) & ~7ul;
    size_t off_str  = off_sym + sym_sz;
    size_t off_dyn  = (off_str + strsz + 7) & ~7ul;
    size_t total    = off_dyn + dyn_sz;
    uint8_t *f = calloc(1, total);

    // ELF header
    memcpy(f, "\x7f" "ELF", 4);
    f[4] = 2; f[5] = 1; f[6] = 1;              // 64-bit, little-endian, current
    *(uint16_t *)(f + 16) = 3;                 // ET_DYN
    *(uint16_t *)(f + 18) = 183;               // EM_AARCH64
    *(uint32_t *)(f + 20) = 1;
    *(uint64_t *)(f + 32) = off_ph;            // e_phoff
    *(uint16_t *)(f + 52) = 64;                // e_ehsize
    *(uint16_t *)(f + 54) = 56;                // e_phentsize
    *(uint16_t *)(f + 56) = 2;                 // e_phnum
    *(uint16_t *)(f + 58) = 64;                // e_shentsize (no sections)

    gl_phdr *ph = (gl_phdr *)(f + off_ph);
    ph[0].p_type = GL_PT_LOAD; ph[0].p_flags = 4;       // PF_R
    ph[0].p_offset = 0; ph[0].p_vaddr = ph[0].p_paddr = 0;
    ph[0].p_filesz = ph[0].p_memsz = total; ph[0].p_align = 0x10000;
    ph[1].p_type = 2; ph[1].p_flags = 4;                // PT_DYNAMIC
    ph[1].p_offset = ph[1].p_vaddr = ph[1].p_paddr = off_dyn;
    ph[1].p_filesz = ph[1].p_memsz = dyn_sz; ph[1].p_align = 8;

    // .dynsym
    gl_sym *sym = (gl_sym *)(f + off_sym);
    for (unsigned i = 0; i < n; i++) {
        unsigned k = order[i].idx;
        gl_sym *s = &sym[i + 1];
        s->st_name  = name_off[i];
        s->st_info  = (uint8_t)((1 /* STB_GLOBAL */ << 4) | (e.data[k] ? 1 /* OBJECT */ : 2 /* FUNC */));
        s->st_shndx = 0xfff1;                  // SHN_ABS
        s->st_value = (uint64_t)(uintptr_t)e.addrs[k];
        s->st_size  = e.sizes[k];
    }
    memcpy(f + off_str, str, strsz);

    // DT_HASH
    uint32_t *h = (uint32_t *)(f + off_hash);
    h[0] = hb; h[1] = nsym;
    uint32_t *hbk = h + 2, *hch = h + 2 + hb;
    for (unsigned i = 1; i < nsym; i++) {
        uint32_t b = gl_sysv_hash(order[i - 1].name) % hb;
        hch[i] = hbk[b];
        hbk[b] = i;
    }
    // DT_GNU_HASH
    uint32_t *gh = (uint32_t *)(f + off_gnu);
    gh[0] = g_nbucket; gh[1] = 1; gh[2] = 1; gh[3] = 6;
    *(uint64_t *)(gh + 4) = ~0ull;
    uint32_t *gbk = gh + 6, *gch = gh + 6 + g_nbucket;
    for (unsigned i = 0; i < n; i++) {
        uint32_t hv = gl_gnu_hash(order[i].name), b = hv % g_nbucket;
        if (!gbk[b]) gbk[b] = i + 1;
        int last = i + 1 == n || gl_gnu_hash(order[i + 1].name) % g_nbucket != b;
        gch[i] = (hv & ~1u) | (last ? 1u : 0u);
    }

    // .dynamic
    uint64_t *d = (uint64_t *)(f + off_dyn);
    size_t k = 0;
    d[k++] = 14; d[k++] = so_off;              // DT_SONAME
    d[k++] = 4;  d[k++] = off_hash;            // DT_HASH
    d[k++] = 0x6ffffef5; d[k++] = off_gnu;     // DT_GNU_HASH
    d[k++] = 5;  d[k++] = off_str;             // DT_STRTAB
    d[k++] = 6;  d[k++] = off_sym;             // DT_SYMTAB
    d[k++] = 10; d[k++] = strsz;               // DT_STRSZ
    d[k++] = 11; d[k++] = sizeof(gl_sym);      // DT_SYMENT
    d[k++] = 0;  d[k++] = 0;                   // DT_NULL

    int rc = -1;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.kl-tmp", path);
    FILE *o = fopen(tmp, "wb");
    if (!o) snprintf(err, cap, "cannot write %s: %s", tmp, strerror(errno));
    else {
        size_t w = fwrite(f, 1, total, o);
        fclose(o);
        if (w != total) snprintf(err, cap, "short write to %s", tmp);
        else if (rename(tmp, path) != 0) snprintf(err, cap, "rename %s: %s", path, strerror(errno));
        else rc = 0;
    }
    free(f); free(str); free(name_off); free(order);
    free(e.names); free(e.addrs); free(e.sizes); free(e.data);
    return rc;
}
