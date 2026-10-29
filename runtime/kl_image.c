// ELF64/AArch64 image loader + self-relocator for Darwin arm64.
// macOS has no <elf.h>, so the (small) subset we need is defined inline.
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <mach/mach.h>
// vm_*, not mach_vm_*: <mach/mach_vm.h> is `#error unsupported` on both the
// xrOS and xrsimulator SDKs, while <mach/vm_map.h> exists on all three and the
// two families are identical for an in-process mach_task_self() call. One code
// path rather than an #ifdef, so `make check` on the host tests what ships.
#include <mach/vm_map.h>
#include <dlfcn.h>
#include <limits.h>
#include <dirent.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include "klepton.h"
#include "kl_x18.h"
#include "kl_guestpatch.h"
#include "kl_tls.h"
#include "kl_glibc.h"      // kl_glibc_lookup_native: a glibc image's first answer


// ---------- ELF64 subset ----------
typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } Elf64_Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr,
    p_filesz, p_memsz, p_align; } Elf64_Phdr;
typedef struct { uint64_t d_tag; uint64_t d_val; } Elf64_Dyn;
typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx;
    uint64_t st_value, st_size; } Elf64_Sym;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } Elf64_Rela;
typedef struct { uint32_t sh_name, sh_type; uint64_t sh_flags, sh_addr, sh_offset,
    sh_size; uint32_t sh_link, sh_info; uint64_t sh_addralign, sh_entsize; } Elf64_Shdr;

#define SHT_PROGBITS  1
#define SHT_DYNSYM    11
#define SHF_EXECINSTR 0x4
#define SHF_WRITE     0x1
#define SHF_ALLOC     0x2
#define SHT_NOBITS 8
// The image behind an executable chunk, for the `br x18` jump-table rewrite —
// see kl_x18_image in kl_x18.h. `bias` turns a MAPPED address back into the
// ELF's own VA, because at load time the addresses handed to the veneer pass are
// the mapping's own.
struct x18_img_ctx { const Elf64_Shdr *sh; int shnum; uint64_t bias; };

// Read-only ALLOC data only. Executable would let a mis-recognition write over
// code; writable (.data.rel.ro) is rewritten by relocations at load, which would
// undo the repointing silently. A jump table is in .rodata.
static void *x18_img_data_at(void *vctx, uint64_t va, size_t len) {
    struct x18_img_ctx *c = vctx;
    uint64_t gva = va - c->bias;
    for (int i = 0; i < c->shnum; i++) {
        const Elf64_Shdr *s = &c->sh[i];
        if (s->sh_type != SHT_PROGBITS) continue;
        if (s->sh_flags & (SHF_EXECINSTR | SHF_WRITE)) continue;
        if (!(s->sh_flags & SHF_ALLOC) || !s->sh_addr) continue;
        if (gva < s->sh_addr || gva + len > s->sh_addr + s->sh_size) continue;
        return (void *)(uintptr_t)va;      // the mapping is contiguous from `lo`
    }
    return NULL;
}

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_TLS 7
#define PT_GNU_RELRO 0x6474e552
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define DT_NULL 0
#define DT_NEEDED 1
#define DT_HASH   4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
// Android's packed relocations. The NDK links most modern .so files with
// `--pack-dyn-relocs=android`, which REPLACES DT_RELA/DT_RELASZ with an
// sleb128-compressed stream under these two tags. An image that carries them
// has no DT_RELA at all, so a loader that only knows DT_RELA relocates NOTHING
// and says nothing — see decode_android_relocs() for what that cost.
#define DT_ANDROID_RELA   0x60000011
#define DT_ANDROID_RELASZ 0x60000012
// The other compressed form, RELR — a bitmap of R_AARCH64_RELATIVE sites and
// nothing else, which is why it can hold a library's entire relative-relocation
// population in a few hundred bytes.
//
// TWO NUMBERINGS, and knowing only one of them is worth a whole round. RELR was
// shipped by Android under DT_ANDROID_RELR (0x6fffe000) before it was
// standardised as DT_RELR (36), and the NDK still emits the Android numbers.
// The avatar libraries here carry 0x6fffe000/1/3 — so a loader that knew the
// standard tags alone saw NO relative relocations at all, said nothing (there
// was no tag it recognised to refuse), and called a DT_INIT_ARRAY entry that
// was still a link-time vaddr: `signal 11 at 0x805338`. Round 27 added the
// refusal for the standard tags and hit exactly that crash through the ones
// below, which is what named them.
#define DT_RELR   0x24
#define DT_RELRSZ 0x23
#define DT_RELRENT 0x25
#define DT_ANDROID_RELR    0x6fffe000
#define DT_ANDROID_RELRSZ  0x6fffe001
#define DT_ANDROID_RELRENT 0x6fffe003
#define DT_INIT       12
#define DT_SYMBOLIC   16
#define DT_FLAGS      30
#define DF_SYMBOLIC   0x2
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_JMPREL 23
#define DT_PLTRELSZ 2
#define ELF64_R_SYM(i)  ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i) ((uint32_t)(i))
#define R_AARCH64_ABS64      257
#define R_AARCH64_GLOB_DAT  1025
#define R_AARCH64_JUMP_SLOT 1026
#define R_AARCH64_RELATIVE  1027
// A 16-byte TLS descriptor: word 0 the resolver, word 1 its argument. Served by
// runtime/kl_tls.c — see kl_tls.h for how it maps onto S0.1's TLS model.
#define R_AARCH64_TLSDESC   1031
// A constant offset from the thread pointer into the image's STATIC TLS block
// (initial-exec code). Served by kl_tls_static_block — see kl_tls.h.
#define R_AARCH64_TLS_TPREL64 1030
#define STB_LOCAL 0
#define STB_WEAK 2
#define STV_PROTECTED 3
// glibc's "one definition process-wide" binding (libstdc++'s inline statics,
// 106 of them; libphonon 9). Bound like STB_GLOBAL, except that in glibc mode a
// DEFINED one still binds to the first definition in load order.
#define STB_GNU_UNIQUE 10
#define DT_SONAME   14
#define DT_RPATH    15
#define DT_RUNPATH  29
// The GNU hash table, and the reason it is read at all: it is the ONLY symbol
// table index in 131 of the Alyx payload's 133 ELFs (no DT_HASH), and without
// one there is no honest .dynsym count — see gnu_hash_nsyms.
#define DT_GNU_HASH 0x6ffffef5
#define DT_VERSYM   0x6ffffff0
#define DT_VERNEED    0x6ffffffe
#define DT_VERNEEDNUM 0x6fffffff

struct kl_image {
    uint8_t   *base;        // ELF vaddr V lives at base + V (V relative to the
                            // lowest PT_LOAD); the load bias the guest sees
    size_t     span;        // ...and the vaddr span, base to the last mapped byte
    // Where `base` sits inside the mapping we actually asked the kernel for, and
    // how big that mapping is. Nonzero map_off only for a guest whose p_align is
    // finer than the host page — see choose_bias(). munmap needs both, since
    // base is then not the address mmap returned.
    size_t     map_off, map_size;
    // The x18 veneer pool, reserved on the end of the same mapping so it is
    // within `b`'s reach of the code by construction. munmap frees both.
    size_t     x18_arena_size;
    Elf64_Sym *symtab;
    size_t     nsyms;       // from DT_HASH's nchain; 0 if the image had none
    const char *strtab;
    void     (**init_array)(void);
    size_t     init_count;
    void      (*init_fn)(void);      // DT_INIT (glibc images run it before the array)
    int        symbolic;             // DT_SYMBOLIC / DF_SYMBOLIC: own definitions first
    kl_stats   stats;
    char       path[512];
    const char **missing;      // unique unresolved import names
    unsigned    missing_n, missing_cap;
    const char **weak;         // ...and the weak ones, deliberately left NULL
    unsigned    weak_n, weak_cap;
    void       *dl_handle;     // set when the image arrived as a translated
                               // Mach-O dylib; then dyld owns the mapping
    // PT_TLS, read before the relocations so a TLSDESC can be served. `tlsmod`
    // is registered lazily on the first TLSDESC — an image with a PT_TLS and no
    // TLSDESC (most of them) costs nothing.
    const void *tls_init;
    uint64_t    tls_filesz, tls_memsz, tls_align;
    int         has_tls;
    kl_tlsmod  *tlsmod;
    int64_t     stls_off;      // TPREL64's static block: its TP offset, 0 = none
    // DT_GNU_HASH (kl_sym's index when present) and DT_VERSYM (glibc mode skips
    // the hidden, non-default versions a versioned library carries beside the
    // default one under the same name).
    const uint32_t *gnu_hash;
    const uint16_t *versym;
    const uint8_t  *verneed;   // DT_VERNEED: which FILE each version index names
    unsigned        verneednum;
    int             bound;     // relocations applied (kl_map_dylib_unbound: not yet)
    // A GLIBC image (the Alyx payload): imports resolve in glibc's order — the
    // personality first — rather than bionic's. See sym_value_glibc.
    int         glibc;
    const char *soname;        // DT_SONAME, or NULL
    const char *needed[48];    // DT_NEEDED, in order (into strtab)
    unsigned    nneeded;
};

static void record_missing(kl_image *img, const char *nm) {
    if (!nm) return;
    for (unsigned i = 0; i < img->missing_n; i++)
        if (strcmp(img->missing[i], nm) == 0) return;      // already seen
    if (img->missing_n == img->missing_cap) {
        img->missing_cap = img->missing_cap ? img->missing_cap * 2 : 64;
        img->missing = realloc(img->missing, img->missing_cap * sizeof *img->missing);
    }
    img->missing[img->missing_n++] = nm;
}

static void record_weak(kl_image *img, const char *nm) {
    if (!nm) return;
    for (unsigned i = 0; i < img->weak_n; i++)
        if (strcmp(img->weak[i], nm) == 0) return;
    if (img->weak_n == img->weak_cap) {
        img->weak_cap = img->weak_cap ? img->weak_cap * 2 : 32;
        img->weak = realloc(img->weak, img->weak_cap * sizeof *img->weak);
    }
    img->weak[img->weak_n++] = nm;
}

const char *const *kl_missing_imports(kl_image *img, unsigned *count) {
    if (count) *count = img->missing_n;
    return img->missing;
}

const char *const *kl_weak_imports(kl_image *img, unsigned *count) {
    if (count) *count = img->weak_n;
    return img->weak;
}

// ---------- unresolved-import stubs ----------
// Every unresolved import gets its OWN cell, so calling one fails BY NAME — the
// name is where the next milestone starts, and one shared abort stub can only
// report "called an unresolved import". The cell loads the name and tail-calls
// the reporter:
//
//   0:  mov w16, #N              // this cell's index, baked in at build time
//   4:  b   kl_stub_named_common // loads slots[N] -> x0, slots[N+1] -> x16, br
//
// Data imports get one too. They are not called, so the guest dereferences the
// cell instead — but a wild pointer into a known page beats a wild pointer into
// nothing, and the index identifies the name.
// The cells are PRE-BUILT and signed with the binary — runtime/kl_stub_cells.S,
// which carries the full rationale. Nothing here writes instructions any more:
// a cell knows only its own index, and the allocator's whole job is to fill in
// the { payload, target } pair the shared dispatcher will read.
//
// An mmap'd pool generated into at runtime is a SIGKILL on visionOS instead —
// AMFI answers "namespace CODESIGNING, Invalid Page" for any pc that did not
// come from a signed file, whatever the page's permissions are.
// Measured on device 2026-08-07; see kl_stub_cells.S for the report.
#define KL_STUB_CELL_BYTES  8              /* mov w16,#i + b — see the .S */
// 2048 was sized for Beat Saber's five libraries. Steam Link's Qt chain is a
// different order of magnitude — libQt6Widgets ALONE exhausted it, and past
// exhaustion an import silently becomes an unnamed stub, so failing BY NAME —
// the property the whole bring-up method rests on — stops working. 16384;
// the ceiling is 65536, above which `mov w16,#N` stops being a single movz and
// the cell stride in kl_stub_cells.S would have to change.
#define KL_STUB_NAMED_CELLS 16384
#define KL_STUB_TRACE_CELLS 1024

void kl_unresolved_named(const char *name);   // kl_shim.c

// The cells (text, read-only, signed) and the slots the dispatcher reads
// (data, written here). Both names are referenced from kl_stub_cells.S.
extern char kl_stub_named_cells[];
extern char kl_stub_trace_cells[];
void *kl_stub_named_slots[KL_STUB_NAMED_CELLS * 2];
void *kl_stub_trace_slots[KL_STUB_TRACE_CELLS * 2];

static struct { char *name; void *stub; void *handler; } *g_stubs;
static unsigned g_stub_n, g_stub_cap;
static unsigned g_named_used, g_trace_used;
static pthread_mutex_t g_stub_mu = PTHREAD_MUTEX_INITIALIZER;

// Claim one pre-built cell and point it at (payload, target). Caller holds
// g_stub_mu. `payload` NULL means the owned copy of nm (kl_named_stub's
// argument). `trace` picks which of the two shapes — and therefore which cell
// array and which dispatcher — the caller wants.
//
// The mutex covers the dedup scan and the bump counter, which are racy under
// M6's concurrent stub traffic (the render thread calling existing stubs while
// the main thread creates new ones). It guards no W^X window: the cells are
// read-only text from the moment the binary is signed, and only the slots array
// is written.
static void *stub_emit(const char *nm, void *payload, void *target, int trace) {
    for (unsigned i = 0; i < g_stub_n; i++)             // shared across images
        if (g_stubs[i].handler == target && strcmp(g_stubs[i].name, nm) == 0)
            return g_stubs[i].stub;

    unsigned cap  = trace ? KL_STUB_TRACE_CELLS : KL_STUB_NAMED_CELLS;
    unsigned *use = trace ? &g_trace_used : &g_named_used;
    if (*use >= cap) {
        // Exhaustion is now a fixed ceiling rather than a 64 KB pool, so say so
        // by name instead of returning a silent NULL that becomes a generic
        // "unresolved" a layer up.
        fprintf(stderr, "  [klepton] out of %s stub cells (%u) — '%s' unnamed\n",
                trace ? "trace" : "named", cap, nm);
        return NULL;
    }

    // The name lives in the image's strtab, which kl_unload would take with it.
    char *owned = strdup(nm);
    if (!owned) return NULL;

    unsigned idx = (*use)++;
    if (!payload) payload = owned;
    void **slots = trace ? kl_stub_trace_slots : kl_stub_named_slots;
    slots[idx * 2]     = payload;
    slots[idx * 2 + 1] = target;

    void *s = (trace ? kl_stub_trace_cells : kl_stub_named_cells)
              + (size_t)idx * KL_STUB_CELL_BYTES;

    if (kl_env_on("KL_TRACE_IMAGES", 0))
        fprintf(stderr, "  [klepton] stub '%s' -> %s cell %u (%p)\n",
                nm, trace ? "trace" : "named", idx, s);

    if (g_stub_n == g_stub_cap) {
        g_stub_cap = g_stub_cap ? g_stub_cap * 2 : 64;
        g_stubs = realloc(g_stubs, g_stub_cap * sizeof *g_stubs);
    }
    g_stubs[g_stub_n].name = owned;
    g_stubs[g_stub_n].stub = s;
    g_stubs[g_stub_n].handler = target;
    g_stub_n++;
    return s;
}

// Build a stub that tail-calls `handler` with `nm` in x0. Shared by anything
// that has to name what the guest reached for: unresolved ELF imports, and the
// GL entry points handed out by eglGetProcAddress (kl_egl.c), which have the
// same problem — one anonymous abort tells you nothing about which of two
// hundred functions the guest actually called.
void *kl_named_stub(const char *nm, void *handler) {
    if (!nm || !*nm || !handler) return kl_shim_lookup("__klepton_unresolved");
    pthread_mutex_lock(&g_stub_mu);
    void *s = stub_emit(nm, NULL, handler, 0 /* named */);
    pthread_mutex_unlock(&g_stub_mu);
    return s ? s : kl_shim_lookup("__klepton_unresolved");
}

// The other shape of stub: one that logs and then lets the original call proceed
// untouched. kl_named_stub puts the name in x0 and never returns to the guest,
// which is right for "you called something unimplemented" and useless for tracing.
// Here the payload is a descriptor instead of a bare name, and the branch goes to
// kl_gl_trace_tramp (runtime/gfx/kl_gl_trace.S), which saves the argument registers,
// logs, restores, and tail-branches to the real function.
//
//    0: mov w16, #N              // this cell's index
//    4: b   kl_stub_trace_common // loads slots[N] -> x16, slots[N+1] -> x17, br
//
// A SEPARATE cell array from kl_named_stub's, because the two shapes differ in
// what they may clobber: this one has to leave x0-x7 alone (the real function
// still needs its arguments), so the payload arrives in x16 rather than x0.
// Dedup is on (trampoline, name) exactly as it is on (handler, name).
void *kl_trace_stub(const char *nm, void *desc, void *tramp) {
    if (!nm || !*nm || !desc || !tramp) return NULL;
    pthread_mutex_lock(&g_stub_mu);
    void *s = stub_emit(nm, desc, tramp, 1 /* trace */);
    pthread_mutex_unlock(&g_stub_mu);
    return s;
}

static void *unresolved_stub(const char *nm) {
    return kl_named_stub(nm, (void *)kl_unresolved_named);
}

static char g_err[512];
static void err(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vsnprintf(g_err, sizeof g_err, fmt, a); va_end(a);
}
const char *kl_error(void) { return g_err; }
void  *kl_base(kl_image *i) { return i->base; }
size_t kl_span(kl_image *i) { return i->span; }
const kl_stats *kl_get_stats(kl_image *i) { return &i->stats; }

// The mapped phdr array. Every guest lib here has a first PT_LOAD covering file
// offset 0 at vaddr 0 (verified for all five), so the ELF header — and the phdr
// table it points at — is mapped at `base`, and the load bias equals the base.
// Checked rather than assumed: a lib whose phdrs are not mapped reports none,
// and the unwinder then simply does not see it.
const void *kl_phdrs(kl_image *i, unsigned *count) {
    if (count) *count = 0;
    if (!i || !i->base) return NULL;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)i->base;
    if (memcmp(eh->e_ident, "\177ELF", 4) != 0) return NULL;
    if (eh->e_phentsize != sizeof(Elf64_Phdr)) return NULL;
    if (eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > i->span) return NULL;
    if (count) *count = eh->e_phnum;
    return i->base + eh->e_phoff;
}

// ---------- rewrite `mrs xN, tpidr_el0` -> `mrs xN, tpidrro_el0` ----------
// Darwin keeps the thread pointer in TPIDRRO_EL0; TPIDR_EL0 is clobbered by the
// kernel on preemption. The two encodings differ by exactly one bit (0x20).
// 98% of these sites are -fstack-protector prologues reading bionic slot 5.
// It runs on the same chunks as the x18 pass and carries the same exposure to
// data inside an executable section: a word of a constant table matching this
// pattern gets a bit flipped, and the corrupted constant surfaces somewhere
// else entirely. One in 2^27 words matches by chance. Both rewriters consult
// the same data detector; this one must read the UNMODIFIED buffer, which it
// does, being the first pass over a chunk.
//
// A refusal here is NOT the free outcome it is on the x18 side, and it must
// never again be silent: a `mrs xN, tpidr_el0` left alone reads a register the
// kernel clobbers, so the site returns garbage on every thread and the guest
// faults dereferencing it somewhere else entirely — so a false positive in
// klx_looks_like_data reintroduces the very bug the TLS rewrite fixes. Each one
// is counted apart from the x18 population and
// named by address, because the address is what makes it a five-minute
// disassembly instead of a bisect.
// The guest-patch table's view of this mapping: one contiguous span, so a guest
// vaddr is a subtraction. Out of range answers NULL, which the table reports by
// address rather than treating as a miss.
static uint32_t *gp_at(void *ctx, uint64_t va) {
    struct gp_ctx { uint8_t *base; uint64_t lo, span; } *c = ctx;
    if (va < c->lo || va + 4 > c->lo + c->span) return NULL;
    return (uint32_t *)(c->base + (va - c->lo));
}

static void rewrite_tls(uint8_t *p, size_t n, uint64_t va, const char *path,
                        kl_stats *st) {
    uint32_t *w = (uint32_t *)p;
    size_t words = n / 4;
    for (size_t i = 0; i < words; i++) {
        if ((w[i] & 0xffffffe0u) == 0xd53bd040u) {
            if (kl_x18_is_data(w, n, i)) {
                st->tls_refused++;
                fprintf(stderr, "  [klepton] %s: TLS site at +0x%llx left alone — its "
                                "neighbourhood reads as data. If it IS code, "
                                "that thread pointer is garbage\n",
                        path, (unsigned long long)(va + i * 4));
                continue;
            }
            w[i] |= 0x20u;
            st->tls_rewrites++;
        } else if ((w[i] & 0xffffffe0u) == 0xd51bd040u && !kl_x18_is_data(w, n, i)) {
            // `msr tpidr_el0, xN` -> `nop`. The guest's thread pointer is
            // TPIDRRO_EL0 here (the read rewrite above), which user code cannot
            // write; TPIDR_EL0 is Darwin's, and libmalloc reads its low byte as
            // the CPU number for per-CPU zone state. The Alyx jailbreak host
            // saves the "thread pointer" around every call into its guest and
            // writes it back (479 sites) — the TSD base, into Darwin's CPU
            // register — and the next xzone malloc indexed its per-CPU table
            // with a pointer's low byte and faulted forever. A guest cannot
            // switch its thread pointer on Darwin at all, so the write is
            // dropped rather than performed wrongly.
            w[i] = 0xd503201fu;
            st->tp_writes++;
        } else if (w[i] == 0xd4000001u) { st->svc_sites++; }    // svc #0 — report only
    }
}

// ---------- symbol lookup ----------
// Registered interposers, consulted for defined and undefined symbols alike.
// Small and fixed: this is an instrument, and a table that has to grow is a sign
// the question being asked wants a different tool.
//
// It grew anyway, and the way it grew is the reason this now complains. The
// SHAR interior probe alone installs nine rows (Render, three InteriorManager
// entry points, rReleasePrintf, Place, Remove, MarkCameraVisible,
// IsSphereInCone); against the old bound of eight, the NINTH was dropped on the
// floor without a word, and the row that lost was whichever was registered
// last. That is the worst possible failure for an instrument: the build looks
// installed, the log looks normal, and one hook — round 27 nearly shipped with
// IsSphereInCone as the casualty, taking round 24's force-accept and the whole
// wall census with it — is simply not there. Sixteen rows, and a refusal that
// says so.
// 14 rows are live on the simpsons target as of round 37 (the SHAR interior,
// streaming and radar probes plus kl_slink's two). The refusal below is loud
// but it is still a dropped hook, and a dropped hook on a REPLACING
// interposer silently changes behaviour — so the headroom is worth its 128
// bytes.
// hla needs 26 as of round 5 (its loader serving, locale boundary, guards,
// the device-create audit and the guest-exit door) — 32. The native hla build
// carries the door's name interposes over (V8 flags, LuaJIT's mmap, the Valve
// log tap's spew names) on top of its own — 64.
#define KL_INTERPOSE_MAX 64
static struct { const char *name; void *fn; } g_interpose[KL_INTERPOSE_MAX];
static unsigned g_interpose_n;

void kl_interpose(const char *name, void *fn) {
    if (!name || !fn) return;
    if (g_interpose_n >= KL_INTERPOSE_MAX) {
        fprintf(stderr, "  [klepton] interpose table FULL (%d rows) — \"%s\" is "
                        "NOT installed and its hook will never run. Raise "
                        "KL_INTERPOSE_MAX or install fewer probes.\n",
                KL_INTERPOSE_MAX, name);
        return;
    }
    g_interpose[g_interpose_n].name = name;
    g_interpose[g_interpose_n].fn = fn;
    g_interpose_n++;
}

static uint64_t sym_value(kl_image *img, uint32_t idx, const char **name_out) {
    const Elf64_Sym *s = &img->symtab[idx];
    const char *nm = img->strtab + s->st_name;
    if (name_out) *name_out = nm;
    for (unsigned i = 0; i < g_interpose_n; i++)
        if (!strcmp(nm, g_interpose[i].name))
            return (uint64_t)(uintptr_t)g_interpose[i].fn;
    if (s->st_shndx != 0) return (uint64_t)(uintptr_t)(img->base + s->st_value);
    void *ext = kl_shim_lookup(nm);
    // Not a shim: try other already-loaded guest images. Android's linker binds
    // an .so's imports against its DT_NEEDED chain; our chain is the image
    // registry, filled dependencies-first by kl_load_recursive. This is what
    // lets libmain.so's SDL_* imports bind to the guest libSDL3 instead of
    // falling to an unresolved stub.
    if (!ext) ext = kl_guest_sym_global(nm);
    return (uint64_t)(uintptr_t)ext;      // 0 if unresolved
}

// ---------- glibc mode ----------
// A GLIBC image (Valve's Alyx payload, linked by this loader with no host) binds
// in the order a glibc process would, with the personality standing where
// Valve's libc.so.6 would have been:
//
//   1. the personality (kl_glibc_lookup_native) — libc.so.6 / libm.so.6 /
//      ld-linux. FIRST, ahead of every image, and that is load-bearing for one
//      family above all: `hlvr`'s first DT_NEEDED is libmimalloc.so, which
//      exports malloc/free/calloc/realloc/memalign/posix_memalign/strdup... A
//      search that met libmimalloc before the personality would bind half the
//      process to mimalloc's heap and the other half (everything the bionic
//      table allocates for the guest — strdup, getline, fopen's buffers) to
//      Darwin's, and the first cross free is heap corruption (round 5's
//      fontconfig abort under the host was exactly that). NATIVE-PORT.md R6.
//   2. interposes (kl_interpose) — the instrument, as for any image.
//   3. the other GLIBC images, in load order (first definition wins);
//   4. the kept BIONIC images (libalyxsdlcore, libalyxgnsbackend,
//      libc++_shared), after every glibc one: libc++_shared exports the same
//      __cxa_* / _Unwind_* names libstdc++ does, and a glibc image must get
//      libstdc++'s;
//   5. a replaced soname's runtime module (kl_glibc_claim_sym: libvulkan.so.1
//      -> kl_vulkan, libopenxr_loader.so -> kl_openxr), only for an image that
//      NEEDS that soname;
//   6. unresolved: a named stub, as everywhere.
//
// NOT the bionic shim table directly: a name the personality does not list is
// a name whose glibc ABI nobody has checked, and binding it to bionic's answer
// would be a silent wrong rather than a named gap. The personality's GL_SAME
// rows are how a name opts in to bionic's answer.
//
// `provider` (when non-NULL) says which step answered: "glibc", "interpose",
// a provider image's soname, the claiming module's label, or NULL for
// unresolved.
static kl_bind_observer g_bind_observer;
void kl_set_bind_observer(kl_bind_observer fn) { g_bind_observer = fn; }

// The lookup scope for a glibc image's name: by default every registered glibc
// image in load order (one global scope — the A1 sweep); the native door
// installs ld.so's real one (the global scope, then the image's own dlopen
// group) with kl_set_glibc_scope.
static kl_glibc_scope_fn      g_glibc_scope;
static kl_glibc_unresolved_fn g_glibc_unresolved;
void kl_set_glibc_scope(kl_glibc_scope_fn fn) { g_glibc_scope = fn; }
void kl_set_glibc_unresolved(kl_glibc_unresolved_fn fn) { g_glibc_unresolved = fn; }
static void *glibc_scope(kl_image *img, const char *nm, const char **who) {
    return g_glibc_scope ? g_glibc_scope(img, nm, who) : kl_guest_sym_ordered(nm, 1, who);
}

void *kl_interpose_lookup(const char *name) {
    for (unsigned i = 0; name && i < g_interpose_n; i++)
        if (!strcmp(name, g_interpose[i].name)) return g_interpose[i].fn;
    return NULL;
}

static uint64_t sym_value_glibc(kl_image *img, uint32_t idx, const char **name_out,
                                const char **provider) {
    const Elf64_Sym *s = &img->symtab[idx];
    const char *nm = img->strtab + s->st_name;
    const char *who = NULL;
    void *v = NULL;
    if (name_out) *name_out = nm;
    if (s->st_shndx != 0) {
        // Defined here. Interposes still apply (the instrument), and a GNU
        // unique symbol binds to the first definition anywhere — that is its
        // whole meaning: libstdc++'s inline statics must be ONE object.
        for (unsigned i = 0; i < g_interpose_n; i++)
            if (!strcmp(nm, g_interpose[i].name)) {
                if (provider) *provider = "interpose";
                return (uint64_t)(uintptr_t)g_interpose[i].fn;
            }
        void *own = img->base + s->st_value;
        if ((s->st_info >> 4) == STB_GNU_UNIQUE) {
            v = glibc_scope(img, nm, &who);
            if (v) {
                if (v != own) img->stats.unique_shared++;
                if (provider) *provider = who;
                return (uint64_t)(uintptr_t)v;
            }
        }
        // SYMBOL PREEMPTION, as ld.so does it: a reference through the GOT to a
        // default-visibility global the image itself defines binds to the
        // FIRST definition in the lookup scope, not to the image's own copy —
        // unless the image was linked -Bsymbolic (DT_SYMBOLIC / DF_SYMBOLIC:
        // the ffmpeg family, pango, libsteam_api) or the symbol is protected.
        // The personality stands first here for the same reason it does for
        // an import (NATIVE-PORT.md R6): the only payload image defining a
        // name the personality answers is libmimalloc (its malloc family), and
        // its own GOT references then meet the same heap as every caller's.
        if (!img->symbolic && (s->st_other & 3) != STV_PROTECTED
            && (s->st_info >> 4) != STB_LOCAL) {
            if ((v = kl_glibc_lookup_native(nm))) who = "glibc";
            else if ((v = glibc_scope(img, nm, &who)) == own) v = NULL;
            if (v) {
                img->stats.preempted++;
                if (provider) *provider = who;
                return (uint64_t)(uintptr_t)v;
            }
        }
        if (provider) *provider = "self";
        return (uint64_t)(uintptr_t)own;
    }
    if ((v = kl_glibc_lookup_native(nm))) { who = "glibc"; goto out; }
    for (unsigned i = 0; i < g_interpose_n; i++)
        if (!strcmp(nm, g_interpose[i].name)) { v = g_interpose[i].fn; who = "interpose"; goto out; }
    if ((v = glibc_scope(img, nm, &who))) goto out;
    if ((v = kl_guest_sym_ordered(nm, 0, &who))) goto out;
    for (unsigned i = 0; i < img->nneeded; i++)
        if ((v = kl_glibc_claim_sym(img->needed[i], nm, &who))) goto out;
    // Nothing answers a STRONG import: the native door's named stand-ins (a
    // replaced module not written yet, a library the payload does not carry).
    if ((s->st_info >> 4) != STB_WEAK && g_glibc_unresolved
        && (v = g_glibc_unresolved(img, nm, &who))) goto out;
    who = NULL;
out:
    if (provider) *provider = who;
    return (uint64_t)(uintptr_t)v;
}

static int apply_relocs(kl_image *img, const Elf64_Rela *r, size_t count) {
    for (size_t i = 0; i < count; i++, r++) {
        uint64_t *slot = (uint64_t *)(img->base + r->r_offset);
        uint32_t type = ELF64_R_TYPE(r->r_info);
        uint32_t sidx = ELF64_R_SYM(r->r_info);
        const char *nm = NULL;
        switch (type) {
        case R_AARCH64_RELATIVE:
            *slot = (uint64_t)(uintptr_t)img->base + (uint64_t)r->r_addend;
            img->stats.relative++;
            break;
        case R_AARCH64_ABS64:
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT: {
            const char *prov = NULL;
            uint64_t v = img->glibc ? sym_value_glibc(img, sidx, &nm, &prov)
                                    : sym_value(img, sidx, &nm);
            // Every import, and every definition that preemption bound elsewhere.
            if (img->glibc && g_bind_observer
                && (img->symtab[sidx].st_shndx == 0 || (prov && strcmp(prov, "self"))))
                g_bind_observer(img, nm, prov, v,
                                (img->symtab[sidx].st_info >> 4) == STB_WEAK);
            if (!v && (img->symtab[sidx].st_info >> 4) == STB_WEAK
                   && img->symtab[sidx].st_shndx == 0) {
                // A WEAK undefined must stay NULL — that is the whole mechanism
                // by which a guest asks whether an optional symbol exists. The
                // call site null-tests the slot, so an abort trampoline here
                // answers "present" and the guest then calls it. Beat Saber 1.40
                // is where this stopped being theoretical: libunity weak-imports
                // the entire AMediaCodec / AImageReader / AHardwareBuffer family
                // to probe for NDK video decode, and every name kl_mediandk.c
                // does not implement would have read as supported.
                //
                // Reported, not silent: this trades an abort naming the symbol
                // for a NULL call naming nothing, so record_weak keeps the list.
                img->stats.imports_weak_null++;
                record_weak(img, nm);
            } else if (!v) {
                // Dedupe: libil2cpp has 711k relocations; per-site logging would flood.
                img->stats.imports_missing++;
                record_missing(img, nm);
                v = (uint64_t)(uintptr_t)unresolved_stub(nm);
            } else if (img->symtab[sidx].st_shndx == 0) {
                img->stats.imports_bound++;
            }
            *slot = v + (uint64_t)r->r_addend;
            if (type == R_AARCH64_ABS64) img->stats.abs64++;
            else if (type == R_AARCH64_GLOB_DAT) img->stats.glob_dat++;
            else img->stats.jump_slot++;
            break;
        }
        case R_AARCH64_TLSDESC: {
            // The relocation covers a 16-byte descriptor: we own both words, and
            // the addend is the variable's offset inside the DEFINING module's
            // TLS block. Every one seen so far is sym 0 — a C++ `thread_local`
            // with internal linkage — and a symbol that IS named must still be
            // defined here, because a block belonging to another module is a
            // different question (whose PT_TLS? loaded when?) and answering it
            // by guessing would put a live thread's variable at someone else's
            // address. Refuse that out loud instead.
            uint64_t off = (uint64_t)r->r_addend;
            if (sidx) {
                if (img->symtab[sidx].st_shndx == 0) {
                    err("TLSDESC against undefined symbol '%s' at %#llx — "
                        "cross-module TLS is not served",
                        img->strtab + img->symtab[sidx].st_name,
                        (unsigned long long)r->r_offset);
                    return -1;
                }
                off += img->symtab[sidx].st_value;
            }
            if (!img->has_tls) {
                err("TLSDESC at %#llx but the image has no PT_TLS",
                    (unsigned long long)r->r_offset);
                return -1;
            }
            if (off >= img->tls_memsz) {
                err("TLSDESC at %#llx wants offset %#llx of a %#llx-byte TLS block",
                    (unsigned long long)r->r_offset, (unsigned long long)off,
                    (unsigned long long)img->tls_memsz);
                return -1;
            }
            if (!img->tlsmod) {
                img->tlsmod = kl_tls_register(img->path, img->tls_init,
                                              img->tls_filesz, img->tls_memsz,
                                              img->tls_align);
                // A descriptor whose resolver cannot find its per-thread slot
                // resolves to a WRONG address, not to nothing — so a failed
                // registration fails the load.
                if (!img->tlsmod) { err("TLSDESC needs per-thread TLS and it is unavailable"); return -1; }
            }
            kl_tlsdesc_arg *a = calloc(1, sizeof *a);   // lives as long as the image
            if (!a) { err("out of memory for a TLS descriptor"); return -1; }
            a->mod = img->tlsmod;
            a->off = off;
            slot[0] = (uint64_t)(uintptr_t)&kl_tlsdesc_resolver;
            slot[1] = (uint64_t)(uintptr_t)a;
            img->stats.tlsdesc++;
            break;
        }
        case R_AARCH64_TLS_TPREL64: {
            // The image's own static TLS, at a constant TP offset: the block's
            // home is a run of TSD slots (kl_tls_static_block), and the slot
            // gets that home's offset plus the variable's. Like TLSDESC, a
            // NAMED symbol must be this image's own — another module's static
            // block is not served, and guessing would alias a live variable.
            uint64_t off = (uint64_t)r->r_addend;
            if (sidx) {
                if (img->symtab[sidx].st_shndx == 0) {
                    err("TPREL64 against undefined symbol '%s' at %#llx — another "
                        "module's static TLS is not served",
                        img->strtab + img->symtab[sidx].st_name,
                        (unsigned long long)r->r_offset);
                    return -1;
                }
                off += img->symtab[sidx].st_value;
            }
            if (!img->has_tls || off >= img->tls_memsz) {
                err("TPREL64 at %#llx wants offset %#llx of a %#llx-byte TLS block",
                    (unsigned long long)r->r_offset, (unsigned long long)off,
                    (unsigned long long)img->tls_memsz);
                return -1;
            }
            if (!img->stls_off) {
                img->stls_off = kl_tls_static_block(img->path, img->tls_init,
                                                    img->tls_filesz, img->tls_memsz,
                                                    img->tls_align);
                if (img->stls_off <= 0) {
                    img->stls_off = 0;
                    err("TPREL64 needs a static TLS block and none could be placed");
                    return -1;
                }
                kl_thread_init_hook(kl_tls_static_seed);   // every later guest thread
            }
            *slot = (uint64_t)img->stls_off + off;
            img->stats.tprel++;
            break;
        }
        default:
            err("unhandled relocation type %u at %#llx", type, (unsigned long long)r->r_offset);
            return -1;
        }
    }
    return 0;
}

// ---------- RELR ----------
// Every entry is a 64-bit word. An EVEN word is an address: relocate there, and
// remember it as the place the next bitmap starts from. An ODD word is a
// bitmap of the next 63 words after that place — bit 1 is the first of them —
// after which the place advances by 63 words. Every site is an implicit
// R_AARCH64_RELATIVE: *p += load bias. Nothing else can be encoded this way,
// which is why the format needs no type field.
static int apply_relr(kl_image *img, const uint64_t *entries, size_t count) {
    uint64_t *where = NULL;
    for (size_t i = 0; i < count; i++) {
        uint64_t e = entries[i];
        if ((e & 1) == 0) {
            where = (uint64_t *)(img->base + e);
            *where++ += (uint64_t)(uintptr_t)img->base;
            img->stats.relative++;
        } else {
            if (!where) { err("RELR bitmap with no preceding address entry"); return -1; }
            uint64_t *p = where;
            for (e >>= 1; e; e >>= 1, p++)
                if (e & 1) { *p += (uint64_t)(uintptr_t)img->base; img->stats.relative++; }
            where += 63;
        }
    }
    return 0;
}

// ---------- Android packed relocations (APS2) ----------
//
// WHY THIS EXISTS, because "the loader was missing a relocation format" is the
// kind of gap that reads as trivia until you see what it cost.
//
// Eleven Table Tennis' avatar selection screen could not be passed: NEXT did
// nothing, forever. The chain ends here. `libovrbody.so` — the Meta Avatar
// SDK's native half — is linked with `--pack-dyn-relocs=android`, so it carries
// DT_ANDROID_RELA instead of DT_RELA. bind_dynamic() looked for DT_RELA, found
// none, applied ZERO relocations and returned success; the first DT_INIT_ARRAY
// entry was then an unrebased ELF vaddr, and calling it jumped to 0x805338 and
// took the process. So the library was left out of the staged set, which left
// `OvrAvatarManager.initialized:False`, which left the panel's preview avatar
// never created, which is the null its unmount dereferences. Four layers, one
// missing decoder. (ETT STATUS.md round 27.)
//
// THE FORMAT, as bionic writes and reads it: the magic "APS2", then a stream of
// signed LEB128 values — relocation count, initial r_offset, then groups of
// { size, flags, [offset delta], [r_info], [addend] } where the flags say which
// of the three fields is shared by the whole group and which repeats per entry.
// r_offset and r_addend are CUMULATIVE across the whole stream; r_info is not.
// A grouped addend is added once per entry, not once per group.
#define APS2_GROUPED_BY_INFO         1
#define APS2_GROUPED_BY_OFFSET_DELTA 2
#define APS2_GROUPED_BY_ADDEND       4
#define APS2_GROUP_HAS_ADDEND        8

// One signed LEB128, advancing *p. `end` bounds the read so a truncated or
// mis-pointed stream stops rather than walking off the mapping.
static int64_t sleb128(const uint8_t **p, const uint8_t *end) {
    int64_t  v = 0;
    unsigned shift = 0;
    uint8_t  b = 0;
    while (*p < end) {
        b = *(*p)++;
        v |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    if (shift < 64 && (b & 0x40)) v |= -((int64_t)1 << shift);   // sign-extend
    return v;
}

// Expand a DT_ANDROID_RELA blob into a plain Elf64_Rela array. Returns the
// array (caller frees) and writes the count; NULL on a malformed stream.
static Elf64_Rela *decode_android_relocs(const uint8_t *blob, size_t size, size_t *count_out) {
    const uint8_t *p = blob, *end = blob + size;
    if (size < 4 || p[0] != 'A' || p[1] != 'P' || p[2] != 'S' || p[3] != '2') {
        err("DT_ANDROID_RELA blob is not APS2 (magic %02x%02x%02x%02x)",
            size > 0 ? blob[0] : 0, size > 1 ? blob[1] : 0,
            size > 2 ? blob[2] : 0, size > 3 ? blob[3] : 0);
        return NULL;
    }
    p += 4;

    int64_t total = sleb128(&p, end);
    if (total <= 0 || (uint64_t)total > (1u << 24)) {
        err("DT_ANDROID_RELA claims %lld relocations", (long long)total);
        return NULL;
    }
    Elf64_Rela *out = calloc((size_t)total, sizeof *out);
    if (!out) { err("out of memory for %lld packed relocations", (long long)total); return NULL; }

    int64_t offset = sleb128(&p, end);
    int64_t addend = 0;
    size_t  n = 0;

    while (n < (size_t)total) {
        if (p >= end) { err("DT_ANDROID_RELA truncated after %zu of %lld", n, (long long)total); free(out); return NULL; }
        int64_t gsize  = sleb128(&p, end);
        int64_t gflags = sleb128(&p, end);
        if (gsize <= 0 || (uint64_t)(n + (size_t)gsize) > (uint64_t)total) {
            err("DT_ANDROID_RELA group of %lld overruns the declared count", (long long)gsize);
            free(out);
            return NULL;
        }
        int64_t gdelta = 0, ginfo = 0, gaddend = 0;
        if (gflags & APS2_GROUPED_BY_OFFSET_DELTA) gdelta = sleb128(&p, end);
        if (gflags & APS2_GROUPED_BY_INFO)         ginfo  = sleb128(&p, end);
        if ((gflags & (APS2_GROUP_HAS_ADDEND | APS2_GROUPED_BY_ADDEND))
                   == (APS2_GROUP_HAS_ADDEND | APS2_GROUPED_BY_ADDEND))
            gaddend = sleb128(&p, end);
        else if (!(gflags & APS2_GROUP_HAS_ADDEND))
            addend = 0;

        for (int64_t i = 0; i < gsize; i++, n++) {
            offset += (gflags & APS2_GROUPED_BY_OFFSET_DELTA) ? gdelta : sleb128(&p, end);
            int64_t info = (gflags & APS2_GROUPED_BY_INFO) ? ginfo : sleb128(&p, end);
            if (gflags & APS2_GROUP_HAS_ADDEND)
                addend += (gflags & APS2_GROUPED_BY_ADDEND) ? gaddend : sleb128(&p, end);
            out[n].r_offset = (uint64_t)offset;
            out[n].r_info   = (uint64_t)info;
            out[n].r_addend = addend;
        }
    }
    *count_out = n;
    return out;
}

// Walk PT_DYNAMIC and bind the image: symtab/strtab, DT_INIT_ARRAY, and every
// relocation. Shared by both loaders — the mmap path below and kl_load_dylib()
// — because this half is identical either way. It is what lets klepton-ld emit a
// dylib with ZERO dyld fixups: nothing here needs dyld's help, and no relocation
// target is in __TEXT (measured across every guest library), so the guest image
// can be mapped read-execute by dyld and still relocate correctly.
//
// `lo` is the lowest PT_LOAD vaddr, so `img->base + p_vaddr - lo` addresses the
// mapped copy in both cases (it is 0 for every guest library here).
// ---------- the GNU hash table ----------
// { nbuckets, symoffset, bloom_size, bloom_shift, bloom[bloom_size] (64-bit
// words on ELF64), buckets[nbuckets], chain[] }. Symbols below `symoffset` are
// not hashed (undefined and local); chain[i - symoffset] is symbol i's hash
// with bit 0 set on the LAST entry of each bucket's run.
static uint32_t gnu_hash_of(const char *s) {
    uint32_t h = 5381;
    for (; *s; s++) h = h * 33 + (uint8_t)*s;
    return h;
}

// The .dynsym count, which DT_GNU_HASH encodes only implicitly: the highest
// bucket start, walked to the end of its chain, is the last hashed symbol.
// This is how glibc's own ld.so sizes the table when there is no DT_HASH, and
// it is the only honest answer for a translated image (its section headers
// are not mapped). The old fallback — the gap between DT_SYMTAB and DT_STRTAB —
// is the wrong-layout bug kl_sym's comment describes.
static size_t gnu_hash_nsyms(const uint32_t *h) {
    uint32_t nbuckets = h[0], symoffset = h[1], bloom_size = h[2];
    const uint32_t *buckets = (const uint32_t *)((const uint64_t *)(h + 4) + bloom_size);
    const uint32_t *chain = buckets + nbuckets;
    uint32_t last = 0;
    for (uint32_t i = 0; i < nbuckets; i++) if (buckets[i] > last) last = buckets[i];
    if (last < symoffset) return symoffset;
    while (!(chain[last - symoffset] & 1)) last++;
    return (size_t)last + 1;
}

static const Elf64_Sym *gnu_hash_find(const kl_image *img, const char *name) {
    const uint32_t *h = img->gnu_hash;
    uint32_t nbuckets = h[0], symoffset = h[1], bloom_size = h[2], shift = h[3];
    if (!nbuckets || !bloom_size) return NULL;
    const uint64_t *bloom = (const uint64_t *)(h + 4);
    const uint32_t *buckets = (const uint32_t *)(bloom + bloom_size);
    const uint32_t *chain = buckets + nbuckets;
    uint32_t hv = gnu_hash_of(name);
    uint64_t word = bloom[(hv / 64) % bloom_size];
    uint64_t mask = (1ull << (hv % 64)) | (1ull << ((hv >> shift) % 64));
    if ((word & mask) != mask) return NULL;
    uint32_t i = buckets[hv % nbuckets];
    if (i < symoffset) return NULL;
    for (;; i++) {
        uint32_t ch = chain[i - symoffset];
        if ((ch | 1) == (hv | 1)) {
            const Elf64_Sym *s = &img->symtab[i];
            // glibc mode: a HIDDEN version (versym bit 15) is a compatibility
            // copy only an explicitly versioned reference may bind; an
            // unversioned search takes the default one beside it.
            if (s->st_shndx != 0 && !strcmp(img->strtab + s->st_name, name)
                && !(img->glibc && img->versym && (img->versym[i] & 0x8000)))
                return s;
        }
        if (ch & 1) break;
    }
    return NULL;
}

// `apply` 0 parses the dynamic section only (symbols, NEEDED, SONAME) and
// relocates nothing: a replaced library's real file, mapped so its export list
// can say which names belong to it (kl_map_dylib_unbound), and never bound.
static int bind_dynamic(kl_image *img, const Elf64_Phdr *ph, int phnum, uint64_t lo,
                        int apply) {
    const Elf64_Rela *rela = NULL, *jmprel = NULL;
    const uint8_t    *packed = NULL;
    const uint64_t   *relr = NULL;
    size_t relasz = 0, pltsz = 0, packedsz = 0, relrsz = 0, relrent = 8;
    uint64_t needed_off[48], soname_off = 0;
    unsigned nneeded = 0;
    int has_soname = 0;

    // PT_TLS first: a TLSDESC relocation below needs the block it describes, and
    // the program headers are not visited again after this loop.
    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_TLS) continue;
        img->tls_init   = img->base + ph[i].p_vaddr - lo;
        img->tls_filesz = ph[i].p_filesz;
        img->tls_memsz  = ph[i].p_memsz;
        img->tls_align  = ph[i].p_align ? ph[i].p_align : 8;
        img->has_tls    = 1;
    }

    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_DYNAMIC) continue;
        const Elf64_Dyn *dp = (const Elf64_Dyn *)(img->base + ph[i].p_vaddr - lo);
        for (; dp->d_tag != DT_NULL; dp++) {
            switch (dp->d_tag) {
            case DT_SYMTAB: img->symtab = (Elf64_Sym *)(img->base + dp->d_val); break;
            case DT_STRTAB: img->strtab = (const char *)(img->base + dp->d_val); break;
            // The authoritative .dynsym entry count. DT_HASH's second word is
            // nchain, and the chain array is parallel to the symbol table, so
            // nchain IS the number of symbols — this is how a real dynamic
            // linker knows, and it needs no section headers.
            case DT_HASH:   img->nsyms = ((const uint32_t *)(img->base + dp->d_val))[1]; break;
            case DT_RELA:   rela   = (const Elf64_Rela *)(img->base + dp->d_val); break;
            case DT_RELASZ: relasz = dp->d_val; break;
            case DT_ANDROID_RELA:   packed   = (const uint8_t *)(img->base + dp->d_val); break;
            case DT_ANDROID_RELASZ: packedsz = dp->d_val; break;
            // Both numberings (see the DT_RELR block above); an image carries
            // one or the other, never both.
            case DT_RELR: case DT_ANDROID_RELR:
                relr = (const uint64_t *)(img->base + dp->d_val); break;
            case DT_RELRSZ: case DT_ANDROID_RELRSZ:  relrsz  = dp->d_val; break;
            case DT_RELRENT: case DT_ANDROID_RELRENT: relrent = dp->d_val; break;
            case DT_JMPREL: jmprel = (const Elf64_Rela *)(img->base + dp->d_val); break;
            case DT_PLTRELSZ: pltsz = dp->d_val; break;
            case DT_INIT:         img->init_fn = (void (*)(void))(img->base + dp->d_val); break;
            case DT_SYMBOLIC:     img->symbolic = 1; break;
            case DT_FLAGS:        if (dp->d_val & DF_SYMBOLIC) img->symbolic = 1; break;
            case DT_INIT_ARRAY:   img->init_array = (void (**)(void))(img->base + dp->d_val); break;
            case DT_INIT_ARRAYSZ: img->init_count = dp->d_val / sizeof(void *); break;
            case DT_GNU_HASH: img->gnu_hash = (const uint32_t *)(img->base + dp->d_val); break;
            case DT_VERSYM:   img->versym = (const uint16_t *)(img->base + dp->d_val); break;
            case DT_VERNEED:  img->verneed = img->base + dp->d_val; break;
            case DT_VERNEEDNUM: img->verneednum = (unsigned)dp->d_val; break;
            case DT_SONAME:   soname_off = dp->d_val; has_soname = 1; break;
            case DT_NEEDED:
                if (nneeded < sizeof needed_off / sizeof *needed_off)
                    needed_off[nneeded++] = dp->d_val;
                else if (img->glibc) {
                    // Past the table a dependency would vanish without a word:
                    // refuse the image by name instead (the payload's most is 13).
                    // glibc mode only — the list is what its claims read; a
                    // bionic image never consults it, so it loads as before.
                    err("more than %zu DT_NEEDED entries — raise kl_image.needed",
                        sizeof needed_off / sizeof *needed_off);
                    return -1;
                }
                break;
            }
        }
    }
    if (!img->symtab || !img->strtab) { err("missing DT_SYMTAB/DT_STRTAB"); return -1; }
    if (has_soname) img->soname = img->strtab + soname_off;
    for (unsigned i = 0; i < nneeded; i++) img->needed[i] = img->strtab + needed_off[i];
    img->nneeded = nneeded;
    // No DT_HASH: size .dynsym from the GNU table. The ELF path overrides this
    // from the section headers afterwards when it has them. glibc images only
    // on driver: a bionic guest keeps the count it has always had (the
    // promotion from hla changed no existing target's symbol search).
    if (!img->nsyms && img->gnu_hash && img->glibc) img->nsyms = gnu_hash_nsyms(img->gnu_hash);
    if (!apply) return 0;
    img->bound = 1;

    // REFUSE rather than relocate nothing. A table we cannot read is
    // indistinguishable, downstream, from an image that needed no relocations at
    // all — and the difference only shows up as a jump to an unrebased address
    // several seconds later, in whatever ran first. RELR's entry size is the
    // one thing about it that could ever change, so it is checked rather than
    // assumed: a different stride would silently relocate the wrong words.
    if (relr && relrent != 8) {
        err("DT_RELRENT is %zu, not 8 — this loader reads 8-byte RELR entries only", relrent);
        return -1;
    }
    if (relr && apply_relr(img, relr, relrsz / 8) != 0) return -1;

    if ((rela   && apply_relocs(img, rela,   relasz / sizeof(Elf64_Rela)) != 0) ||
        (jmprel && apply_relocs(img, jmprel, pltsz  / sizeof(Elf64_Rela)) != 0))
        return -1;

    if (packed) {
        size_t      n = 0;
        Elf64_Rela *r = decode_android_relocs(packed, packedsz, &n);
        if (!r) return -1;
        int rc = apply_relocs(img, r, n);
        free(r);
        if (rc != 0) return -1;
    }

    // A static-TLS block's template is relocated by now: give the loading
    // thread its copy (later threads get theirs from kl_thread_init).
    if (img->stls_off) kl_tls_static_seed();

    // Say what was left NULL, by name, once per image.
    //
    // A missing STRONG import gets a named stub, so calling it aborts with the
    // symbol in the message and the log reads like a bug report. A weak one
    // gets nothing — deliberately, that is what weak means — and calling it is
    // `br` to zero: pc=0, EXC_BAD_ACCESS, no frame of ours in the crash report,
    // and not one line anywhere saying which symbol it was. Arkham Shadow's
    // cape-glide tutorial died exactly that way on AMediaDataSource_new, and
    // the only reason it took a disassembly of the PLT stub and the ELF's own
    // relocation table to find out is that this list existed and was never
    // printed. kl_weak_imports() had no callers at all.
    //
    // It is one line per image and it is the difference between a round and a
    // grep, so it is unconditional rather than behind a debug flag.
    if (img->weak_n) {
        fprintf(stderr, "  [klepton] %s: %u weak import(s) left NULL — the guest "
                        "must null-test each before calling it:\n", img->path,
                img->weak_n);
        for (unsigned i = 0; i < img->weak_n; i++)
            fprintf(stderr, "  [klepton]     %s\n", img->weak[i]);
    }
    if (img->missing_n)
        fprintf(stderr, "  [klepton] %s: %u unresolved import(s), each behind a "
                        "stub that aborts by name\n", img->path, img->missing_n);
    return 0;
}

// ---------- load a klepton-ld-translated Mach-O dylib ----------
// The shipping loader. dyld maps and (on device) AMFI validates the guest text,
// so nothing here is ever RWX and no guest byte is written by us — the TLS
// rewrite and the x18 veneers were applied offline by klepton-ld.
//
// The guest ELF image is embedded verbatim in the dylib at a constant shift, and
// found through the __TEXT,__klelf section rather than an exported symbol: that
// is what lets klepton-ld skip the exports trie entirely. getsectiondata()
// applies the image's slide for us.
//
// The mach_header is located by matching the dylib against dyld's loaded-image
// list. dlsym would be the obvious route and is precisely what we cannot use —
// the translated dylib deliberately exports nothing.
// `mode`: 0 an ordinary (bionic) guest; KL_LOAD_GLIBC binds in glibc's order
// (sym_value_glibc); KL_LOAD_UNBOUND maps and reads the symbol table only and
// applies NO relocation — a replaced library's real file, kept for its export
// list, which nothing may call (its GOT is unrelocated).
enum { KL_LOAD_GLIBC = 1, KL_LOAD_UNBOUND = 2 };
static kl_image *load_dylib(const char *path, int mode);

kl_image *kl_load_dylib(const char *path) { return load_dylib(path, 0); }
kl_image *kl_load_dylib_glibc(const char *path) { return load_dylib(path, KL_LOAD_GLIBC); }
kl_image *kl_map_dylib_unbound(const char *path) { return load_dylib(path, KL_LOAD_UNBOUND); }
int       kl_image_is_glibc(const kl_image *img) { return img && img->glibc; }
const char *kl_image_soname(const kl_image *img) { return img ? img->soname : NULL; }
const char *const *kl_image_needed(const kl_image *img, unsigned *count) {
    if (count) *count = img ? img->nneeded : 0;
    return img ? img->needed : NULL;
}
int64_t   kl_image_static_tls(const kl_image *img) { return img ? img->stls_off : 0; }

// The second half of a two-phase load: relocate an image kl_map_dylib_unbound
// mapped. glibc's ld.so maps a whole NEEDED closure first and relocates it
// afterwards, which is what lets a dependency CYCLE bind both ways (the
// payload's loader-cycle fixture is exactly that) and what makes the global
// search order the load order rather than the order relocation happened in.
int kl_bind_mapped(kl_image *img) {
    if (!img || img->bound) return img ? 0 : -1;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)img->base;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(img->base + eh->e_phoff);
    uint64_t lo = UINT64_MAX;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
    return bind_dynamic(img, ph, eh->e_phnum, lo == UINT64_MAX ? 0 : lo, 1);
}

// Which FILE an undefined import's symbol version names (its DT_VERNEED entry:
// `__libc_start_main@GLIBC_2.34` -> "libc.so.6"), or NULL for an unversioned
// import. This is the one place an ELF records which library a name was
// linked against, so it is how the native door buckets what is left.
const char *kl_import_version_file(const kl_image *img, const char *name) {
    if (!img || !img->versym || !img->verneed || !name) return NULL;
    size_t n = img->nsyms;
    const char *lo_p = (const char *)img->base, *hi_p = lo_p + img->span;
    for (size_t i = 0; i < n; i++) {
        const Elf64_Sym *s = &img->symtab[i];
        if ((const char *)(s + 1) > hi_p) break;
        if (s->st_shndx != 0 || !s->st_name) continue;
        const char *nm = img->strtab + s->st_name;
        if (nm < lo_p || nm >= hi_p || strcmp(nm, name)) continue;
        uint16_t ver = img->versym[i] & 0x7fff;
        if (ver < 2) return NULL;                    // local / global: unversioned
        const uint8_t *vn = img->verneed;
        for (unsigned k = 0; k < img->verneednum && vn; k++) {
            uint16_t cnt = *(const uint16_t *)(vn + 2);
            uint32_t file = *(const uint32_t *)(vn + 4), aux = *(const uint32_t *)(vn + 8);
            uint32_t next = *(const uint32_t *)(vn + 12);
            const uint8_t *va = vn + aux;
            for (unsigned a = 0; a < cnt && va; a++) {
                if (*(const uint16_t *)(va + 6) == ver) return img->strtab + file;
                uint32_t an = *(const uint32_t *)(va + 12);
                va = an ? va + an : NULL;
            }
            vn = next ? vn + next : NULL;
        }
        return NULL;
    }
    return NULL;
}

static kl_image *load_dylib(const char *path, int mode) {
    char real[PATH_MAX];
    if (!realpath(path, real)) snprintf(real, sizeof real, "%s", path);

    void *h = dlopen(real, RTLD_NOW | RTLD_LOCAL);
    if (!h) { err("dlopen %s: %s", real, dlerror()); return NULL; }

    const struct mach_header_64 *mh = NULL;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *nm = _dyld_get_image_name(i);
        char nr[PATH_MAX];
        if (!nm) continue;
        if (!realpath(nm, nr)) snprintf(nr, sizeof nr, "%s", nm);
        if (strcmp(nr, real) == 0) {
            mh = (const struct mach_header_64 *)_dyld_get_image_header(i);
            break;
        }
    }
    if (!mh) { err("%s loaded but not found in dyld's image list", real);
               dlclose(h); return NULL; }

    unsigned long secsz = 0;
    uint8_t *elf = getsectiondata(mh, "__TEXT", "__klelf", &secsz);
    if (!elf) { err("%s has no __TEXT,__klelf section — not a klepton-ld image", real);
                dlclose(h); return NULL; }

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)elf;
    if (memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0 || eh->e_machine != 183) {
        err("%s: __klelf does not hold an AArch64 ELF64 image", real);
        dlclose(h); return NULL;
    }

    kl_image *img = calloc(1, sizeof *img);
    img->base      = elf;
    img->dl_handle = h;
    img->glibc     = (mode & (KL_LOAD_GLIBC | KL_LOAD_UNBOUND)) != 0;
    snprintf(img->path, sizeof img->path, "%s", real);

    const Elf64_Phdr *ph = (const Elf64_Phdr *)(elf + eh->e_phoff);
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) {
            if (ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
            if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
        }
    if (lo == UINT64_MAX) { err("%s: embedded image has no PT_LOAD", real);
                            free(img); dlclose(h); return NULL; }
    img->span = (size_t)(hi - lo);

    if (bind_dynamic(img, ph, eh->e_phnum, lo, !(mode & KL_LOAD_UNBOUND)) != 0) {
        free(img); dlclose(h); return NULL;
    }

    // The TLS rewrite and the x18 veneers were applied offline, so there is
    // nothing left in this image to count — which is precisely why klepton-ld
    // records what it did. Reporting a computed 0 here would read as "this
    // library never needed any", not "this was handled at translation time".
    unsigned long statsz = 0;
    const klx_stat_section *rec =
        (const klx_stat_section *)getsectiondata(mh, "__TEXT", "__klstat", &statsz);
    if (rec && (statsz < sizeof *rec || rec->magic != KLX_STAT_MAGIC)) {
        // A record we cannot read is not a record we may ignore: the TSD-slot
        // check below is the only thing standing between a stale translation
        // and seventeen thousand veneers indexing somebody else's per-thread
        // state. An older klepton-ld wrote a shorter record with a different
        // magic, and both would have failed the test above silently.
        err("%s carries a __klstat record this runtime does not recognise "
            "(magic %#x, %lu bytes; expected %#x, %zu) — re-translate it with "
            "this tree's klepton-ld", real, rec->magic, statsz,
            KLX_STAT_MAGIC, sizeof *rec);
        free(img); dlclose(h); return NULL;
    }
    if (rec) {
        img->stats.tls_rewrites = rec->tls_rewrites;
        img->stats.x18_sites    = rec->sites;
        img->stats.x18_patched  = rec->patched;
        img->stats.x18_refused  = rec->refused;
        img->stats.ctr_sites    = rec->ctr_sites;
        img->stats.ctr_patched  = rec->ctr_patched;

        // The veneers carry a TSD slot chosen at translation time. If this
        // process cannot have that slot, every one of them reads and writes
        // somebody else's per-thread state — wrong in both directions and
        // silent. It is baked into the text, so there is no recovering: fail
        // here, at load, rather than as a wrong answer later.
        if (rec->patched) {
            if (rec->slot != KLX_TSD_SLOT) {
                err("%s was translated against TSD slot %u but this runtime uses "
                    "%d — rebuild it with a matching klepton-ld",
                    real, rec->slot, KLX_TSD_SLOT);
                free(img); dlclose(h); return NULL;
            }
            if (kl_x18_init() != 0) {
                err("%s needs x18 veneering but TSD slot %d is unavailable in this "
                    "process", real, KLX_TSD_SLOT);
                free(img); dlclose(h); return NULL;
            }
        }
        // WHAT THE TRANSLATION DID, on the record, on the device.
        //
        // Round 20 of pttr. Launch #19 was a SIGKILL with no crash report, no
        // JetsamEvent and no fault report — the signature of a `br x18`
        // branching through a zeroed shadow, which is a codesigning kill that
        // cannot report itself. Ruling that class in or out needs one number:
        // how many sites this target's veneer pass REFUSED. It was recorded by
        // klepton-ld, it was read into img->stats right above, and it was
        // printed NOWHERE — so the question could only be asked of `make check`
        // on the host, which patches at runtime and is a different pass over a
        // different set of libraries than the device ever runs.
        //
        // Refusals first and unconditionally when non-zero, because a refused
        // site that WRITES x18 leaves the shadow stale and poisons every later
        // read of it. The clean case gets one quiet line too: "0 refused" is
        // the evidence that matters when a crash has no other evidence at all,
        // and a line that only appears when something is wrong cannot be used
        // to prove that nothing is.
        if (rec->sites || rec->refused)
            fprintf(stderr, "  [klepton] %s: x18 translated offline — %u site(s), "
                            "%u patched, %u REFUSED%s\n",
                    real, rec->sites, rec->patched, rec->refused,
                    rec->refused ? "  <-- refused sites read Darwin's x18; a "
                                   "refused WRITE poisons every later read"
                                 : "");

        // The CTR_EL0 constant is self-contained — the veneer answers it and
        // nothing here has to agree — so a difference is reported rather than
        // refused. It is reported at all because "the CTR_EL0 value in this
        // dylib is not the one this tree defaults to" is otherwise invisible,
        // and the A/B for it (KL_CTR_EL0) is baked at translation time.
        if (rec->ctr_patched && rec->ctr_value != KLX_CTR_EL0_VALUE)
            fprintf(stderr, "  [klepton] %s: %u CTR_EL0 veneer(s) answer %#x, not "
                            "this tree's default %#x\n",
                    real, rec->ctr_patched, rec->ctr_value, KLX_CTR_EL0_VALUE);
    }

    if (kl_env_on("KL_TRACE_IMAGES", 0))
        fprintf(stderr, "  [klepton] dylib %s: mach header %p, guest image %p span %#zx\n",
                real, (const void *)mh, (void *)img->base, img->span);
    return img;
}

// Pick a loader for a guest library. With KL_DYLIB_DIR set, a klepton-ld
// translation of this library is used when one has been built, and the ELF
// otherwise — so the boot chain can run *mixed* while the offline x18 veneer
// pass is still missing. Three of the five guest libraries have 0 x18 sites and
// translate today; libunity (14,536) and libil2cpp (3,083) do not, and
// klepton-ld refuses them rather than emitting something silently wrong.
//
// A missing translation is normal and quiet. A translation that is *present and
// fails* is not: it returns NULL and says so, because falling back to the ELF
// there would hide exactly the regression this path exists to catch.
// The translation of `path`, if KL_DYLIB_DIR names one. Factored out because two
// callers must agree about it: kl_load_auto, which loads it, and kl_can_load,
// which answers "does this library exist" for the guest. They disagreed once and
// it cost a device run — see kl_can_load.
static int dylib_candidate(const char *path, char *out, size_t cap, char *name_out,
                           size_t name_cap) {
    const char *dir = kl_env_str("KL_DYLIB_DIR", NULL);
    if (!dir || !*dir) return 0;
    const char *stem = strrchr(path, '/');
    stem = stem ? stem + 1 : path;
    char name[256];
    snprintf(name, sizeof name, "%s", stem);
    char *dot = strstr(name, ".so");
    if (dot) *dot = 0;
    if (name_out) snprintf(name_out, name_cap, "%s", name);

    // Two layouts, because the host and the bundle disagree about what a
    // translated library looks like. `make dylibs` writes bare .dylib files; an
    // app bundle carries frameworks, which is also the layout AMFI accepts —
    // Xcode code-signs what it embeds in Frameworks/, and a loose Mach-O
    // elsewhere in the bundle is only sealed, not signed. Same image either
    // way.
    snprintf(out, cap, "%s/%s.dylib", dir, name);
    if (access(out, R_OK) == 0) return 1;
    snprintf(out, cap, "%s/%s.framework/%s", dir, name, name);
    return access(out, R_OK) == 0;
}

kl_image *kl_load_auto(const char *path) {
    char cand[1024], name[256];
    if (dylib_candidate(path, cand, sizeof cand, name, sizeof name)) {
        kl_image *img = kl_load_dylib(cand);
        if (!img) {
            fprintf(stderr, "  [klepton] %s: translated dylib present but failed "
                            "to load: %s\n", cand, kl_error());
            return NULL;
        }
        fprintf(stderr, "  [klepton] %s: loaded as a translated dylib\n", name);
        return img;
    }
    return kl_load(path);
}

// Could kl_load_auto load a guest library at `path`? Anything that answers an
// existence question about a guest library on the guest's behalf must ask this
// and not stat(path) — because on device the ELF tree is deliberately NOT in the
// bundle (only the translations are, which is 80 MB saved), so the .so path is a
// name the loader resolves rather than a file that exists.
//
// A stat(path) here has ClassLoader.findLibrary() return null for libil2cpp, so
// Unity never attempts the dlopen that would have succeeded, and puts up
// "Failed to load Il2CPP." three layers away from the stat that caused it. The
// path stays the .so path in the answer, so there is still exactly one place
// that resolves it: here.
int kl_can_load(const char *path) {
    char cand[1024];
    if (dylib_candidate(path, cand, sizeof cand, NULL, 0)) return 1;
    return access(path, R_OK) == 0;
}

// The file that IS this guest library on this platform. `kl_can_load` answers
// the yes/no; this one hands back the path, for the caller that has to do more
// than decide — stat(2), notably: a guest asking whether its own libdir holds
// libfoo.so wants a struct back, and on device the only real file behind that
// name is the translation in Frameworks/. Returns 0 when `path` is not a guest
// library we carry, leaving `out` untouched.
int kl_library_file(const char *path, char *out, size_t cap) {
    return dylib_candidate(path, out, cap, NULL, 0);
}

// ---------- segment protection, when the HOST page is coarser than p_align ----------
//
// Apply PT_LOAD permissions at HOST page granularity, as the union of every
// segment sharing a page — and then resolve the one union that cannot be
// applied.
//
// A per-segment mprotect() rounded out to the page is correct only while no two
// segments share one, which held while every guest's p_align was at least
// Apple's 16 KB page: Beat Saber 1.28 ships 64 KB, Steam Link 16 KB. Beat Saber
// 1.40 ships
// **4 KB** — eleven of its thirteen libraries — so segments now share pages,
// the last one written wins, and libmain's r-x segment was left RW because the
// RW segment 0x14e4 bytes above it is processed after it. JNI_OnLoad then took
// an instruction fetch from a non-executable page: SIGBUS with the fault
// address equal to the pc, which reads like a wild jump rather than a
// permission. Nothing about it named the loader.
//
// The union is W|X on exactly one page per library (where the r-x segment's
// tail meets the RW segment's head), and macOS refuses mprotect(RWX) on
// ordinary anonymous memory with EACCES, so "just OR them" does not survive
// contact. Two rules settle every case in the corpus, and both are readings of
// what the file already says rather than heuristics:
//
//   1. Drop X if no SHF_EXECINSTR SECTION lands on the page. The r-x LOAD
//      segment spans .rodata, .dynsym, .rela.dyn and .eh_frame as well as
//      .text — the same distinction the rewrite passes above rely on — so the
//      X on such a page was never for code. 8 of 11 libraries, libunity,
//      libil2cpp and libenet among them.
//   2. Otherwise drop W if every writable byte on the page is inside
//      PT_GNU_RELRO. RELRO is by definition read-only once relocation is done,
//      and relocation IS done here, so this is the ELF's own instruction rather
//      than a concession. The remaining 3 — libmain, lib_burst_generated,
//      libhaptics_sdk.
//
// Anything left is a page that genuinely needs both, which no permission can
// express. It fails by name rather than silently mis-protecting, because the
// symptom on either side is a fault a long way from here.
// How many host pages an image of `span` bytes placed `bias` into its mapping
// covers, and which one a guest vaddr lands on. Both are here rather than
// inline so the planner and the applier cannot disagree about the arithmetic.
static size_t page_count(size_t span, size_t bias, uintptr_t pg) {
    return (span + bias + pg - 1) / pg;
}

// Fill `want` — the ELF PF_* bits each host page needs, as the union of every
// segment on it — and then resolve the unions that cannot be applied, by the
// two rules above. Returns 0 if every page came out expressible, -1 otherwise;
// on -1 `*bad_va` is the guest vaddr the first unresolvable page starts at.
//
// `want` may be NULL, which is the bias search asking whether a placement is
// loadable at all rather than what its protections would be.
static int plan_pages(const Elf64_Ehdr *eh, const Elf64_Phdr *ph, const Elf64_Shdr *sh,
                      uint64_t lo, size_t span, size_t bias, uintptr_t pg,
                      uint8_t *want, int64_t *bad_va) {
    const size_t npages = page_count(span, bias, pg);
    uint8_t *bits = want ? want : calloc(npages, 1);
    if (!bits) return -1;
    if (want) memset(want, 0, npages);

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || !ph[i].p_memsz) continue;
        size_t off = (size_t)(ph[i].p_vaddr - lo) + bias;
        size_t s = off / pg, e = (off + (size_t)ph[i].p_memsz + pg - 1) / pg;
        for (size_t p = s; p < e && p < npages; p++) bits[p] |= (uint8_t)ph[i].p_flags;
    }

    int rc = 0;
    for (size_t p = 0; p < npages; p++) {
        if ((bits[p] & (PF_W | PF_X)) != (PF_W | PF_X)) continue;
        // Signed, because with a bias the first page starts BELOW the image's
        // lowest vaddr — the bytes there are padding we own and nothing claims.
        int64_t ps = (int64_t)lo + (int64_t)(p * pg) - (int64_t)bias;
        int64_t pe = ps + (int64_t)pg;

        int has_code = 0;
        for (int i = 0; sh && i < eh->e_shnum; i++)
            if ((sh[i].sh_flags & SHF_EXECINSTR) && sh[i].sh_type != SHT_NOBITS
                && (int64_t)sh[i].sh_addr < pe
                && (int64_t)(sh[i].sh_addr + sh[i].sh_size) > ps)
                has_code = 1;
        if (!has_code) { bits[p] &= (uint8_t)~PF_X; continue; }

        // Every writable byte on this page must be inside a PT_GNU_RELRO range.
        int relro_covers = 1;
        for (int i = 0; i < eh->e_phnum && relro_covers; i++) {
            if (ph[i].p_type != PT_LOAD || !(ph[i].p_flags & PF_W)) continue;
            int64_t ws = (int64_t)ph[i].p_vaddr, we = ws + (int64_t)ph[i].p_memsz;
            if (ws >= pe || we <= ps) continue;
            if (ws < ps) ws = ps;
            if (we > pe) we = pe;
            int covered = 0;
            for (int j = 0; j < eh->e_phnum; j++)
                if (ph[j].p_type == PT_GNU_RELRO && (int64_t)ph[j].p_vaddr <= ws
                    && (int64_t)(ph[j].p_vaddr + ph[j].p_memsz) >= we)
                    covered = 1;
            if (!covered) relro_covers = 0;
        }
        if (relro_covers) { bits[p] &= (uint8_t)~PF_W; continue; }

        if (bad_va) *bad_va = ps;
        rc = -1;
        break;
    }
    if (!want) free(bits);
    return rc;
}

// ...and the third way out, which is not a protection at all: MOVE THE IMAGE.
//
// The general way out, and BONELAB needs it. Its libmain.so has just TWO PT_LOADs,
// r-x at 0 and rw- at 0x2d80, so both live in host page 0; PT_GNU_RELRO covers
// 0x2d80..0x3000 and the 0x40 bytes above it are ordinary .data. Rule 1 cannot
// apply (there is real code on the page) and rule 2 cannot (those 0x40 bytes are
// writable and outside RELRO), so the library that IS the entry point failed to
// load, before any of the interesting questions about the title could be asked.
//
// What no permission can express, placement can: the mapping is ours to choose,
// and shifting the image inside it moves where the host page boundaries FALL
// without moving any segment relative to any other. Put a boundary in the gap
// between the last executable byte and the first writable one and the collision
// is gone rather than resolved. klepton_ld.c does exactly this offline for the
// dylib path (see its `image_shift` search) — this is the runtime's half, and
// the two now agree that a guest's placement is part of loading it.
//
// **The shift must be a multiple of 4096**, whatever else it does: `adrp`
// resolves against a 4 KB page, so every PC-relative reference the linker
// encoded survives only if the whole image moves by a multiple of that. A shift
// that is not costs a run to diagnose — klepton_ld.c's comment records what it
// looks like (a static table read one page out, and nothing naming the shift).
//
// Stepping by 4 KB through one host page is exhaustive, because the layout
// repeats mod the host page size. Bias 0 is tried FIRST and wins whenever it is
// loadable, so every guest that loads today is placed exactly where it is
// placed today — a 16 KB-or-coarser guest never reaches the second candidate.
#define KL_ADRP_PAGE 0x1000u
static size_t choose_bias(const Elf64_Ehdr *eh, const Elf64_Phdr *ph,
                          const Elf64_Shdr *sh, uint64_t lo, size_t span,
                          uintptr_t pg) {
    for (size_t bias = 0; bias < (size_t)pg; bias += KL_ADRP_PAGE)
        if (plan_pages(eh, ph, sh, lo, span, bias, pg, NULL, NULL) == 0)
            return bias;
    return 0;   // none works; protect_pages reports which page and why
}

static int protect_pages(kl_image *img, const Elf64_Ehdr *eh, const Elf64_Phdr *ph,
                         const Elf64_Shdr *sh, uint64_t lo) {
    const uintptr_t pg = (uintptr_t)getpagesize();
    const size_t npages = page_count(img->span, img->map_off, pg);
    uint8_t *want = calloc(npages, 1);          // ELF PF_* bits, per host page
    if (!want) { err("out of memory for %zu page protections", npages); return -1; }

    int64_t bad_va = 0;
    if (plan_pages(eh, ph, sh, lo, img->span, img->map_off, pg, want, &bad_va) != 0) {
        uint64_t minalign = 0;
        for (int i = 0; i < eh->e_phnum; i++)
            if (ph[i].p_type == PT_LOAD && (!minalign || ph[i].p_align < minalign))
                minalign = ph[i].p_align;
        // Reached only when NO placement works either — choose_bias has already
        // tried every 4 KB offset through a host page. So the gap between the
        // executable bytes and the writable ones is narrower than a page
        // everywhere, which is a property of the file and not of this run.
        err("%s: page at vaddr %#llx needs W and X together and neither rule "
            "applies — executable sections are present AND writable bytes "
            "outside PT_GNU_RELRO share it, at every 4 KB placement of the "
            "image (LOAD p_align %#llx < host page %#lx)",
            img->path, (unsigned long long)bad_va,
            (unsigned long long)minalign, (unsigned long)pg);
        free(want);
        return -1;
    }

    // Coalesce equal-protection runs: libil2cpp is 5380 pages and would
    // otherwise cost 5380 syscalls and as many VM map entries.
    uint8_t *map = img->base - img->map_off;
    for (size_t p = 0; p < npages; ) {
        if (!want[p]) { p++; continue; }        // a gap between segments; leave RW
        size_t q = p;
        while (q < npages && want[q] == want[p]) q++;
        int prot = ((want[p] & PF_R) ? PROT_READ  : 0) |
                   ((want[p] & PF_W) ? PROT_WRITE : 0) |
                   ((want[p] & PF_X) ? PROT_EXEC  : 0);
        if (mprotect(map + p * pg, (q - p) * pg, prot) != 0)
            fprintf(stderr, "  [klepton] mprotect(%p,%#zx,%d) failed: %s\n",
                    (void *)(map + p * pg), (q - p) * pg, prot, strerror(errno));
        p = q;
    }
    free(want);
    return 0;
}

kl_image *kl_load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { err("open %s: %s", path, strerror(errno)); return NULL; }
    struct stat sb;
    if (fstat(fd, &sb) != 0) { err("fstat: %s", strerror(errno)); close(fd); return NULL; }
    uint8_t *file = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED) { err("mmap file: %s", strerror(errno)); return NULL; }

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)file;
    if (memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0 || eh->e_ident[4] != 2) {
        err("not an ELF64 file"); munmap(file, sb.st_size); return NULL;
    }
    if (eh->e_machine != 183) { err("not AArch64 (e_machine=%u)", eh->e_machine);
                                munmap(file, sb.st_size); return NULL; }

    const Elf64_Phdr *ph = (const Elf64_Phdr *)(file + eh->e_phoff);
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) {
            if (ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
            if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
        }
    if (lo == UINT64_MAX) { err("no PT_LOAD"); munmap(file, sb.st_size); return NULL; }

    kl_image *img = calloc(1, sizeof *img);
    img->span = (size_t)(hi - lo);
    snprintf(img->path, sizeof img->path, "%s", path);

    // Section headers are read here rather than at the rewrite passes below
    // because the placement decision needs them too: rule 1 asks which pages
    // carry executable SECTIONS, not which carry the executable segment.
    const Elf64_Shdr *sh = (eh->e_shoff && eh->e_shnum)
                         ? (const Elf64_Shdr *)(file + eh->e_shoff) : NULL;

    // One RW mapping for the whole vaddr span; permissions tightened after
    // relocation. The span is padded by a host page and the image placed
    // `map_off` into it, so that a guest whose p_align is finer than the host
    // page can have a page boundary land between its executable bytes and its
    // writable ones — see choose_bias(). map_off is 0 for every guest that does
    // not need it, which is every guest aligned to 16 KB or coarser.
    const uintptr_t pg = (uintptr_t)getpagesize();
    img->map_off  = choose_bias(eh, ph, sh, lo, img->span, pg);
    img->map_size = (img->span + img->map_off + pg - 1) & ~((size_t)pg - 1);

    // ...and the x18 veneer pool is reserved WITH the image, on the end of the
    // same mapping.
    //
    // A separate mmap with a hint just past the code does NOT work: a hint is
    // only a hint, Darwin returns the first free hole at or above it,
    // and the address after a guest image is exactly the one guaranteed to be
    // taken. For libUE4 (172 MB, every one of its 8038 x18 sites in a 5 MB band
    // at the top) under a viewer run — ANGLE, Metal and SDL already mapped — the
    // nearest hole measures 240 MB away, past `b`'s +/-128 MB, so every veneer
    // is refused and the library runs against Darwin's reserved x18. Searching
    // harder does not help: there is no free hole in the window at all. Only a
    // reservation made at the same moment as the image is guaranteed to be
    // within reach of it.
    //
    // Sized by counting the sites in the FILE's executable sections, which is
    // exact enough to be tight: relocations never touch guest text, so the
    // words counted here are the words the rewrite pass will see. The count is
    // an over-estimate only in that it includes data words the pass will refuse.
    size_t x18_arena = 0;
    for (int i = 0; sh && i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_PROGBITS || !(sh[i].sh_flags & SHF_EXECINSTR)) continue;
        if (sh[i].sh_offset + sh[i].sh_size > (uint64_t)sb.st_size) continue;
        x18_arena += (size_t)kl_x18_count(file + sh[i].sh_offset, (size_t)sh[i].sh_size);
        // ...plus the jump-table rewrite's worst case per `br x18`, the one site
        // shape whose veneer grows a thunk per table target.
        x18_arena += (size_t)kl_x18_count_termbr(file + sh[i].sh_offset,
                                                 (size_t)sh[i].sh_size)
                   * (KLX_JT_MAX_INSN / KLX_VEN_MAX_INSN + 2);
    }
    if (x18_arena) {
        // Per-chunk pools each round up to 64 KB and one section can be several
        // chunks (data ranges inside .text split it), so the reservation
        // carries slack for that rounding rather than exactly the veneer bytes.
        x18_arena = x18_arena * KLX_VEN_MAX_INSN * 4 + (64u << 10) * 64;
        x18_arena = (x18_arena + pg - 1) & ~((size_t)pg - 1);
    }
    img->x18_arena_size = x18_arena;

    uint8_t *map = mmap(NULL, img->map_size + x18_arena, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (map == MAP_FAILED) { err("mmap image: %s", strerror(errno));
                             free(img); munmap(file, sb.st_size); return NULL; }
    img->base = map + img->map_off;
    if (img->map_off)
        fprintf(stderr, "  [klepton] %s: placed %#zx into its mapping so a host "
                        "page boundary falls between the executable bytes and "
                        "the writable ones\n", path, img->map_off);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        memcpy(img->base + ph[i].p_vaddr - lo, file + ph[i].p_offset, ph[i].p_filesz);
        // remainder of memsz is .bss — already zero from MAP_ANON
    }

    // ---- PT_DYNAMIC, then relocate ----
    // Every relocation target is in the RW segment; __TEXT is never written.
    if (bind_dynamic(img, ph, eh->e_phnum, lo, 1) != 0) {
        munmap(file, sb.st_size); return NULL;
    }

    // ---- rewrite guest text: TLS, then x18 ----
    //
    // Executable *sections*, not the executable segment. The r-x LOAD segment of
    // these libraries starts at file offset 0 and runs to the end of
    // .gcc_except_table, so it also spans .hash, .dynsym, .rela.dyn, .rodata and
    // .eh_frame — for libunity that is 1.5 MB of read-only data and 720 KB of
    // relocations sitting behind PF_X. Rewriting a word that merely looks like
    // an instruction in there corrupts data — the x18 pass patches a branch
    // rather than flipping one bit, so it corrupts several words at a time.
    // (`sh` is read above — the placement decision needs the same distinction.)

    // The authoritative .dynsym entry count, and the reason it is taken from the
    // SECTION rather than from DT_HASH: DT_HASH is optional. Steam Link's VR APK
    // builds libSDL3/libmain with `.gnu.hash` only — no `.hash` section, so no
    // DT_HASH — and `.gnu.hash` then sits between `.dynsym` and `.dynstr`, which
    // is exactly the adjacency the old inference assumed. Section headers are
    // required here anyway (x18 cannot separate code from rodata without them),
    // so this is free. DT_HASH's nchain, set in bind_dynamic, is the fallback.
    for (int i = 0; sh && i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_DYNSYM || !sh[i].sh_entsize) continue;
        img->nsyms = (size_t)(sh[i].sh_size / sh[i].sh_entsize);
        break;
    }

    int veneer = sh != NULL;      // KL_X18 is honoured inside kl_x18_patch, which
                                  // still counts sites when the rewrite is off
    if (!sh)
        fprintf(stderr, "  [klepton] %s: no section headers — cannot separate code "
                        "from rodata; x18 veneering disabled\n", path);

    for (int i = 0; i < eh->e_phnum && !sh; i++) {
        // Fallback for a stripped image: the old whole-segment scan. Narrower
        // than it should be is not an option for TLS — an unrewritten
        // tpidr_el0 read is a wrong answer on every thread.
        if (ph[i].p_type != PT_LOAD || !(ph[i].p_flags & PF_X)) continue;
        rewrite_tls(img->base + ph[i].p_vaddr - lo, (size_t)ph[i].p_filesz,
                    ph[i].p_vaddr, path, &img->stats);
    }
    // ...and code inside those sections, not whole sections. An executable
    // section can itself contain data: Steam Link's libmain.so carries
    // BoringSSL's 148 KB `ecp_nistz256_precomputed` table inside .text, and
    // 1059 of its 1080 apparent x18 sites are words of that table. Patching
    // them would replace elliptic-curve constants with branches — a wrong
    // answer that surfaces as a failing TLS handshake, nowhere near here.
    static klx_range skip[256];
    unsigned nskip = sh ? kl_x18_data_ranges(file, (size_t)sb.st_size, skip,
                                             sizeof skip / sizeof *skip) : 0;
    if (nskip) {
        uint64_t bytes = 0;
        for (unsigned i = 0; i < nskip; i++) bytes += skip[i].end - skip[i].start;
        fprintf(stderr, "  [klepton] %s: %u data range(s) inside executable sections, "
                        "%llu bytes — left alone\n",
                path, nskip, (unsigned long long)bytes);
    }
    // The reservation made with the mapping above, carved across every chunk of
    // every executable section. Running out is not silent: kl_x18_patch falls
    // back to searching for a hole, and a search that fails leaves the refusals
    // that the report below names.
    uint8_t *x18_arena_base = img->x18_arena_size
                            ? img->base - img->map_off + img->map_size : NULL;
    size_t   x18_arena_used = 0;

    // The image behind each chunk, so the `br x18` jump-table rewrite can reach
    // the table it has to repoint. Here the chunk addresses ARE the mapped
    // addresses, so resolving is only a bounds check against the section that
    // owns the VA — and the section must be read-only ALLOC data: executable
    // would let a mis-recognition write over code, and writable is rewritten by
    // relocations, which would undo the repointing without a word.
    struct x18_img_ctx ictx = { sh, eh->e_shnum, (uint64_t)(uintptr_t)img->base - lo };
    kl_x18_image ximg = { x18_img_data_at, &ictx };

    for (int i = 0; sh && i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_PROGBITS || !(sh[i].sh_flags & SHF_EXECINSTR)) continue;
        uint64_t cursor = sh[i].sh_addr, cva;
        size_t csz;
        while (kl_x18_next_code(sh[i].sh_addr, (size_t)sh[i].sh_size,
                                skip, nskip, &cursor, &cva, &csz)) {
            uint8_t *p = img->base + cva - lo;
            rewrite_tls(p, csz, cva, path, &img->stats);
            if (!veneer) continue;
            kl_x18_stats xs;
            if (kl_x18_patch(p, csz, x18_arena_base, img->x18_arena_size,
                             &x18_arena_used, &ximg, &xs) != 0) {
                fprintf(stderr, "  [klepton] %s: x18 veneering failed to start\n", path);
                veneer = 0;
                continue;
            }
            // The jump-table rewrite, both halves. A CONVERTED `br x18` no
            // longer branches through x18 at all and cannot be killed by the
            // kernel zeroing it; a FALLBACK one still can, and that residual is
            // what killed Arizona Sunshine 2 three launches running — so it is
            // printed rather than left to be inferred from silence.
            if (xs.jt_sites || xs.jt_fallback)
                fprintf(stderr, "  [klepton] %s: br x18 — %u converted to thunk "
                                "dispatch (%u targets), %u on the guarded veneer%s\n",
                        path, xs.jt_sites, xs.jt_thunks, xs.jt_fallback,
                        xs.jt_fallback ? " (residual x18 window)" : "");
            // A REFUSED x18 site runs against Darwin's reserved x18, which is
            // zero after any exception return, and it has no other error
            // surface: libUE4's 8038 sites all refused for REACH surface as an
            // intermittent SIGSEGV inside Oodle's decompressor on a pak worker
            // thread, hours from the load that caused it.
            //
            // So it is printed unconditionally when non-zero, with the pool's
            // own distance beside it, because the two refusal kinds want
            // different fixes and only one of them is about the decoder.
            if (xs.refused) {
                int64_t d = (int64_t)xs.pool_va - (int64_t)(uintptr_t)p;
                fprintf(stderr,
                        "  [klepton] %s: x18 REFUSED %u of %u sites (%u for REACH"
                        " — pool at %#llx, %+lld MB from the code at %p). Those "
                        "sites run against Darwin's reserved x18 and it is zero "
                        "after any exception return.\n",
                        path, xs.refused, xs.sites, xs.far_refused,
                        (unsigned long long)xs.pool_va,
                        (long long)(d / (1024 * 1024)), (void *)p);
            }
            img->stats.x18_sites   += xs.sites;
            img->stats.x18_patched += xs.patched;
            img->stats.x18_refused += xs.refused;
            img->stats.x18_data_words += xs.data_words;
            img->stats.ctr_sites   += xs.ctr_sites;
            img->stats.ctr_patched += xs.ctr_patched;
            img->stats.ctr_refused += xs.ctr_refused;
        }
    }
    // Measured guest patches, here because this is the last moment the image is
    // writable and the icache flush below already covers whatever they wrote.
    // Every row is guarded by the word it expects to find, so a library this
    // table has never heard of costs one strcmp — see kl_guestpatch.c.
    struct gp_ctx { uint8_t *base; uint64_t lo, span; } gpc = {
        img->base, lo, (uint64_t)img->span
    };
    if (img->stats.tp_writes)
        fprintf(stderr, "  [klepton] %s: %u `msr tpidr_el0` write(s) dropped — the guest's "
                        "thread pointer is TPIDRRO_EL0 here and TPIDR_EL0 is Darwin's\n",
                path, img->stats.tp_writes);
    kl_guest_patch_apply(path, gp_at, &gpc);

    sys_icache_invalidate(img->base, img->span);

    if (protect_pages(img, eh, ph, sh, lo) != 0) {
        munmap(file, (size_t)sb.st_size); return NULL;
    }

    if (kl_env_on("KL_TRACE_IMAGES", 0))
        fprintf(stderr, "  [klepton] image %s base %p span %#zx\n",
                path, (void *)img->base, img->span);
    munmap(file, (size_t)sb.st_size);
    return img;
}

void kl_image_initializers(const kl_image *img, void (**init)(void),
                           void (***array)(void), size_t *count) {
    if (init)  *init  = img ? img->init_fn : NULL;
    if (array) *array = img ? img->init_array : NULL;
    if (count) *count = img ? img->init_count : 0;
}
int kl_image_symbolic(const kl_image *img) { return img && img->symbolic; }

void kl_run_init(kl_image *img) {
    for (size_t i = 0; i < img->init_count; i++)
        if (img->init_array[i]) img->init_array[i]();
}

void *kl_sym(kl_image *img, const char *name) {
    // Linear scan of .dynsym.
    //
    // The count comes from DT_HASH's nchain, never from the gap between symtab
    // and strtab: that assumes .dynstr directly follows .dynsym, which
    // is true of every Beat Saber library and false of every Steam Link one,
    // where the linker puts .gnu.hash and .hash between them. libc++_shared.so
    // has 2506 symbols and a gap of 4198 entries, so the scan runs off the end
    // of .dynsym into the hash tables, reads their words as st_name offsets and
    // dereferences strtab + garbage — a fault inside strcmp during a
    // cross-image import bind, and a wrong answer under a different layout.
    //
    // A GLIBC image is searched through its GNU hash table instead: the same
    // first match a linear scan finds (a bucket's chain runs in symbol order),
    // in a few probes rather than thousands of strcmps — which stopped being an
    // economy with the Alyx payload, ~65k import relocations across 130 images
    // each searching every image loaded before it. Bionic guests keep the scan
    // they have always had.
    if (img->glibc && img->gnu_hash) {
        const Elf64_Sym *s = gnu_hash_find(img, name);
        return s ? img->base + s->st_value : NULL;
    }
    size_t n = img->nsyms;
    if (!n)     // neither sections nor DT_HASH: infer, rather than bind nothing
        n = ((const char *)img->strtab - (const char *)img->symtab) / sizeof(Elf64_Sym);

    // Belt and braces, and this is the part that makes the class of bug above
    // impossible rather than merely fixed: an over-long count must degrade to
    // "no match", never to a wild read. Everything an ELF symbol table can name
    // lives inside the mapping, so a st_name or st_value outside it is a symbol
    // this table does not really have — which is precisely what a miscounted
    // scan reads once it walks off the end into the hash tables.
    const char *lo_p = (const char *)img->base, *hi_p = lo_p + img->span;
    for (size_t i = 0; i < n; i++) {
        const Elf64_Sym *s = &img->symtab[i];
        if ((const char *)(s + 1) > hi_p) break;             // past the mapping
        if (s->st_shndx == 0 || !s->st_name) continue;
        const char *nm = img->strtab + s->st_name;
        if (nm < lo_p || nm >= hi_p) continue;               // not a real name
        if (strcmp(nm, name) == 0)
            return img->base + s->st_value;
    }
    return NULL;
}

void kl_unload(kl_image *img) {
    if (!img) return;
    if (img->dl_handle) dlclose(img->dl_handle);   // translated: dyld owns it
    else                munmap(img->base - img->map_off,
                               img->map_size + img->x18_arena_size);
    free(img);
}

// Read an image's DT_NEEDED sonames straight out of the file, without loading
// it. kl_load_recursive uses this to pull the dependency chain in before the
// image that imports it, so relocation-time binding (sym_value's global guest
// search) has the exports in place.
//
// Everything the walk needs is addressed by GUEST VADDR — PT_DYNAMIC's, and
// DT_STRTAB's — and the two shapes an unloaded image comes in answer that
// question differently, which is what the view below exists to hide:
//
//   a guest .so on disk — vaddr -> file offset through the PT_LOAD table.
//   a translated dylib  — klepton-ld does NOT embed the file. __TEXT,__klelf is
//     the read-only PT_LOAD GROUP laid out by VADDR, and __DATA,__kldata is the
//     writable one; each group's bytes start at that group's vlo, not at zero.
//
// Group 0 is identity-mapped on these guests (p_vaddr == p_offset), so a
// file-offset walk reads the ELF header and the program headers correctly —
// and then follows PT_DYNAMIC, which lives in the WRITABLE group, off the end
// of __klelf into whatever the Mach-O laid down next. No DT_STRTAB is found
// there, so every translated library reported ZERO dependencies. That is a
// topological sort with no edges, which emits its input unchanged: hl2vr's
// device chain loaded in Frameworks-directory order, libsoundemittersystem
// initialised before libtier0, its g_pMemAlloc import never bound, and its
// first operator new dereferenced the unbound slot — SIGSEGV inside
// DT_INIT_ARRAY, two seconds into launch #2. The host never saw it: there the
// .so tree is present, so the file path answers.
typedef struct { uint64_t vlo, vhi, foff; } elf_span;
typedef struct {
    const uint8_t *file;
    size_t         size;
    elf_span       s[8];
    int            n;
} elf_view;

static void ev_add(elf_view *v, uint64_t vlo, uint64_t vhi, uint64_t foff) {
    if (v->n < (int)(sizeof v->s / sizeof v->s[0]) && vhi > vlo)
        v->s[v->n++] = (elf_span){ vlo, vhi, foff };
}

static const void *ev_at(const elf_view *v, uint64_t vaddr) {
    for (int i = 0; i < v->n; i++) {
        if (vaddr < v->s[i].vlo || vaddr >= v->s[i].vhi) continue;
        uint64_t off = v->s[i].foff + (vaddr - v->s[i].vlo);
        return off < v->size ? v->file + off : NULL;
    }
    return NULL;
}

// The DT_NEEDED walk itself, over an ELF image that is already in memory.
// `elf` points at the ELF header; `v` maps guest vaddrs to bytes, and an empty
// one means "this is a plain file", where the PT_LOAD table is that map.
// `soname`/`runpath` (either may be NULL) come back as DT_SONAME and
// DT_RUNPATH (DT_RPATH when there is no RUNPATH), or empty.
// DT_NEEDED names past the caller's `max` (a dependency that would vanish
// silently): counted here, named with the file by kl_list_dynamic.
static __thread unsigned g_needed_dropped;
static int elf_needed(const uint8_t *elf, elf_view *v, char names[][128], int max,
                      char *soname, char *runpath, size_t cap) {
    if (soname && cap) soname[0] = 0;
    if (runpath && cap) runpath[0] = 0;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)elf;
    if (memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0 || eh->e_machine != 183) return 0;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(elf + eh->e_phoff);

    if (!v->n)
        for (int i = 0; i < eh->e_phnum; i++)
            if (ph[i].p_type == PT_LOAD)
                ev_add(v, ph[i].p_vaddr, ph[i].p_vaddr + ph[i].p_filesz, ph[i].p_offset);

    const Elf64_Dyn *dyn = NULL;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC) { dyn = ev_at(v, ph[i].p_vaddr); break; }
    if (!dyn) return 0;

    const char *strtab = NULL;
    for (const Elf64_Dyn *dp = dyn; dp->d_tag != DT_NULL && dp - dyn < 4096; dp++)
        if (dp->d_tag == DT_STRTAB) { strtab = ev_at(v, dp->d_val); break; }
    if (!strtab) return 0;

    int n = 0, have_runpath = 0;
    for (const Elf64_Dyn *dp = dyn; dp->d_tag != DT_NULL && dp - dyn < 4096; dp++) {
        if (dp->d_tag == DT_NEEDED && n < max) snprintf(names[n++], 128, "%s", strtab + dp->d_val);
        else if (dp->d_tag == DT_NEEDED) g_needed_dropped++;     // said aloud by the caller
        else if (dp->d_tag == DT_SONAME && soname) snprintf(soname, cap, "%s", strtab + dp->d_val);
        else if (dp->d_tag == DT_RUNPATH && runpath) {
            snprintf(runpath, cap, "%s", strtab + dp->d_val);
            have_runpath = 1;
        } else if (dp->d_tag == DT_RPATH && runpath && !have_runpath)
            snprintf(runpath, cap, "%s", strtab + dp->d_val);
    }
    return n;
}

// The embedded guest image inside a MAPPED klepton-ld dylib, by walking the
// load commands to __TEXT,__klelf (and __DATA,__kldata beside it).
// getsectiondata() answers the same question for a LOADED image; this one is
// asked about a file, before anything is loaded, which is what a dependency
// sort needs. `sec_off`/`sec_size`, when given, come back as {klelf, kldata}.
static const uint8_t *macho_klelf(const uint8_t *file, size_t size,
                                  uint64_t sec_off[2], uint64_t sec_size[2]) {
    if (size < sizeof(struct mach_header_64)) return NULL;
    const struct mach_header_64 *mh = (const struct mach_header_64 *)file;
    if (mh->magic != MH_MAGIC_64) return NULL;
    const uint8_t *klelf = NULL;
    const uint8_t *p = file + sizeof *mh;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)p;
        if (lc->cmdsize < sizeof *lc || (size_t)(p - file) + lc->cmdsize > size) break;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg = (const struct segment_command_64 *)p;
            const struct section_64 *sec = (const struct section_64 *)(sg + 1);
            for (uint32_t j = 0; j < sg->nsects; j++, sec++) {
                int which = !strncmp(sec->sectname, "__klelf", 16)  ? 0 :
                            !strncmp(sec->sectname, "__kldata", 16) ? 1 : -1;
                if (which < 0 || sec->offset >= size) continue;
                if (!which) klelf = file + sec->offset;
                if (sec_off)  sec_off[which]  = sec->offset;
                if (sec_size) sec_size[which] = sec->size;
            }
        }
        p += lc->cmdsize;
    }
    return klelf;
}

// The view over a translated dylib's embedded groups. klepton-ld groups
// consecutive PT_LOADs by writability and emits group 0 as __klelf and group 1
// as __kldata, each starting at its own vlo — so reconstructing the same
// grouping from the guest's program headers is what pairs the two.
static void klelf_view(const uint8_t *elf, elf_view *v,
                       const uint64_t sec_off[2], const uint64_t sec_size[2]) {
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)elf;
    if (memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0) return;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(elf + eh->e_phoff);
    int g = -1, gw = -1;
    uint64_t vlo = 0, vhi = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        int w = (ph[i].p_flags & PF_W) != 0;
        if (g >= 0 && w == gw) {
            if (ph[i].p_vaddr + ph[i].p_filesz > vhi) vhi = ph[i].p_vaddr + ph[i].p_filesz;
            continue;
        }
        if (g >= 0 && g < 2 && sec_size[g]) ev_add(v, vlo, vlo + sec_size[g], sec_off[g]);
        g++; gw = w;
        vlo = ph[i].p_vaddr;
        vhi = ph[i].p_vaddr + ph[i].p_filesz;
    }
    // Groups past the second have no section of their own — klepton-ld emits
    // two — so they stay unmapped and ev_at answers NULL for them.
    if (g >= 0 && g < 2 && sec_size[g]) ev_add(v, vlo, vlo + sec_size[g], sec_off[g]);
    (void)vhi;
}

int kl_list_needed(const char *path, char names[][128], int max) {
    return kl_list_dynamic(path, names, max, NULL, NULL, 0);
}

int kl_list_dynamic(const char *path, char names[][128], int max,
                    char *soname, char *runpath, size_t cap) {
    // The translation FIRST, by the same rule kl_can_load answers existence
    // with: on device the ELF tree is deliberately not in the bundle, so the
    // .so path is a name rather than a file and reading DT_NEEDED off it would
    // report "no dependencies" for every library — which is a topological sort
    // that silently degrades to directory order, and directory order is what
    // put libfilesystem_stdio before libtier0.
    char cand[1024];
    const char *real = path;
    if (dylib_candidate(path, cand, sizeof cand, NULL, 0)) real = cand;

    int fd = open(real, O_RDONLY);
    if (fd < 0) return -1;
    struct stat sb;
    if (fstat(fd, &sb) != 0) { close(fd); return -1; }
    uint8_t *file = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED) return -1;

    elf_view view = { file, (size_t)sb.st_size, {{0}}, 0 };
    const uint8_t *elf = file;
    if (memcmp(elf, "\x7f" "ELF", 4) != 0) {
        uint64_t sec_off[2] = { 0, 0 }, sec_size[2] = { 0, 0 };
        elf = macho_klelf(file, (size_t)sb.st_size, sec_off, sec_size);
        if (elf) klelf_view(elf, &view, sec_off, sec_size);
    }
    g_needed_dropped = 0;
    int n = elf ? elf_needed(elf, &view, names, max, soname, runpath, cap) : 0;
    if (g_needed_dropped)
        fprintf(stderr, "  [klepton] %s: %u DT_NEEDED name(s) past the reader's %d DROPPED — "
                        "raise the caller's table\n", path, g_needed_dropped, max);
    munmap(file, (size_t)sb.st_size);
    return n;
}

// Every guest library the bundle's translations carry, as .so names — the
// device's answer to "list the guest libdir", which on device is a directory
// that does not exist.
//
// The test for "is this one of the guest's" is the __klelf section itself, not
// a name list: KL_DYLIB_DIR is the app's Frameworks/, which also holds ANGLE,
// MoltenVK and whatever else the bundle links, and none of those carry an
// embedded guest image. A name filter would have to be updated every time the
// bundle gains a framework; this cannot go stale.
unsigned kl_list_translations(char names[][64], unsigned max) {
    const char *dir = kl_env_str("KL_DYLIB_DIR", NULL);
    if (!dir || !*dir || !max) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    unsigned n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        char stem[256], path[1200];
        const char *dot = strstr(e->d_name, ".framework");
        if (dot && !dot[10]) {
            snprintf(stem, sizeof stem, "%.*s", (int)(dot - e->d_name), e->d_name);
            snprintf(path, sizeof path, "%s/%s/%s", dir, e->d_name, stem);
        } else if ((dot = strstr(e->d_name, ".dylib")) && !dot[6]) {
            snprintf(stem, sizeof stem, "%.*s", (int)(dot - e->d_name), e->d_name);
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        } else continue;

        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        struct stat sb;
        if (fstat(fd, &sb) != 0) { close(fd); continue; }
        uint8_t *file = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (file == MAP_FAILED) continue;
        if (macho_klelf(file, (size_t)sb.st_size, NULL, NULL))
            snprintf(names[n++], 64, "%s.so", stem);
        munmap(file, (size_t)sb.st_size);
    }
    closedir(d);
    return n;
}
