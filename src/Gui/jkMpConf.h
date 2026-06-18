#ifndef _JKMPCONF_H
#define _JKMPCONF_H

#include "types.h"

void jkMpConf_Reload(void);
void jkMpConf_ApplyJoin(void);
void jkMpConf_ApplyHostSettings(void);
int jkMpConf_HasHostEpisode(void);
int jkMpConf_HasHostMap(void);
const char *jkMpConf_GetHostEpisode(void);
const char *jkMpConf_GetHostMap(void);
void jkMpConf_CopyHostPassword(wchar_t *out, int outChars);
int jkMpConf_HasCharacterName(void);
int jkMpConf_HasCharacterRank(void);
int jkMpConf_GetCharacterRank(void);
void jkMpConf_CopyCharacterName(wchar_t *out, int outChars);
void jkMpConf_PrefillNewCharacter(wchar_t *nameOut, int nameChars, int *rankInOut);
int jkMpConf_TryLoadCharacter(jkPlayerMpcInfo *info);

#endif
