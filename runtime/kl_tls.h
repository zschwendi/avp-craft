// S0.1b — R_AARCH64_TLSDESC for the ELF-loader path.
//
// WHY THIS EXISTS. The Meta Avatar SDK's three native libraries
// (libovrbody, libovravatar2, libovrgpuskinning) use C++ `thread_local`, and
// the NDK compiles every access through the TLSDESC model. The loader had no
// resolver for relocation type 1031, so those libraries could not be loaded at
// all, so `OvrAvatarManager.initialized` stayed False and ETT's avatar screen
// could not be passed (ETT STATUS.md rounds 27-28).
//
// HOW IT FITS THE TLS MODEL WE ALREADY HAVE, because there must be one model
// and not two. Klepton's whole TLS story is a single sentence: **the guest's
// thread pointer is Darwin's thread pointer**. S0.1 rewrites every guest
// `mrs xN, tpidr_el0` to `tpidrro_el0` (kl_image.c rewrite_tls, and the same
// rewrite offline in tools/klepton_ld.c) so the guest reads the register Darwin
// actually keeps it in; from there bionic's TLS slots ARE Darwin TSD slots —
// that is why kl_shim.c can put the stack canary in slot 5 and why kl_x18.c can
// index the shadow slot as [tp + KLX_TSD_SLOT*8]. Nothing here changes that.
//
// What TLSDESC adds is the piece the rewrite alone cannot serve: a *module's*
// TLS block. The compiler emits
//
//     adrp x0, :tlsdesc:v ; ldr x1, [x0,#lo12] ; add x0, x0, #lo12
//     blr  x1             ; mrs x8, TPIDR_EL0  ; ldr x0, [x8, x0]
//
// so the resolver is called ON EVERY ACCESS, on the accessing thread, and must
// return a TP-RELATIVE OFFSET which the guest then adds to its own reading of
// the thread pointer. Because the resolver runs per access we do not need the
// block to sit at a fixed offset from TP (it cannot: Darwin owns that layout
// and there is no per-module room in it). We allocate the block per thread and
// return `block + addend - tp` — an offset that differs per thread, which is
// exactly what a per-access resolver is allowed to do. The guest's `mrs` is the
// rewritten one, so its TP and ours are the same register: the subtraction and
// the guest's addition cancel exactly.
//
// The corollary, and it is a real limit: R_AARCH64_TLS_TPREL64 CANNOT be served
// this way — it bakes a constant TP offset into the image with no resolver call
// to intercept. It is served by a different trick instead (kl_tls_static_block
// below): a run of consecutive Darwin TSD slots, which every thread owns at the
// same offset from its thread pointer. Valve's libmimalloc (the Alyx payload,
// 3 TPREL relocations, 9 bytes) is the one guest library that needs it.
//
// The per-thread blocks live in one Darwin TSD slot holding a small array
// indexed by module, freed by the key's destructor at thread exit.
#ifndef KL_TLS_H
#define KL_TLS_H
#include <stdint.h>

// One guest module's PT_TLS. `index` is first because the resolver's fast path
// loads it with a bare `ldr w, [mod]` — see runtime/kl_tls_thunk.S, which knows
// this layout and the one below.
typedef struct {
    uint32_t    index;      // +0  slot in the per-thread block array
    uint32_t    _pad;
    const void *init;       // +8  PT_TLS initialisation image, filesz bytes
    uint64_t    filesz;     // +16
    uint64_t    memsz;      // +24 block size; bytes past filesz are zero
    uint64_t    align;      // +32
    const char *path;       // +40 for diagnostics only
} kl_tlsmod;

// The second word of a TLSDESC descriptor points at one of these. One per
// relocation, allocated at load time and never freed (images outlive the run).
typedef struct {
    kl_tlsmod *mod;         // +0
    uint64_t   off;         // +8  byte offset of the variable within the block
} kl_tlsdesc_arg;

// Claim the TSD key and prove direct indexing works on this OS. Idempotent;
// returns 0 on success. A non-zero return must make the LOAD fail — a TLSDESC
// descriptor pointing at a resolver that cannot find its slot is a wrong
// address, not a missing feature.
int kl_tls_init(void);

// Register a module's PT_TLS, returning its record (NULL if the table is full
// or kl_tls_init failed). Called once per image, on the loading thread.
kl_tlsmod *kl_tls_register(const char *path, const void *init,
                           uint64_t filesz, uint64_t memsz, uint64_t align);

// The resolver the descriptor's first word is set to. Defined in
// runtime/kl_tls_thunk.S; declared here only so kl_image.c can take its address.
// NOT callable from C — it obeys the TLSDESC register contract, not AAPCS.
void kl_tlsdesc_resolver(void);

// The thunk's slow path: allocate this thread's block for this module and
// return the TP-relative offset of the variable. Also the whole computation, so
// the fast path and the slow path cannot disagree about the answer.
int64_t kl_tlsdesc_slow(const kl_tlsdesc_arg *a);

// Read by the thunk's fast path. Set by kl_tls_init.
extern uint32_t kl_tlsdesc_slot;

// Static TLS, for R_AARCH64_TLS_TPREL64 (initial-exec / local-exec code: the
// guest adds a CONSTANT to its thread pointer, with no call to intercept). The
// block is given a home that IS at a fixed offset from every thread's pointer:
// a run of consecutive Darwin TSD slots (pthread keys), 8 bytes each. Returns
// that offset (> 0), or -1 when no run of keys could be had or the block needs
// more than a slot's 8-byte alignment. The template is copied into each
// thread's slots by kl_tls_static_seed — NOT here, because at registration the
// template may still be waiting for its own relocations.
// (The Alyx host door keeps its own copy of this for the host's linker —
// alyx_static_tls in kl_alyx.c; this is the runtime loader's.)
int64_t kl_tls_static_block(const char *path, const void *init, uint64_t filesz,
                            uint64_t memsz, uint64_t align);

// Copy every registered static-TLS template into THIS thread's slots, once per
// block per thread: a second call on the same thread changes nothing, so a
// thread's live values are never reset. The loader installs it as a
// kl_thread_init hook at the first registration and calls it on the loading
// thread.
void kl_tls_static_seed(void);

#endif
