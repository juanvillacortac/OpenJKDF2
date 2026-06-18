#if defined(TARGET_LINUX_GLES) || defined(OPENJKDF2_RUNTIME_GL)

#include "Platform/trace_gles.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int openjkdf2_trace_enabled = -1;

static int openjkdf2_trace_is_enabled(void)
{
    if (openjkdf2_trace_enabled < 0) {
        const char *env = getenv("OPENJKDF2_TRACE");
        openjkdf2_trace_enabled = (env && env[0] && env[0] != '0') ? 1 : 0;
    }
    return openjkdf2_trace_enabled;
}

static void openjkdf2_trace_write(const char *msg)
{
    int fd;
    size_t len;

    if (!openjkdf2_trace_is_enabled() || !msg) {
        return;
    }

    fputs(msg, stderr);
    fputc('\n', stderr);
    fflush(stderr);

    fd = open("startup.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return;
    }

    len = strlen(msg);
    if (len > 0) {
        write(fd, msg, len);
    }
    write(fd, "\n", 1);
    close(fd);
}

void openjkdf2_trace(const char *msg)
{
    openjkdf2_trace_write(msg);
}

void openjkdf2_trace_fmt(const char *fmt, ...)
{
    char buf[512];
    va_list args;

    if (!openjkdf2_trace_is_enabled() || !fmt) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    openjkdf2_trace_write(buf);
}

#endif
