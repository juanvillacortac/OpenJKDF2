#include "Platform/handheld.h"

#include "types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Boards with ~1 GB RAM (e.g. RG CubeXX) often report 950k–1.1M kB MemTotal. */
#define OPENJKDF2_LOW_MEMORY_AUTO_KB 1250000L

static int openjkdf2_handheld_mode;

static int openjkdf2_parse_env_toggle(const char *env)
{
    if (!env || !env[0])
        return -1;
    if (env[0] == '0')
        return 0;
    if (!strcmp(env, "false") || !strcmp(env, "FALSE") || !strcmp(env, "off") || !strcmp(env, "OFF"))
        return 0;
    return 1;
}

static long openjkdf2_probe_system_ram_kb(void)
{
#if defined(LINUX) || defined(__linux__)
    FILE *f;
    char line[128];
    long mem_kb = 0;

    f = fopen("/proc/meminfo", "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %ld kB", &mem_kb) == 1)
            break;
    }
    fclose(f);
    return mem_kb;
#else
    return 0;
#endif
}

int openjkdf2_IsHandheld(void)
{
    return openjkdf2_handheld_mode;
}

void openjkdf2_InitHandheldMode(void)
{
#if defined(TARGET_TWL)
    openjkdf2_handheld_mode = 1;
    return;
#endif
    const char *env = getenv("OPENJKDF2_HANDHELD");
    if (!env || !env[0])
        return;
    if (env[0] == '0')
        return;
    if (!strcmp(env, "false") || !strcmp(env, "FALSE") || !strcmp(env, "off") || !strcmp(env, "OFF"))
        return;
    openjkdf2_handheld_mode = 1;
}

int openjkdf2_IsLowMemoryMode(void)
{
    return openjkdf2_bIsLowMemoryPlatform != 0;
}

void openjkdf2_InitLowMemoryMode(void)
{
    const char *env = getenv("OPENJKDF2_LOW_MEMORY");
    int toggle = openjkdf2_parse_env_toggle(env);

#if defined(TARGET_TWL)
    /* DS/TWL sets these flags from main.c based on expansion RAM. */
    return;
#endif

    if (toggle == 0) {
        openjkdf2_bIsLowMemoryPlatform = 0;
        fprintf(stderr, "OpenJKDF2: low memory mode disabled (OPENJKDF2_LOW_MEMORY=0)\n");
        return;
    }

    if (toggle == 1) {
        openjkdf2_bIsLowMemoryPlatform = 1;
        fprintf(stderr, "OpenJKDF2: low memory mode enabled (OPENJKDF2_LOW_MEMORY=1)\n");
        return;
    }

    {
        long mem_kb = openjkdf2_probe_system_ram_kb();
        if (mem_kb > 0 && mem_kb <= OPENJKDF2_LOW_MEMORY_AUTO_KB) {
            openjkdf2_bIsLowMemoryPlatform = 1;
            fprintf(stderr,
                "OpenJKDF2: low memory mode auto-enabled (MemTotal=%ld kB, threshold<=%ld kB)\n",
                mem_kb, OPENJKDF2_LOW_MEMORY_AUTO_KB);
        }
    }
}

int openjkdf2_IsTextureLodReduced(void)
{
    return openjkdf2_bTextureLodReduced != 0;
}

void openjkdf2_InitTextureLodMode(void)
{
    const char *env = getenv("OPENJKDF2_TEXTURE_LOD");
    int toggle = openjkdf2_parse_env_toggle(env);

#if defined(TARGET_TWL)
    openjkdf2_bTextureLodReduced = 1;
    return;
#endif

    if (toggle == 1) {
        openjkdf2_bTextureLodReduced = 1;
        fprintf(stderr, "OpenJKDF2: reduced texture LOD enabled (OPENJKDF2_TEXTURE_LOD=1)\n");
        return;
    }

    if (toggle == 0) {
        openjkdf2_bTextureLodReduced = 0;
        fprintf(stderr, "OpenJKDF2: reduced texture LOD disabled (OPENJKDF2_TEXTURE_LOD=0)\n");
        return;
    }

    openjkdf2_bTextureLodReduced = openjkdf2_bIsLowMemoryPlatform;
    if (openjkdf2_bTextureLodReduced) {
        fprintf(stderr, "OpenJKDF2: reduced texture LOD enabled (low memory mode)\n");
    }
}
