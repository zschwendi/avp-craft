// kl_perf — the per-second frame-cost line (round N14, hla).
//
//   [perf] 43.1 fps 23.2 ms | ticks 2.79/f | gpu 21.4 ms/f busy 97% (guest 20.6 copy 0.4
//          other 1.1) | wait xr 0.2 fence 9.6 ms/f | cpu 31.0 ms/f: hlvr 12.1 R, ...
//          | mtl 3308 MiB foot 6341 MiB
//
// GPU time is Metal's own clock: every MTLCommandBuffer committed in the
// process gets a completed handler (the concrete command-buffer classes'
// -commit is wrapped through the ObjC runtime, once) that reads
// GPUStartTime/GPUEndTime. A thread-local TAG set around a vkQueueSubmit says
// whose work it was (MoltenVK commits synchronously on the submitting thread
// by default); a commit on a queue that has only ever carried one tag inherits
// it, the rest is "other" (the compositor's own passes). "busy" is the union
// of all command buffers' GPU intervals over the wall clock: ~100% = the GPU
// is the bottleneck.
//
// CPU time is per thread (Mach thread_info), the busiest named, the role of the
// two threads kl_openvr sees (W = WaitGetPoses caller, S = Submit caller).
//
// OFF unless KL_PERF=1 (driver default; hla turns it on in its target env); KL_PERF_GPU=0 keeps the line but does
// not wrap -commit; KL_PERF_SECS=<s> (default 1) is the line's period; every
// KL_PERF_SUBMIT_SECS (default 10) a second line splits the guest's GPU time
// by its submit ordinal within the frame.
#pragma once
#include <stdint.h>

enum {
    KL_PERF_GPU_OTHER = 0,      // untagged: compositor, ANGLE, anything else
    KL_PERF_GPU_GUEST = 1,      // the guest's own vkQueueSubmit
    KL_PERF_GPU_COPY  = 2,      // kl_openvr's eye copy
    KL_PERF_GPU_NTAG  = 3,
};

int  kl_perf_on(void);
// Wrap -commit (idempotent; call once a Metal device exists).
void kl_perf_gpu_install(void *mtl_device);
// Set the calling thread's tag; returns the previous one. A guest submit
// passes KL_PERF_GPU_GUEST and gets its ordinal within the frame folded in.
int  kl_perf_gpu_tag(int tag);
int  kl_perf_gpu_guest_tag(void);
// A GL guest's frame thread, once a frame after kl_perf_frame: tags ANGLE's
// command buffers committed on it as guest submits (KL_PERF_GL_GUEST).
void kl_perf_gl_frame(void);
// KL_PERF_SPLIT_SUBMIT: what the command buffer submitted under `tag` recorded.
void kl_perf_ord_note(int tag, uint32_t passes, uint32_t copies, uint64_t px);

// Roles: the thread calling these is remembered by name in the line.
enum { KL_PERF_ROLE_WAIT = 0, KL_PERF_ROLE_SUBMIT = 1, KL_PERF_ROLE_QUEUE = 2, KL_PERF_ROLE_N = 3 };
void kl_perf_role(int role);

// Frame accounting, called by the frame owner (kl_openvr's WaitGetPoses):
// `ticks` = display intervals this frame advanced the guest's clock by,
// `xr_wait_ns` = time blocked in xrWaitFrame.
void kl_perf_frame(uint32_t ticks, int64_t xr_wait_ns);
void kl_perf_fence_wait(int64_t ns);
// The guest's vkQueueSubmit, timed (MoltenVK encodes the frame's Metal
// commands inside it unless prefill is configured): the calling thread is
// remembered as role Q.
void kl_perf_guest_submit(int64_t ns);
int64_t kl_perf_now_ns(void);

// KL_PERF_PASSES: a label for the next encoder the calling thread opens (kl_fove's
// expand); NULL clears it.
void kl_perf_pass_label(const char *label);

// Supplied by kl_vulkan: bytes Metal has allocated in the process (0 = unknown).
unsigned long long kl_vulkan_mtl_allocated_bytes(void);
