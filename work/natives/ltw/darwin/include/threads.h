/* Darwin overlay for <threads.h>: Apple SDKs ship no C11 <threads.h>. ltw's proc.h includes it only
 * for `thread_local` (egl.c's current_context). Use Mesa's bundled C11-threads emulation (pthread
 * backend, HAVE_PTHREAD) for the thrd_/mtx_ API, and the compiler keyword for thread_local. */
#ifndef LTW_DARWIN_THREADS_H
#define LTW_DARWIN_THREADS_H
#include <c11/threads.h>
#ifndef thread_local
#define thread_local _Thread_local
#endif
#endif
