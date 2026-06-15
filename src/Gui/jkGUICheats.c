#include "jkGUICheats.h"

#ifdef QOL_IMPROVEMENTS

#include "General/Darray.h"
#include "General/stdBitmap.h"
#include "General/stdString.h"
#include "stdPlatform.h"
#include "jk.h"
#include "Gui/jkGUIRend.h"
#include "Gui/jkGUI.h"
#include "Main/jkDev.h"
#include "Main/Main.h"
#include "Dss/sithMulti.h"

#include <stdlib.h>
#include <string.h>

#define JKGUI_CHEATS_ENV "OPENJKDF2_CHEATS_MENU"

enum jkGuiCheatsButton_t
{
    JKGUICHEATS_BTN_LISTCLICK = 1,
};

typedef struct jkGuiCheatsEntry_t
{
    char *paCheatCmd;
} jkGuiCheatsEntry_t;

static int32_t jkGuiCheats_listboxBitmapIndices[2] = {14, 15};

static jkGuiElement jkGuiCheats_aElements[5] = {
    {ELEMENT_TEXT, 0, 5, L"Cheats", 3, {0, 30, 640, 60}, 1, 0, 0, 0, 0, 0, {0}, 0},
    {ELEMENT_LISTBOX, JKGUICHEATS_BTN_LISTCLICK, 2, 0, 0, {80, 120, 480, 260}, 1, 0, 0, 0, 0, jkGuiCheats_listboxBitmapIndices, {0}, 0},
    {ELEMENT_TEXTBUTTON, 1, 2, "GUI_OK", 3, {340, 420, 140, 40}, 1, 0, 0, 0, 0, 0, {0}, 0},
    {ELEMENT_TEXTBUTTON, -1, 2, "GUI_CANCEL", 3, {150, 420, 180, 40}, 1, 0, 0, 0, 0, 0, {0}, 0},
    {ELEMENT_END, 0, 0, 0, 0, {0}, 0, 0, 0, 0, 0, 0, {0}, 0}
};

static jkGuiMenu jkGuiCheats_menu = {jkGuiCheats_aElements, -1, 0xFFFF, 0xFFFF, 0xF, 0, 0, jkGui_stdBitmaps, jkGui_stdFonts, 0, 0, "thermloop01.wav", "thrmlpu2.wav", 0, 0, 0, 0, 0, 0};

static int jkGuiCheats_bInitted;
static int jkGuiCheats_bPendingEndLevel;

int jkGuiCheats_IsEnabled(void)
{
    const char *env = getenv(JKGUI_CHEATS_ENV);
    return env && env[0] == '1' && env[1] == '\0';
}

static void jkGuiCheats_AddEntry(Darray *pListDisplayed, const char *paCheatCmd, const char *paDisplayed)
{
    size_t alloc_sz = (_strlen(paDisplayed) + 1) * sizeof(wchar_t);
    wchar_t *out = (wchar_t *)pHS->alloc(alloc_sz);
    jkGuiCheatsEntry_t *pEntry;
    char *paCheatCmdNew;

    memset(out, 0, alloc_sz);
    pEntry = (jkGuiCheatsEntry_t *)pHS->alloc(sizeof(jkGuiCheatsEntry_t));
    memset(pEntry, 0, sizeof(jkGuiCheatsEntry_t));

    paCheatCmdNew = (char *)pHS->alloc(_strlen(paCheatCmd) + 1);
    stdString_SafeStrCopy(paCheatCmdNew, paCheatCmd, _strlen(paCheatCmd) + 1);
    pEntry->paCheatCmd = paCheatCmdNew;

    stdString_CharToWchar(out, paDisplayed, _strlen(paDisplayed));
    jkGuiRend_DarrayReallocStr(pListDisplayed, out, (intptr_t)pEntry);
    pHS->free(out);
}

static void jkGuiCheats_PopulateEntries(Darray *pListDisplayed, jkGuiElement *element)
{
    if (Main_bMotsCompat) {
        jkGuiCheats_AddEntry(pListDisplayed, "morelife", "Heal");
        jkGuiCheats_AddEntry(pListDisplayed, "diediedie", "All weapons");
        jkGuiCheats_AddEntry(pListDisplayed, "gimmestuff", "All items");
        jkGuiCheats_AddEntry(pListDisplayed, "boinga", "Invulnerable");
        jkGuiCheats_AddEntry(pListDisplayed, "cartograph", "Full map");
        jkGuiCheats_AddEntry(pListDisplayed, "iamagod", "Uber Jedi");
        jkGuiCheats_AddEntry(pListDisplayed, "noclip", "Noclip");
        jkGuiCheats_AddEntry(pListDisplayed, "checkmate", "Next checkpoint");
        jkGuiCheats_AddEntry(pListDisplayed, "fpsview", "Toggle FPS");
        jkGuiCheats_AddEntry(pListDisplayed, "gameover", "End level");
    } else {
        jkGuiCheats_AddEntry(pListDisplayed, "bactame", "Heal");
        jkGuiCheats_AddEntry(pListDisplayed, "red5", "All weapons");
        jkGuiCheats_AddEntry(pListDisplayed, "wamprat", "All items");
        jkGuiCheats_AddEntry(pListDisplayed, "jediwannabe", "Invulnerable");
        jkGuiCheats_AddEntry(pListDisplayed, "5858lvr", "Full map");
        jkGuiCheats_AddEntry(pListDisplayed, "raccoonking", "Uber Jedi");
        jkGuiCheats_AddEntry(pListDisplayed, "noclip", "Noclip");
        jkGuiCheats_AddEntry(pListDisplayed, "checkmate", "Next checkpoint");
        jkGuiCheats_AddEntry(pListDisplayed, "fpsview", "Toggle FPS");
        jkGuiCheats_AddEntry(pListDisplayed, "thereisnotry", "End level");
    }

    jkGuiRend_AddStringEntry(pListDisplayed, 0, 0);
    jkGuiRend_SetClickableString(element, pListDisplayed);
    element->selectedTextEntry = 0;
}

static int jkGuiCheats_IsEndLevelCheat(const char *paCheatCmd)
{
    return paCheatCmd
        && (!strcmp(paCheatCmd, "thereisnotry") || !strcmp(paCheatCmd, "gameover"));
}

static void jkGuiCheats_FreeEntries(Darray *pListDisplayed)
{
    int idx = 0;
    jkGuiCheatsEntry_t *entry;

    for (entry = (jkGuiCheatsEntry_t *)jkGuiRend_GetId(pListDisplayed, idx); entry; entry = (jkGuiCheatsEntry_t *)jkGuiRend_GetId(pListDisplayed, ++idx)) {
        pHS->free(entry->paCheatCmd);
        pHS->free(entry);
    }
}

void jkGuiCheats_Startup(void)
{
    if (jkGuiCheats_bInitted)
        return;

    jkGui_InitMenu(&jkGuiCheats_menu, jkGui_stdBitmaps[JKGUI_BM_BK_SETUP]);
    jkGuiCheats_bInitted = 1;
}

void jkGuiCheats_Shutdown(void)
{
    jkGuiCheats_bInitted = 0;
}

int jkGuiCheats_HasPendingEndLevel(void)
{
    return jkGuiCheats_bPendingEndLevel;
}

int jkGuiCheats_ConsumePendingEndLevel(void)
{
    int pending = jkGuiCheats_bPendingEndLevel;

    jkGuiCheats_bPendingEndLevel = 0;
    return pending;
}

void jkGuiCheats_Show(void)
{
    Darray darray;
    int clicked;

    if (!jkGuiCheats_IsEnabled() || sithNet_isMulti)
        return;

    stdBitmap_EnsureData(jkGui_stdBitmaps[JKGUI_BM_BK_MAIN]);
    jkGui_SetModeMenu(jkGui_stdBitmaps[JKGUI_BM_BK_MAIN]->palette);
    jkGuiRend_DarrayNewStr(&darray, 16, 1);
    jkGuiCheats_PopulateEntries(&darray, &jkGuiCheats_aElements[1]);

    do {
        jkGuiRend_MenuSetReturnKeyShortcutElement(&jkGuiCheats_menu, &jkGuiCheats_aElements[2]);
        jkGuiRend_MenuSetEscapeKeyShortcutElement(&jkGuiCheats_menu, &jkGuiCheats_aElements[3]);
        clicked = jkGuiRend_DisplayAndReturnClicked(&jkGuiCheats_menu);

        if (clicked == JKGUICHEATS_BTN_LISTCLICK) {
            jkGuiCheatsEntry_t *pEntry = (jkGuiCheatsEntry_t *)jkGuiRend_GetId(&darray, jkGuiCheats_aElements[1].selectedTextEntry);
            if (pEntry && pEntry->paCheatCmd) {
                if (jkGuiCheats_IsEndLevelCheat(pEntry->paCheatCmd)) {
                    jkGuiCheats_bPendingEndLevel = 1;
                    break;
                }
                jkDev_TryCommand(pEntry->paCheatCmd);
            }
        }
    } while (clicked != -1);

    jkGuiCheats_FreeEntries(&darray);
    jkGuiRend_DarrayFree(&darray);
}

#else

int jkGuiCheats_IsEnabled(void) { return 0; }
void jkGuiCheats_Startup(void) {}
void jkGuiCheats_Shutdown(void) {}
void jkGuiCheats_Show(void) {}
int jkGuiCheats_HasPendingEndLevel(void) { return 0; }
int jkGuiCheats_ConsumePendingEndLevel(void) { return 0; }

#endif
