// T8a host sanity: dlopen libminecraft.dylib (env KLMC_DYLIB, default <exe dir>/libminecraft.dylib), print
// klmc_version(), then call klmc_main(argc, argv) -- our own argv passed through unchanged, argv[0] included --
// on a fresh pthread with an 8 MB stack (KLMC_STACK_MB overrides), the way the app shell will. Exit = its status.
#include <dlfcn.h>
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int (*main_fn)(int, char **);
typedef const char *(*ver_fn)(void);
static main_fn klmc_main; static int g_argc; static char **g_argv; static int g_rc = -1;
static void *run(void *u) { (void)u; g_rc = klmc_main(g_argc, g_argv); return NULL; }
int main(int argc, char **argv) {
  char path[PATH_MAX]; const char *p = getenv("KLMC_DYLIB");
  if (!p) { uint32_t n = sizeof path; char exe[PATH_MAX]; _NSGetExecutablePath(exe, &n);
            snprintf(path, sizeof path, "%s/libminecraft.dylib", dirname(exe)); p = path; }
  void *h = dlopen(p, RTLD_NOW | RTLD_LOCAL);
  if (!h) { fprintf(stderr, "[klmc-host] dlopen %s: %s\n", p, dlerror()); return 2; }
  ver_fn ver = (ver_fn)dlsym(h, "klmc_version"); klmc_main = (main_fn)dlsym(h, "klmc_main");
  if (!ver || !klmc_main) { fprintf(stderr, "[klmc-host] missing klmc_* in %s\n", p); return 3; }
  fprintf(stderr, "[klmc-host] loaded %s: %s\n", p, ver());
  g_argc = argc; g_argv = argv;
  size_t mb = getenv("KLMC_STACK_MB") ? strtoul(getenv("KLMC_STACK_MB"), NULL, 10) : 8;
  pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, mb << 20);
  pthread_t t; if (pthread_create(&t, &a, run, NULL)) { perror("pthread_create"); return 4; }
  pthread_join(t, NULL);
  fprintf(stderr, "[klmc-host] klmc_main returned %d\n", g_rc);
  return g_rc;
}
