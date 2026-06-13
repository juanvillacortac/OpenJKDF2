#ifndef OPENJKDF2_HANDHELD_H
#define OPENJKDF2_HANDHELD_H

/* Runtime handheld mode: set OPENJKDF2_HANDHELD=1 in the launcher (no auto-detection). */
void openjkdf2_InitHandheldMode(void);
int openjkdf2_IsHandheld(void);

/*
 * Low-memory tuning for 1 GB handhelds (sound cache cap, material purge).
 * OPENJKDF2_LOW_MEMORY=1 forces on, =0 forces off; unset auto-detects via /proc/meminfo.
 * Does not skip cutscenes.
 *
 * Reduced texture LOD (smallest mipmap only): follows low memory unless
 * OPENJKDF2_TEXTURE_LOD=1/0 overrides.
 */
void openjkdf2_InitLowMemoryMode(void);
int openjkdf2_IsLowMemoryMode(void);
void openjkdf2_InitTextureLodMode(void);
int openjkdf2_IsTextureLodReduced(void);

#endif
