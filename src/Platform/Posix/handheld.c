#include "Platform/handheld.h"

#include "types.h"
#include "World/jkPlayer.h"
#include "stdPlatform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Boards with ~1 GB RAM (e.g. RG CubeXX) often report 950k–1.1M kB MemTotal. */
#define OPENJKDF2_LOW_MEMORY_AUTO_KB 1250000L

static int openjkdf2_handheld_mode;

#define SSAA_AUTO_STEP          0.125f
#define SSAA_AUTO_MIN           0.5f
#define SSAA_AUTO_MIN_LOWMEM    0.375f
#define SSAA_AUTO_TARGET_FPS    58.0f
#define SSAA_AUTO_DOWN_FPS      52.0f
#define SSAA_AUTO_UP_FPS        58.0f
#define SSAA_AUTO_EVAL_MS       500
#define SSAA_AUTO_UP_HOLD_MS    3000
#define SSAA_AUTO_EMA_ALPHA     0.08f

static int openjkdf2_ssaa_auto_enabled;
static int openjkdf2_ssaa_auto_initialized;
static float openjkdf2_ssaa_auto_min;
static float openjkdf2_ssaa_auto_max;
static float openjkdf2_ssaa_auto_ema_fps;
static uint32_t openjkdf2_ssaa_auto_last_eval_ms;
static uint32_t openjkdf2_ssaa_auto_above_target_since_ms;

static float openjkdf2_ssaa_auto_clamp(float v)
{
    if (v < openjkdf2_ssaa_auto_min)
        return openjkdf2_ssaa_auto_min;
    if (v > openjkdf2_ssaa_auto_max)
        return openjkdf2_ssaa_auto_max;
    return v;
}

static float openjkdf2_ssaa_auto_quantize(float v)
{
    int steps = (int)(v / SSAA_AUTO_STEP + 0.5f);

    return openjkdf2_ssaa_auto_clamp((float)steps * SSAA_AUTO_STEP);
}

static int openjkdf2_ssaa_auto_should_enable(void)
{
    int toggle = openjkdf2_parse_env_toggle(getenv("OPENJKDF2_SSAA_AUTO"));

    if (toggle == 0)
        return 0;
    if (toggle == 1)
        return 1;
    return openjkdf2_IsHandheld();
}

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

void openjkdf2_InitSsaaAuto(void)
{
    const char *ssaa_env;
    float max_ssaa;

    if (openjkdf2_ssaa_auto_initialized)
        return;
    if (!openjkdf2_ssaa_auto_should_enable())
        return;

    ssaa_env = getenv("OPENJKDF2_SSAA");
    max_ssaa = jkPlayer_ssaaMultiple;
    if (ssaa_env && ssaa_env[0]) {
        float env_ssaa = (float)atof(ssaa_env);
        if (env_ssaa >= 0.25f && env_ssaa <= 1.0f)
            max_ssaa = env_ssaa;
    }
    if (max_ssaa < 0.25f)
        max_ssaa = 0.25f;
    if (max_ssaa > 1.0f)
        max_ssaa = 1.0f;

    openjkdf2_ssaa_auto_enabled = 1;
    openjkdf2_ssaa_auto_min = openjkdf2_IsLowMemoryMode() ? SSAA_AUTO_MIN_LOWMEM : SSAA_AUTO_MIN;
    openjkdf2_ssaa_auto_max = max_ssaa;
    if (openjkdf2_ssaa_auto_max < openjkdf2_ssaa_auto_min)
        openjkdf2_ssaa_auto_max = openjkdf2_ssaa_auto_min;

    jkPlayer_ssaaMultiple = openjkdf2_ssaa_auto_max;
    openjkdf2_ssaa_auto_ema_fps = SSAA_AUTO_TARGET_FPS;
    openjkdf2_ssaa_auto_last_eval_ms = stdPlatform_GetTimeMsec();
    openjkdf2_ssaa_auto_above_target_since_ms = 0;
    openjkdf2_ssaa_auto_initialized = 1;

    fprintf(stderr,
        "OpenJKDF2: SSAA auto enabled (%.2f-%.2f, target %.0f FPS)\n",
        openjkdf2_ssaa_auto_min, openjkdf2_ssaa_auto_max, SSAA_AUTO_TARGET_FPS);
}

void openjkdf2_SsaaAutoOnFrame(uint32_t frame_ms)
{
    float instant_fps;
    float cur;
    float new_ssaa;
    uint32_t now;

    if (!openjkdf2_ssaa_auto_enabled || !openjkdf2_ssaa_auto_initialized)
        return;
    if (frame_ms == 0 || frame_ms > 500)
        return;

    instant_fps = 1000.0f / (float)frame_ms;
    if (openjkdf2_ssaa_auto_ema_fps <= 0.0f)
        openjkdf2_ssaa_auto_ema_fps = instant_fps;
    else
        openjkdf2_ssaa_auto_ema_fps += (instant_fps - openjkdf2_ssaa_auto_ema_fps) * SSAA_AUTO_EMA_ALPHA;

    now = stdPlatform_GetTimeMsec();
    if (now - openjkdf2_ssaa_auto_last_eval_ms < SSAA_AUTO_EVAL_MS)
        return;
    openjkdf2_ssaa_auto_last_eval_ms = now;

    cur = jkPlayer_ssaaMultiple;
    new_ssaa = cur;

    if (openjkdf2_ssaa_auto_ema_fps < SSAA_AUTO_DOWN_FPS) {
        new_ssaa = openjkdf2_ssaa_auto_quantize(cur - SSAA_AUTO_STEP);
        openjkdf2_ssaa_auto_above_target_since_ms = 0;
    } else if (openjkdf2_ssaa_auto_ema_fps >= SSAA_AUTO_UP_FPS) {
        if (openjkdf2_ssaa_auto_above_target_since_ms == 0)
            openjkdf2_ssaa_auto_above_target_since_ms = now;
        else if (now - openjkdf2_ssaa_auto_above_target_since_ms >= SSAA_AUTO_UP_HOLD_MS) {
            new_ssaa = openjkdf2_ssaa_auto_quantize(cur + SSAA_AUTO_STEP);
            openjkdf2_ssaa_auto_above_target_since_ms = now;
        }
    } else {
        openjkdf2_ssaa_auto_above_target_since_ms = 0;
    }

    if (new_ssaa != cur) {
        fprintf(stderr,
            "OpenJKDF2: SSAA auto %.2f -> %.2f (%.1f FPS avg)\n",
            cur, new_ssaa, openjkdf2_ssaa_auto_ema_fps);
        jkPlayer_ssaaMultiple = new_ssaa;
    }
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
