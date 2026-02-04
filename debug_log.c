#include "debug_log.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

int xh_debug_enabled = 0;

void xh_debug_log(const char *fmt, ...)
{
    if (!xh_debug_enabled) return;

    static char buf[4096];
    struct timeval tv;

    gettimeofday(&tv, NULL);
    int off = snprintf(buf, sizeof(buf), "%ld.%06ld ", (long)tv.tv_sec, (long)tv.tv_usec);
    
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + off, sizeof(buf) - off, fmt, ap);
    va_end(ap);
    
    fputs(buf, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}
