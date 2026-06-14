#include "fcaseopen.h"

#if !defined(_WIN32)
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <dirent.h>
#include <errno.h>
#include <unistd.h>

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

#if 0
static int is_directory(const char *path) {
   struct stat statbuf;
   if (stat(path, &statbuf) != 0)
       return 0;
   return S_ISDIR(statbuf.st_mode);
}
#endif

// r must have strlen(path) + 2 bytes
int casepath(char const *path, char *r)
{
    size_t l = strlen(path);
    char *p = alloca(l + 16);
    strcpy(p, path);
    size_t rl = 0;
    
    DIR *d;
    if (p[0] == '/')
    {
        d = opendir("/");
        p = p + 1;
    }
    else
    {
        d = opendir(".");
        r[0] = '.';
        r[1] = 0;
        rl = 1;
    }
    
    int last = 0;
    char *c = strsep(&p, "/");
    while (c)
    {
        if (!d)
        {
            return 0;
        }
        
        if (last)
        {
            closedir(d);
            return 0;
        }
        
        r[rl] = '/';
        rl += 1;
        r[rl] = 0;
        
        struct dirent *e = readdir(d);
        while (e)
        {
            if (strcasecmp(c, e->d_name) == 0)
            {
                strcpy(r + rl, e->d_name);
                rl += strlen(e->d_name);

                closedir(d);
                d = opendir(r);
                
                break;
            }
            
            e = readdir(d);
        }
        
        if (!e)
        {
            strcpy(r + rl, c);
            rl += strlen(c);
            last = 1;
        }

        c = strsep(&p, "/");
    }

#if 0
    // Added
    if(is_directory(r)) {
        strcat(r, "/");
    }
#endif
    
    if (d) closedir(d);
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
