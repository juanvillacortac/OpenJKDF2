#include "fcaseopen.h"

#if !defined(_WIN32)
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <dirent.h>
#include <errno.h>
#include <unistd.h>

static void casepath_norm_inplace(char *p)
{
    char *r = p;
    char *w = p;

    while (*r) {
        char c = (*r == '\\') ? '/' : *r;
        if (!(c == '/' && w > p && *(w - 1) == '/')) {
            *w++ = c;
        }
        r++;
    }
    *w = '\0';

    while (w > p && *(w - 1) == '/') {
        *(--w) = '\0';
    }
}

static FILE *fcaseopen_resolved(const char *path, const char *mode)
{
    char *resolved = malloc(strlen(path) + 16);
    FILE *f = NULL;

    if (!resolved) {
        return NULL;
    }

    if (casepath(path, resolved)) {
        f = fopen(resolved, mode);
    }

    free(resolved);
    return f;
}

static int fcaseopen_swap_archive_ext(char *buf, size_t cap, const char *path)
{
    size_t len = strlen(path);

    if (len < 4 || len >= cap) {
        return 0;
    }

    strcpy(buf, path);

    if (!strcasecmp(buf + len - 4, ".goo")) {
        buf[len - 1] = 'b';
        return 1;
    }

    if (!strcasecmp(buf + len - 4, ".gob")) {
        buf[len - 1] = 'o';
        return 1;
    }

    return 0;
}

static FILE *fcaseopen_try(const char *path, const char *mode)
{
    FILE *f = fopen(path, mode);

    if (f) {
        return f;
    }

    f = fcaseopen_resolved(path, mode);
    if (f) {
        return f;
    }

    return NULL;
}

// r must have strlen(path) + 16 bytes
int casepath(char const *path, char *r)
{
    size_t l = strlen(path);
    char *work;
    int absolute;
    DIR *d;
    size_t rl;
    char *cursor;
    char *comp;

    if (!path || !r || l == 0) {
        return 0;
    }

    work = alloca(l + 2);
    strcpy(work, path);
    casepath_norm_inplace(work);
    if (work[0] == '\0') {
        return 0;
    }

    absolute = (work[0] == '/');
    if (absolute) {
        d = opendir("/");
        r[0] = '\0';
        rl = 0;
        cursor = work + 1;
    } else {
        d = opendir(".");
        r[0] = '.';
        r[1] = '\0';
        rl = 1;
        cursor = work;
    }

    if (!d) {
        return 0;
    }

    comp = cursor;
    while (comp && *comp) {
        char *slash = strchr(comp, '/');
        int is_last = (slash == NULL);
        struct dirent *e;
        struct dirent *match = NULL;

        if (slash) {
            *slash = '\0';
        }

        if (*comp == '\0') {
            if (slash) {
                comp = slash + 1;
            } else {
                comp = NULL;
            }
            continue;
        }

        if (!d) {
            return 0;
        }

        r[rl] = '/';
        rl += 1;
        r[rl] = '\0';

        for (e = readdir(d); e; e = readdir(d)) {
            if (strcasecmp(comp, e->d_name) == 0) {
                match = e;
                break;
            }
        }
        closedir(d);
        d = NULL;

        if (!match) {
            return 0;
        }

        strcpy(r + rl, match->d_name);
        rl += strlen(match->d_name);

        if (!is_last) {
            d = opendir(r);
            if (!d) {
                return 0;
            }
        }

        comp = slash ? slash + 1 : NULL;
    }

    if (d) {
        closedir(d);
    }

    return 1;
}
#endif

FILE *fcaseopen(char const *path, char const *mode)
{
    FILE *f = fcaseopen_try(path, mode);

#if !defined(_WIN32)
    if (!f) {
        char alt[512];

        if (fcaseopen_swap_archive_ext(alt, sizeof(alt), path)) {
            f = fcaseopen_try(alt, mode);
        }
    }
#endif

    return f;
}

void casechdir(char const *path)
{
#if !defined(_WIN32)
    char *r = malloc(strlen(path) + 16);
    if (casepath(path, r))
    {
        chdir(r);
    }
    else
    {
        errno = ENOENT;
    }
    if (r)
        free(r);
#else
    chdir(path);
#endif
}
