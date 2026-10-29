/* Darwin overlay for <android/log.h>: only the subset ltw uses (proc.c). Implemented in
 * darwin/ltw_darwin.c on top of stderr (+ os_log on Apple platforms). */
#ifndef LTW_DARWIN_ANDROID_LOG_H
#define LTW_DARWIN_ANDROID_LOG_H
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum android_LogPriority {
    ANDROID_LOG_UNKNOWN = 0, ANDROID_LOG_DEFAULT, ANDROID_LOG_VERBOSE, ANDROID_LOG_DEBUG,
    ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR, ANDROID_LOG_FATAL, ANDROID_LOG_SILENT,
} android_LogPriority;
int __android_log_print(int prio, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4), visibility("hidden")));
int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)
    __attribute__((visibility("hidden")));
int __android_log_write(int prio, const char *tag, const char *text)
    __attribute__((visibility("hidden")));
#ifdef __cplusplus
}
#endif
#endif
