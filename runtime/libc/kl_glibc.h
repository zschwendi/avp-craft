// The glibc personality: the answers a GLIBC-linked guest gets for the names it
// would have bound in libc.so.6 / libm.so.6 / ld-linux-aarch64.so.1.
//
// The Alyx port's payload is Valve's ARM64 Linux build, linked against glibc,
// and the jailbreak host that loads it binds its imports itself. Valve's own
// libc.so.6 cannot run on XNU (raw `svc`), so it is never loaded: in its place
// the host is handed a STAND-IN — an ELF with the same soname, no code, and a
// dynamic symbol table whose every entry is an absolute (SHN_ABS) address in
// this runtime. The host's own linker then binds each payload import to it
// exactly where Valve's libc.so.6 sat in the search order. See kl_glibc.c for
// what each name is: the bionic table's answer where the ABI is the same, a
// glibc-shaped rebuild where it is not, and a refusal that names itself.
#ifndef KL_GLIBC_H
#define KL_GLIBC_H
#include <stddef.h>
#include <stdio.h>

// The personality's answer for `name`, or NULL when it has none.
void *kl_glibc_lookup(const char *name);

// The NATIVE boot's answer (no jailbreak host): the rows above PLUS the names
// the host used to intercept (kl_glibc_native_table.h — the allocator, errno,
// dl*, exit, sysconf, the SysV semaphores...). The loader's first stop for a
// glibc image's import (kl_image.c sym_value_glibc). The stand-in never
// carries the native rows, so the host build is unchanged by them.
void *kl_glibc_lookup_native(const char *name);

// A door's own answer for `name`, ahead of both tables in
// kl_glibc_lookup_native (never in the stand-in). Register before the images
// that import it are bound.
void kl_glibc_override(const char *name, void *fn);

// What the native table does with `name`, for the bind-time census: answers
// it, refuses it (a NOSYS/NULL/ERR row: the call fails with ENOSYS), aborts
// naming itself when called (noreturn rows, and recorded-not-yet-implemented
// ones), or has no row.
enum { KL_GLIBC_NONE, KL_GLIBC_ANSWERED, KL_GLIBC_REFUSED, KL_GLIBC_ABORTS };
int kl_glibc_row_kind(const char *name);

// Every name the personality answers, with its address, its size (0 for a
// function) and whether it is a data object.
typedef void (*kl_glibc_each_fn)(const char *name, void *addr, size_t size,
                                 int is_data, void *ctx);
void kl_glibc_each(kl_glibc_each_fn fn, void *ctx);

// Write the stand-in ELF for `soname` to `path`. 0 on success; otherwise -1
// and the reason in `err`.
int kl_glibc_write_standin(const char *path, const char *soname, char *err, size_t cap);

// Where the fortified printf family (__printf_chk, __fprintf_chk,
// __vfprintf_chk) sends stdout/stderr text: a guest-ABI vfprintf taking an
// AAPCS64 va_list by reference. The Alyx door points it at the host's own
// console (AlyxGuestConsoleVfprintf), so a payload that prints through the
// fortified entry points reaches the same console as one that calls printf.
// NULL (the default) formats here and writes to the host stream.
void kl_glibc_set_console(int (*vfprintf_fn)(void *fp, const char *fmt, void *va));

// The host library's own resolver for names IT serves (locale objects,
// nl_langinfo, strftime, the allocator) — the `_l` rebuilds and the
// reallocating calls must agree with the objects the host hands out. NULL
// falls back to this runtime's answers.
void kl_glibc_set_host_resolver(void *(*fn)(const char *name));

// Images a glibc guest can ask about (_dl_find_object, dl_iterate_phdr):
// `base` is where ELF vaddr 0 lives, `phdrs` its in-memory program headers.
void kl_glibc_register_image(const char *name, void *base, size_t span,
                             const void *phdrs, unsigned phnum);

// The in-process SysV semaphore/shared-memory emulation (visionOS has no SysV
// IPC) for a BIONIC guest's sem*/shm* imports: the function, or NULL for any
// other name (or when KL_BIONIC_SYSV=0). kl_shim_lookup's first stop.
void *kl_sysv_lookup(const char *name);

// Refusals and first uses, for the end-of-run report.
void kl_glibc_report(FILE *out);

#endif
