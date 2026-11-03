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

#ifdef __cplusplus
extern "C" {
#endif
extern FILE *sm64ds_log_file;   // set by vita_main.cpp before the test runs

static inline int sm64ds_vfprintf(FILE *stream, const char *fmt, va_list ap)
{
    if (sm64ds_log_file && (stream == stdout || stream == stderr))
        stream = sm64ds_log_file;
    return vfprintf(stream, fmt, ap);
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

#define printf  sm64ds_printf
#define fprintf sm64ds_fprintf

#endif
