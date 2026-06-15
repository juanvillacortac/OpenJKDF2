#ifndef OPENJKDF2_HANDHELD_H
#define OPENJKDF2_HANDHELD_H

#include "types.h"

/* Runtime handheld mode: set OPENJKDF2_HANDHELD=1 in the launcher (no auto-detection). */
void openjkdf2_InitHandheldMode(void);
int openjkdf2_IsHandheld(void);

/*
 * Low-memory tuning for 1 GB handhelds (sound cache cap, material purge).
 * OPENJKDF2_LOW_MEMORY=1 forces on, =0 forces off; unset auto-detects via /proc/meminfo.
 * Does not skip cutscenes.
 */
void openjkdf2_InitLowMemoryMode(void);
int openjkdf2_IsLowMemoryMode(void);

/*
 * Dynamic SSAA for handheld: scales jkPlayer_ssaaMultiple to hold ~58 FPS.
 * Enabled by default in handheld mode; OPENJKDF2_SSAA_AUTO=0 disables.
 * OPENJKDF2_SSAA (if set) caps the maximum; rendering starts at max and steps down.
 */
void openjkdf2_InitSsaaAuto(void);
void openjkdf2_SsaaAutoOnFrame(uint32_t frame_ms);

#endif
