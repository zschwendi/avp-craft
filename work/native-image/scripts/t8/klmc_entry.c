// T8a: the app-facing C entry of libminecraft.dylib, linked next to native-image's object file by relink-xros.sh.
//   int klmc_main(int argc, char **argv)  -> run_main (native-image --shared main-class entry): creates the isolate,
//       parses argv exactly like the T4 executable (argv[0] = program name, then -Xmx/-XX:/-R:/-D... options, then
//       the Minecraft args), runs SpikeT23.main on the CALLING thread, returns its exit status. Blocks until the game
//       returns. Call it from a thread with >= 8 MB of stack (warned below if smaller).
//   const char *klmc_version(void)        -> static build string (KLMC_VERSION, set by relink-xros.sh).
//
// Also: a BOUNDED getaddrinfo for the image (see klmc_dns below). This file is linked into the same dylib as the
// image's static JDK libnet, whose Inet{4,6}AddressImpl reference _getaddrinfo; a definition here wins over the
// libSystem import at static link time, and it is NOT exported (exported_symbols.list), so nothing outside the
// image sees it. The real resolver is looked up in libSystem by handle.
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#ifndef KLMC_VERSION
#define KLMC_VERSION "klmc unknown"
#endif
extern int run_main(int argc, char **argv);
__attribute__((visibility("default"))) const char *klmc_version(void) { return KLMC_VERSION; }
__attribute__((visibility("default"))) int klmc_main(int argc, char **argv) {
  size_t st = pthread_get_stacksize_np(pthread_self());
  fprintf(stderr, "[klmc] %s: klmc_main argc=%d stack=%zu KB%s\n", KLMC_VERSION, argc, st >> 10,
          st < (8u << 20) ? "  WARNING: < 8 MB, call from an 8 MB pthread" : "");
  fflush(stderr);
  return run_main(argc, argv);
}

// ---------------------------------------------------------------------------------------------------------------
// klmc_dns: getaddrinfo with a deadline (M3C launch-colour round, 2026-10-12).
//
// Minecraft 1.20.2+ starts two authlib lookups in the Minecraft constructor (profileFuture: sessionserver.mojang.com,
// userPropertiesFuture: api.minecraftservices.com) and the RENDER THREAD then join()s them before it loads resources
// (26.2: Minecraft.userProperties() right after "Using graphics device extensions"; 1.21.11: before Vivecraft's VR
// init). With the headset's resolver not answering, each lookup sat in getaddrinfo for the resolver's own ~30 s and
// then failed (UnknownHostException), so launches 8 and 9 showed a black immersive space for 30 s on every boot:
// launch 9 session 2, 05:34:31 "Using graphics device extensions" -> 05:35:01 "Failed to fetch user properties" ->
// 05:35:01 "Reloading ResourceManager" -> 05:35:02 VR connected. The game then plays perfectly well offline.
//
// So the lookup runs on a detached worker and the caller waits at most KL_DNS_TIMEOUT_MS (default 3000; 0 = the
// plain call, no deadline). A lookup that misses the deadline fails with EAI_AGAIN — Java turns that into the same
// UnknownHostException the 30-s failure produced, and authlib already handles it — and the worker finishes on its
// own and frees its result. Numeric hosts, NULL hosts and localhost go straight through (no resolver involved).
// KL_DNS_TEST_DELAY_MS=<ms> delays every resolver answer for a DOTTED name by that much: the host-side stand-in for
// a dead upstream resolver (dot-less names such as the Mac's own hostname are answered locally on the device too).
typedef int (*klmc_gai_fn)(const char *, const char *, const struct addrinfo *, struct addrinfo **);
static klmc_gai_fn klmc_real_gai(void) {
  static _Atomic(klmc_gai_fn) fn;
  klmc_gai_fn f = atomic_load(&fn);
  if (f) return f;
  void *h = dlopen("/usr/lib/libSystem.B.dylib", RTLD_LAZY | RTLD_NOLOAD);
  if (h) f = (klmc_gai_fn)dlsym(h, "getaddrinfo");
  if (!f) f = (klmc_gai_fn)dlsym(RTLD_DEFAULT, "getaddrinfo");
  atomic_store(&fn, f);
  return f;
}
static long klmc_env_ms(const char *name, long dflt) {
  const char *e = getenv(name);
  if (!e || !*e) return dflt;
  char *end; long v = strtol(e, &end, 10);
  return (end == e || v < 0) ? dflt : v;
}
struct klmc_dns_job {
  pthread_mutex_t m; pthread_cond_t c;
  _Atomic int refs;            // the waiter + the worker; the last one out frees
  int done, rc;
  struct addrinfo *res;
  char *node, *serv;
  struct addrinfo hints; int has_hints;
  long delay_ms;
};
static void klmc_dns_unref(struct klmc_dns_job *j) {
  if (atomic_fetch_sub(&j->refs, 1) != 1) return;
  if (j->res) freeaddrinfo(j->res);    // only when the waiter gave up: it never took the result
  pthread_mutex_destroy(&j->m); pthread_cond_destroy(&j->c);
  free(j->node); free(j->serv); free(j);
}
static void *klmc_dns_worker(void *arg) {
  struct klmc_dns_job *j = arg;
  if (j->delay_ms > 0) usleep((useconds_t)(j->delay_ms * 1000));
  struct addrinfo *res = NULL;
  int rc = klmc_real_gai()(j->node, j->serv, j->has_hints ? &j->hints : NULL, &res);
  pthread_mutex_lock(&j->m);
  j->rc = rc; j->res = res; j->done = 1;
  pthread_cond_signal(&j->c);
  pthread_mutex_unlock(&j->m);
  klmc_dns_unref(j);
  return NULL;
}
static int klmc_is_literal(const char *node) {
  unsigned char buf[16];
  if (!node) return 1;
  if (!strcasecmp(node, "localhost")) return 1;
  return inet_pton(AF_INET, node, buf) == 1 || inet_pton(AF_INET6, node, buf) == 1;
}
static double klmc_now_ms(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec * 1e3 + tv.tv_usec / 1e3; }

int getaddrinfo(const char *node, const char *serv, const struct addrinfo *hints, struct addrinfo **res) {
  klmc_gai_fn real = klmc_real_gai();
  if (!real || real == getaddrinfo) return EAI_FAIL;
  long tmo = klmc_env_ms("KL_DNS_TIMEOUT_MS", 3000), delay = klmc_env_ms("KL_DNS_TEST_DELAY_MS", 0);
  if (klmc_is_literal(node)) return real(node, serv, hints, res);
  if (tmo == 0 && delay == 0) return real(node, serv, hints, res);
  struct klmc_dns_job *j = calloc(1, sizeof *j);
  if (!j) return real(node, serv, hints, res);
  pthread_mutex_init(&j->m, NULL); pthread_cond_init(&j->c, NULL);
  atomic_store(&j->refs, 2);
  j->node = strdup(node); j->serv = serv ? strdup(serv) : NULL;
  if (hints) { j->hints = *hints; j->hints.ai_canonname = NULL; j->hints.ai_addr = NULL; j->hints.ai_next = NULL; j->has_hints = 1; }
  j->delay_ms = strchr(node, '.') ? delay : 0;
  pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  pthread_t t;
  double t0 = klmc_now_ms();
  if (pthread_create(&t, &a, klmc_dns_worker, j) != 0) {
    pthread_attr_destroy(&a);
    atomic_store(&j->refs, 1); klmc_dns_unref(j);
    return real(node, serv, hints, res);
  }
  pthread_attr_destroy(&a);
  pthread_mutex_lock(&j->m);
  if (tmo == 0) {
    while (!j->done) pthread_cond_wait(&j->c, &j->m);
  } else {
    struct timeval now; gettimeofday(&now, NULL);
    struct timespec dl;
    long long ns = (long long)now.tv_usec * 1000 + (tmo % 1000) * 1000000LL;
    dl.tv_sec = now.tv_sec + tmo / 1000 + (time_t)(ns / 1000000000LL);
    dl.tv_nsec = (long)(ns % 1000000000LL);
    while (!j->done) if (pthread_cond_timedwait(&j->c, &j->m, &dl) == ETIMEDOUT) break;
  }
  int rc;
  if (j->done) {
    rc = j->rc; *res = j->res; j->res = NULL;   // the result is the caller's now
  } else {
    rc = EAI_AGAIN;
    static _Atomic int said;
    if (atomic_fetch_add(&said, 1) < 8)
      fprintf(stderr, "[klmc] dns: getaddrinfo(\"%s\") unanswered after %.0f ms — failed with EAI_AGAIN instead of "
                      "waiting out the resolver (KL_DNS_TIMEOUT_MS=%ld; 0 = no deadline)\n",
              node, klmc_now_ms() - t0, tmo);
  }
  pthread_mutex_unlock(&j->m);
  klmc_dns_unref(j);
  return rc;
}
