#ifndef OPENJKDF2_HANDHELD_H
#define OPENJKDF2_HANDHELD_H

#include "types.h"

/* Runtime handheld mode: set OPENJKDF2_HANDHELD=1 in the launcher (no auto-detection). */
void openjkdf2_InitHandheldMode(void);
int openjkdf2_IsHandheld(void);

/* In-game FPS overlay (cheat "fpsview" / cheats menu). */
void openjkdf2_SetFpsHudText(const char *text);
void openjkdf2_ClearFpsHud(void);
void openjkdf2_DrawFpsHud(void);
/* Process RSS in kB from /proc/self/status; -1 if unavailable. */
long openjkdf2_GetProcessRssKb(void);

/*
 * Low-memory tuning for 1 GB handhelds (sound cache cap, material purge).
 * OPENJKDF2_LOW_MEMORY=1 forces on, =0 forces off; unset auto-detects via /proc/meminfo.
 * Does not skip cutscenes.
 */
void openjkdf2_InitLowMemoryMode(void);
int openjkdf2_IsLowMemoryMode(void);

/*
 * Dynamic SSAA for handheld: scales jkPlayer_ssaaMultiple toward OPENJKDF2_SSAA_TARGET FPS.
 * Enabled by default in handheld mode; OPENJKDF2_SSAA_AUTO=0 disables.
 * Maximum render scale is always 1.0; OPENJKDF2_SSAA_TARGET sets the FPS goal (default 58, range 30-120).
 * SSAA steps down when average FPS stays below target - 6 (default target 58 -> down below 52 FPS).
 * Frame timing matches the fpsview overlay (wall-clock per frame, not CPU-only).
 * Runtime SSAA changes are in-memory only; they are not written to the player config while auto is on.
 */
void openjkdf2_InitSsaaAuto(void);
void openjkdf2_SsaaAutoOnFrame(uint32_t frame_ms);
void openjkdf2_SsaaAutoOnSettingsLoaded(void);
int openjkdf2_IsSsaaAutoActive(void);

/*
 * GLES shader warmup after level load.
 * OPENJKDF2_GLES_WARMUP: default on in handheld GLES builds.
 */
void openjkdf2_OnLevelLoadComplete(void);

/* True only while sithWorld_Load is parsing a .jkl (not the same as sithWorld_pLoading). */
void openjkdf2_SetWorldLoading(int loading);
int openjkdf2_IsWorldLoading(void);

#endif
