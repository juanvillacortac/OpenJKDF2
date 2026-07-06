#ifndef fcaseopen_h
#define fcaseopen_h

#include <stdio.h>

#if defined(__cplusplus)
extern "C" {
#endif

#ifndef CASEPATH_BUFSIZE
#if !defined(_WIN32) && defined(PATH_MAX)
#define CASEPATH_BUFSIZE PATH_MAX
#else
#define CASEPATH_BUFSIZE 4096
#endif
#endif

extern int casepath(char const *path, char *r);
extern FILE *fcaseopen(char const *path, char const *mode);

extern void casechdir(char const *path);

#if defined(__cplusplus)
}
#endif

#endif
