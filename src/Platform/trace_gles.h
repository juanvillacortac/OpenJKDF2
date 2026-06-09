#ifndef _OPENJKDF2_TRACE_GLES_H
#define _OPENJKDF2_TRACE_GLES_H

#if defined(TARGET_LINUX_GLES)
void openjkdf2_trace(const char *msg);
void openjkdf2_trace_fmt(const char *fmt, ...);
#else
#define openjkdf2_trace(msg) ((void)0)
#define openjkdf2_trace_fmt(fmt, ...) ((void)0)
#endif

#endif
