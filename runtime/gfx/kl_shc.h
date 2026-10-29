// kl_shc — Metal shader-compile census and warm-up (round F3, hla). See
// kl_shc.m for the design and hla/FOVEATION-F3.md for the measurements.
#pragma once
#include <stdint.h>

enum { KL_SHC_LIB, KL_SHC_FN, KL_SHC_RPS, KL_SHC_CPS, KL_SHC_DSS, KL_SHC_N };

// Swizzle MoltenVK's MTLDevice / MTLLibrary compile entry points (timing,
// recording, warm-up). Call once a Metal device exists, AFTER kl_fove_install
// (this wrapper must sit outside kl_fove's so that a warm-up compile goes
// through kl_fove's patch exactly as the guest's own compile does).
void kl_shc_install(void *mtl_device);
// The calling thread's cumulative SYNCHRONOUS Metal compile time per category
// (ns); asynchronous compiles only reach the all-thread totals below (their
// completion handler may outlive the thread that started them).
void kl_shc_thread_ns(uint64_t out[KL_SHC_N]);
// Every thread's compile time so far, per category (ns): kl_vulkan takes the
// difference around one vkCreate*Pipelines (MoltenVK compiles on a GCD worker
// and waits, so the per-thread sums above do not see it).
void kl_shc_total_ns(uint64_t out[KL_SHC_N]);
// Seconds since kl_shc_install (0 before), and "<name>#<tid % 100000>".
double kl_shc_clock(void);
void kl_shc_thread_label(char out[32]);
// kl_alyx's Valve log tap: Valve logged `SV:  Spawn Server: <name>` (warm
// order and the record's map tag).
void kl_shc_note_map(const char *name);
// kl_perf: a short field for the [perf] line ("" when nothing compiled).
void kl_shc_perf_field(char *out, unsigned cap);

// Tests (tests/t_shc.c): load a list file into a fresh entry table as this
// device's record (origin 1: a torn tail / bad header is repaired IN PLACE) or
// as the shipped list (2, read-only). Returns the entries added.
uint32_t kl_shc_test_load(const char *path, int origin);
// Append one library entry (MSL `src`, zlib'd) to a record file with the
// runtime's record writer. 1 written.
int kl_shc_test_append(const char *path, uint64_t key, const char *src);
