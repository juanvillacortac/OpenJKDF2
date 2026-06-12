#ifndef OPENJKDF2_HANDHELD_H
#define OPENJKDF2_HANDHELD_H

/* Runtime handheld mode: set OPENJKDF2_HANDHELD=1 in the launcher (no auto-detection). */
void openjkdf2_InitHandheldMode(void);
int openjkdf2_IsHandheld(void);

#endif
