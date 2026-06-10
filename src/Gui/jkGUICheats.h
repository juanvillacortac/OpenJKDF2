#ifndef _JKGUI_CHEATS_H
#define _JKGUI_CHEATS_H

#include "types.h"

/* Handheld cheats submenu; gated at runtime by OPENJKDF2_CHEATS_MENU=1 */
int jkGuiCheats_IsEnabled(void);
void jkGuiCheats_Startup(void);
void jkGuiCheats_Shutdown(void);
void jkGuiCheats_Show(void);
int jkGuiCheats_HasPendingEndLevel(void);
int jkGuiCheats_ConsumePendingEndLevel(void);

#endif // _JKGUI_CHEATS_H
