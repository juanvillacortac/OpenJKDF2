#ifndef OPENJKDF2_LINUX_DISPLAY_H
#define OPENJKDF2_LINUX_DISPLAY_H

/* Linux VT / KMS setup — call before SDL_Init. */
void openjkdf2_InitLinuxDisplayEnv(void);

/* True when running on a bare Linux VT and kmsdrm was selected. */
int openjkdf2_IsLinuxKmsDisplay(void);

/* Restore text console after kmsdrm (call after SDL_Quit). */
void openjkdf2_RestoreLinuxConsole(void);

#endif
