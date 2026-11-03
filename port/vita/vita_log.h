// Force-included (-include) into each gate's test file on the Vita build.
//
// On the console, neither freopen() nor assigning stdout/stderr reaches
// printf in VitaSDK's newlib: the launcher's own lines landed in the log, the
// test's lines did not. So the test's printf/fprintf are rerouted here at
// compile time instead, which does not depend on how the C library binds its
// standard streams. The gates only print through printf and fprintf.
#ifndef SM64DS_VITA_LOG_H
#define SM64DS_VITA_LOG_H

#include <stdio.h>    // declarations first, so the macros below never touch them
#include <stdarg.h>
#include <string.h>
#ifdef __cplusplus
// <cstdio> #undefs printf/fprintf when included. Include it HERE, before the
// macros, so the translation unit's own later #include <cstdio> is a no-op.
#include <cstdio>
#endif

#ifdef __cplusplus
extern "C" {
#endif
extern FILE *sm64ds_log_file;   // set by vita_main.cpp before the test runs

static inline int sm64ds_vfprintf(FILE *stream, const char *fmt, va_list ap)
{
    if (sm64ds_log_file && (stream == stdout || stream == stderr))
        stream = sm64ds_log_file;
    // VitaSDK's newlib printf has no C99 `z' length modifier: "%zu" prints
    // "zu" and shifts every later argument (seen on hardware in gate 4a).
    // size_t is unsigned int on this 32-bit target, so drop the `z'.
    char buf[512];
    size_t o = 0;
    for (const char *p = fmt; *p && o + 1 < sizeof buf; ++p) {
        buf[o++] = *p;
        if (*p != '%') continue;
        // copy flags/width/precision, then skip a `z' before the conversion
        while (p[1] && strchr("-+ #0123456789.*", p[1]) && o + 1 < sizeof buf)
            buf[o++] = *++p;
        if (p[1] == 'z') ++p;
        else if (p[1] == '%' && o + 1 < sizeof buf) buf[o++] = *++p;
    }
    buf[o] = 0;
    return vfprintf(stream, buf, ap);
}
static inline int sm64ds_printf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = sm64ds_vfprintf(stdout, fmt, ap);
    va_end(ap); return n;
}
static inline int sm64ds_fprintf(FILE *stream, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = sm64ds_vfprintf(stream, fmt, ap);
    va_end(ap); return n;
}
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
// Code that spells std::printf / std::fprintf expands to std::sm64ds_*.
namespace std { using ::sm64ds_printf; using ::sm64ds_fprintf; }
#endif

#define printf  sm64ds_printf
#define fprintf sm64ds_fprintf

#endif
