// The TLSDESC resolver. Called by guest code as
//
//     adrp x0, :tlsdesc:v ; ldr x1, [x0,#lo12] ; add x0, x0, #lo12 ; blr x1
//     mrs  x8, TPIDR_EL0  ; ldr x0, [x8, x0]
//
// so on entry x0 is the address of the two-word descriptor whose second word
// kl_image.c pointed at a kl_tlsdesc_arg, and on exit x0 must hold the
// variable's offset from the thread pointer. The offset is a full 64-bit
// register operand in the guest's `ldr x0, [x8, x0]`, so a NEGATIVE offset is
// as usable as a positive one — which is what lets the block be an ordinary
// per-thread allocation instead of something Darwin would have to reserve room
// for. See runtime/kl_tls.h for the model.
//
// THE CONTRACT THAT MAKES THIS ASM AND NOT C: a TLSDESC resolver may clobber
// x0 and nothing else. The call site is inside its caller's live range with no
// save around it, so any register a C compiler treats as scratch — x1-x18, the
// upper halves of v8-v15, all of v0-v7 and v16-v31, and the condition flags —
// is live guest state here. The fast path therefore touches six registers and
// restores them; the slow path, which does call C, saves the entire
// caller-saved file plus NZCV first.
//
// Structure offsets are from kl_tls.h:
//   kl_tlsdesc_arg: +0 mod, +8 off
//   kl_tlsmod:      +0 index (uint32)
// and the per-thread block array lives in TSD slot kl_tlsdesc_slot.

.text
    .globl _kl_tlsdesc_resolver
    .p2align 2
_kl_tlsdesc_resolver:
    stp     x1, x2, [sp, #-48]!
    stp     x3, x4, [sp, #16]
    stp     x5, x6, [sp, #32]

    ldr     x1, [x0, #8]                // the kl_tlsdesc_arg
    adrp    x2, _kl_tlsdesc_slot@PAGE
    ldr     w2, [x2, _kl_tlsdesc_slot@PAGEOFF]
    mrs     x3, tpidrro_el0             // the same register the guest will read
    ldr     x4, [x3, x2, lsl #3]        // this thread's block array
    cbz     x4, Lslow
    ldr     x5, [x1]                    // arg->mod
    ldr     w5, [x5]                    // mod->index
    ldr     x4, [x4, x5, lsl #3]        // this thread's block for that module
    cbz     x4, Lslow
    ldr     x6, [x1, #8]                // arg->off
    add     x4, x4, x6
    sub     x0, x4, x3                  // block + off - TP

    ldp     x5, x6, [sp, #32]
    ldp     x3, x4, [sp, #16]
    ldp     x1, x2, [sp], #48
    ret

    // First access on this thread for this module. Nothing above wrote a
    // register the caller cares about except x0, which is still the descriptor,
    // so unwind the fast-path frame and take the expensive one.
Lslow:
    ldp     x5, x6, [sp, #32]
    ldp     x3, x4, [sp, #16]
    ldp     x1, x2, [sp], #48

    sub     sp, sp, #688
    stp     x1,  x2,  [sp, #0]
    stp     x3,  x4,  [sp, #16]
    stp     x5,  x6,  [sp, #32]
    stp     x7,  x8,  [sp, #48]
    stp     x9,  x10, [sp, #64]
    stp     x11, x12, [sp, #80]
    stp     x13, x14, [sp, #96]
    stp     x15, x16, [sp, #112]
    stp     x17, x18, [sp, #128]
    stp     x29, x30, [sp, #144]        // x30 is our return address (blr x1)
    mrs     x9, nzcv
    str     x9, [sp, #160]
    stp     q0,  q1,  [sp, #176]
    stp     q2,  q3,  [sp, #208]
    stp     q4,  q5,  [sp, #240]
    stp     q6,  q7,  [sp, #272]
    stp     q8,  q9,  [sp, #304]        // v8-v15: only their LOW halves are
    stp     q10, q11, [sp, #336]        // callee-saved by AAPCS, and a guest
    stp     q12, q13, [sp, #368]        // vector live here may be using all 128
    stp     q14, q15, [sp, #400]        // bits, so save them whole.
    stp     q16, q17, [sp, #432]
    stp     q18, q19, [sp, #464]
    stp     q20, q21, [sp, #496]
    stp     q22, q23, [sp, #528]
    stp     q24, q25, [sp, #560]
    stp     q26, q27, [sp, #592]
    stp     q28, q29, [sp, #624]
    stp     q30, q31, [sp, #656]

    ldr     x0, [x0, #8]                // the kl_tlsdesc_arg
    bl      _kl_tlsdesc_slow            // -> TP-relative offset in x0

    ldp     q0,  q1,  [sp, #176]
    ldp     q2,  q3,  [sp, #208]
    ldp     q4,  q5,  [sp, #240]
    ldp     q6,  q7,  [sp, #272]
    ldp     q8,  q9,  [sp, #304]
    ldp     q10, q11, [sp, #336]
    ldp     q12, q13, [sp, #368]
    ldp     q14, q15, [sp, #400]
    ldp     q16, q17, [sp, #432]
    ldp     q18, q19, [sp, #464]
    ldp     q20, q21, [sp, #496]
    ldp     q22, q23, [sp, #528]
    ldp     q24, q25, [sp, #560]
    ldp     q26, q27, [sp, #592]
    ldp     q28, q29, [sp, #624]
    ldp     q30, q31, [sp, #656]
    ldr     x9, [sp, #160]
    msr     nzcv, x9
    ldp     x1,  x2,  [sp, #0]
    ldp     x3,  x4,  [sp, #16]
    ldp     x5,  x6,  [sp, #32]
    ldp     x7,  x8,  [sp, #48]
    ldp     x9,  x10, [sp, #64]
    ldp     x11, x12, [sp, #80]
    ldp     x13, x14, [sp, #96]
    ldp     x15, x16, [sp, #112]
    ldp     x17, x18, [sp, #128]
    ldp     x29, x30, [sp, #144]
    add     sp, sp, #688
    ret
