/* Simple lightweight debug logger for instrumenting runtime events */
#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

extern int xh_debug_enabled;
void xh_debug_log(const char *fmt, ...);

#endif
