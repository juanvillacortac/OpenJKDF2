#include "Platform/handheld.h"
#include "Platform/gl_backend.h"

#include "types.h"
#include "Main/Main.h"
#include "World/jkPlayer.h"
#include "General/stdFont.h"
#include "General/stdString.h"
#include "Platform/std3D.h"
#include "Engine/rdMaterial.h"
#include "Devices/sithSound.h"
#include "stdPlatform.h"

#include "jk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Boards with ~1 GB RAM (e.g. RG CubeXX) often report 950k–1.1M kB MemTotal. */
#define OPENJKDF2_LOW_MEMORY_AUTO_KB 1250000L

static int openjkdf2_handheld_mode;

#define SSAA_AUTO_MAX                   1.0f
#define SSAA_AUTO_STEP                  0.125f
#define SSAA_AUTO_MIN                   0.5f
#define SSAA_AUTO_MIN_LOWMEM            0.375f
#define SSAA_AUTO_TARGET_FPS_DEFAULT    58.0f
#define SSAA_AUTO_TARGET_MARGIN         6.0f
#define SSAA_AUTO_UP_MARGIN             2.0f
#define SSAA_AUTO_WARMUP_MS             3000
#define SSAA_AUTO_EVAL_MS               500
#define SSAA_AUTO_UP_HOLD_MS            3000
#define SSAA_AUTO_EMA_ALPHA             0.15f

static int openjkdf2_ssaa_auto_enabled;
static int openjkdf2_ssaa_auto_initialized;
static float openjkdf2_ssaa_auto_min;
static float openjkdf2_ssaa_auto_max;
static float openjkdf2_ssaa_auto_target_fps;
static float openjkdf2_ssaa_auto_down_fps;
static float openjkdf2_ssaa_auto_up_fps;
static float openjkdf2_ssaa_auto_ema_fps;
static uint32_t openjkdf2_ssaa_auto_last_eval_ms;
static uint32_t openjkdf2_ssaa_auto_above_target_since_ms;
static uint32_t openjkdf2_ssaa_auto_warmup_until_ms;

static int openjkdf2_world_loading;

static wchar_t openjkdf2_fps_hud_text[80];
static int openjkdf2_fps_hud_visible;

static int openjkdf2_parse_env_toggle(const char *env);

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

static float openjkdf2_ssaa_auto_parse_target_fps(void)
{
    const char *env = getenv("OPENJKDF2_SSAA_TARGET");
    float target = SSAA_AUTO_TARGET_FPS_DEFAULT;

    if (env && env[0]) {
        float v = (float)atof(env);
        if (v > 120.0f) {
            fprintf(stderr,
                "OpenJKDF2: OPENJKDF2_SSAA_TARGET=%s out of range, using 120\n", env);
            v = 120.0f;
        } else if (v < 30.0f) {
            fprintf(stderr,
                "OpenJKDF2: OPENJKDF2_SSAA_TARGET=%s out of range, using 30\n", env);
            v = 30.0f;
        }
        target = v;
    }
    return target;
}

static void openjkdf2_ssaa_auto_configure_thresholds(void)
{
    openjkdf2_ssaa_auto_target_fps = openjkdf2_ssaa_auto_parse_target_fps();
    openjkdf2_ssaa_auto_down_fps = openjkdf2_ssaa_auto_target_fps - SSAA_AUTO_TARGET_MARGIN;
    if (openjkdf2_ssaa_auto_down_fps < 20.0f)
        openjkdf2_ssaa_auto_down_fps = 20.0f;
    openjkdf2_ssaa_auto_up_fps = openjkdf2_ssaa_auto_target_fps - SSAA_AUTO_UP_MARGIN;
    if (openjkdf2_ssaa_auto_up_fps < openjkdf2_ssaa_auto_down_fps)
        openjkdf2_ssaa_auto_up_fps = openjkdf2_ssaa_auto_down_fps;
}

static void openjkdf2_ssaa_auto_begin_warmup(void)
{
    openjkdf2_ssaa_auto_warmup_until_ms = stdPlatform_GetTimeMsec() + SSAA_AUTO_WARMUP_MS;
}

static int openjkdf2_lowmem_purge_enabled(void)
{
    int toggle = openjkdf2_parse_env_toggle(getenv("OPENJKDF2_LOW_MEMORY_PURGE"));

    if (toggle == 0)
        return 0;
    if (toggle == 1)
        return 1;
    return openjkdf2_IsLowMemoryMode();
}

static int openjkdf2_gles_warmup_enabled(void)
{
    int toggle = openjkdf2_parse_env_toggle(getenv("OPENJKDF2_GLES_WARMUP"));

    if (toggle == 0)
        return 0;
    if (toggle == 1)
        return 1;
#if defined(TARGET_LINUX_GLES) && !defined(OPENJKDF2_RUNTIME_GL)
    return openjkdf2_IsHandheld();
#elif defined(OPENJKDF2_RUNTIME_GL)
if (openjkdf2_UseGLES()) {
    return openjkdf2_IsHandheld();
} else {
    return 0;
}
#else
    return 0;
#endif
}

static void openjkdf2_LowMemoryPurgeCaches(void)
{
#if defined(SDL2_RENDER) || defined(TARGET_LINUX_GLES)
    if (!openjkdf2_lowmem_purge_enabled())
        return;

    fprintf(stderr, "OpenJKDF2: low-memory cache purge\n");

#if defined(RDMATERIAL_LRU_LOAD_UNLOAD)
    rdMaterial_PurgeEntireMaterialCache();
#endif
    if (std3D_IsReady())
        std3D_PurgeEntireTextureCache();

    sithSound_FreeUpMemory(1024 * 1024);
#endif
}

static void openjkdf2_OnDynamicResolutionDown(void)
{
    if (openjkdf2_IsLowMemoryMode())
        openjkdf2_LowMemoryPurgeCaches();
}

void openjkdf2_OnLevelLoadComplete(void)
{
    /* GLES warmup after load crashed on Mali during menu/loading transitions.
     * First in-game frames compile shaders lazily; skip explicit warmup here. */
}

void openjkdf2_SetWorldLoading(int loading)
{
    openjkdf2_world_loading = loading != 0;
}

int openjkdf2_IsWorldLoading(void)
{
    return openjkdf2_world_loading;
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

void openjkdf2_SetFpsHudText(const char *text)
{
    if (!text || !text[0]) {
        openjkdf2_fps_hud_visible = 0;
        openjkdf2_fps_hud_text[0] = 0;
        return;
    }

    stdString_CharToWchar(openjkdf2_fps_hud_text, text, 79);
    openjkdf2_fps_hud_text[79] = 0;
    openjkdf2_fps_hud_visible = 1;
}

void openjkdf2_ClearFpsHud(void)
{
    openjkdf2_fps_hud_visible = 0;
    openjkdf2_fps_hud_text[0] = 0;
}

void openjkdf2_DrawFpsHud(void)
{
    if (!openjkdf2_fps_hud_visible || !jkHud_pMsgFontSft || !jkHud_pMsgFontSft->pBitmap)
        return;

    stdFont_Draw1GPU(
        jkHud_pMsgFontSft,
        HUD_SCALED(8),
        HUD_SCALED(8),
        640,
        openjkdf2_fps_hud_text,
        1,
        jkPlayer_hudScale);
}

long openjkdf2_GetProcessRssKb(void)
{
#if defined(LINUX) || defined(__linux__)
    FILE *f;
    char line[128];
    long rss_kb = -1;

    f = fopen("/proc/self/status", "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "VmRSS: %ld kB", &rss_kb) == 1)
            break;
    }
    fclose(f);
    return rss_kb;
#else
    return -1;
#endif
}

int openjkdf2_IsSsaaAutoActive(void)
{
    return openjkdf2_ssaa_auto_enabled;
}

void openjkdf2_InitSsaaAuto(void)
{
    if (openjkdf2_ssaa_auto_initialized)
        return;
    if (!openjkdf2_ssaa_auto_should_enable())
        return;

    openjkdf2_ssaa_auto_enabled = 1;
    openjkdf2_ssaa_auto_min = openjkdf2_IsLowMemoryMode() ? SSAA_AUTO_MIN_LOWMEM : SSAA_AUTO_MIN;
    openjkdf2_ssaa_auto_max = SSAA_AUTO_MAX;
    if (openjkdf2_ssaa_auto_max < openjkdf2_ssaa_auto_min)
        openjkdf2_ssaa_auto_max = openjkdf2_ssaa_auto_min;

    openjkdf2_ssaa_auto_configure_thresholds();
    jkPlayer_ssaaMultiple = openjkdf2_ssaa_auto_max;
    openjkdf2_ssaa_auto_ema_fps = 0.0f;
    openjkdf2_ssaa_auto_last_eval_ms = stdPlatform_GetTimeMsec();
    openjkdf2_ssaa_auto_above_target_since_ms = 0;
    openjkdf2_ssaa_auto_begin_warmup();
    openjkdf2_ssaa_auto_initialized = 1;

    fprintf(stderr,
        "OpenJKDF2: SSAA auto enabled (%.2f-%.2f, target %.0f FPS, down <%.0f, up >=%.0f)\n",
        openjkdf2_ssaa_auto_min, openjkdf2_ssaa_auto_max, openjkdf2_ssaa_auto_target_fps,
        openjkdf2_ssaa_auto_down_fps, openjkdf2_ssaa_auto_up_fps);
}

void openjkdf2_SsaaAutoOnSettingsLoaded(void)
{
    if (!openjkdf2_ssaa_auto_enabled || !openjkdf2_ssaa_auto_initialized)
        return;

    jkPlayer_ssaaMultiple = openjkdf2_ssaa_auto_max;
    openjkdf2_ssaa_auto_ema_fps = 0.0f;
    openjkdf2_ssaa_auto_above_target_since_ms = 0;
    openjkdf2_ssaa_auto_begin_warmup();
    fprintf(stderr, "OpenJKDF2: SSAA auto reset to %.2f after settings load\n",
        openjkdf2_ssaa_auto_max);
}

void openjkdf2_SsaaAutoOnFrame(uint32_t frame_ms)
{
    float instant_fps;
    float cur;
    float new_ssaa;
    uint32_t now;

    if (!openjkdf2_ssaa_auto_enabled || !openjkdf2_ssaa_auto_initialized)
        return;
    if (frame_ms == 0)
        return;
    if (frame_ms > 2000)
        frame_ms = 2000;

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

    if (openjkdf2_ssaa_auto_ema_fps < openjkdf2_ssaa_auto_down_fps) {
        if (now >= openjkdf2_ssaa_auto_warmup_until_ms)
            new_ssaa = openjkdf2_ssaa_auto_quantize(cur - SSAA_AUTO_STEP);
        openjkdf2_ssaa_auto_above_target_since_ms = 0;
    } else if (openjkdf2_ssaa_auto_ema_fps >= openjkdf2_ssaa_auto_up_fps) {
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
        if (new_ssaa < cur)
            openjkdf2_OnDynamicResolutionDown();
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
