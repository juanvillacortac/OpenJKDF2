#include "jkMpConf.h"

#include "Gui/jkGUIMultiplayer.h"
#include "Gui/jkGUINetHost.h"
#include "General/stdString.h"
#include "General/util.h"
#include "World/jkPlayer.h"
#include "stdPlatform.h"
#include "jk.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

typedef enum jkMpConfSection
{
    JK_MPCONF_SECTION_NONE = 0,
    JK_MPCONF_SECTION_JOIN,
    JK_MPCONF_SECTION_HOST,
    JK_MPCONF_SECTION_CHARACTER,
} jkMpConfSection;

static int s_joinLoaded;
static int s_hostLoaded;
static char s_joinHost[256];
static wchar_t s_joinPassword[32];
static wchar_t s_hostPassword[32];

static char s_hostEpisode[32];
static char s_hostMap[128];
static int s_hasHostEpisode;
static int s_hasHostMap;
static int s_hostSettingsLoaded;
static int s_hasCharacterName;
static int s_hasCharacterRank;
static wchar_t s_characterName[32];
static int s_characterRank;

static void jkMpConf_Trim(char *line)
{
    char *start = line;
    char *end;

    while (*start && isspace((unsigned char)*start))
        ++start;
    if (start != line)
        memmove(line, start, strlen(start) + 1);

    end = line + strlen(line);
    while (end > line && isspace((unsigned char)end[-1]))
        --end;
    *end = '\0';
}

static int jkMpConf_ParseBool(const char *value)
{
    if (!value || !value[0])
        return 0;
    if (value[0] == '0')
        return 0;
    if (!__strcmpi(value, "false") || !__strcmpi(value, "no") || !__strcmpi(value, "off"))
        return 0;
    return 1;
}

static jkMpConfSection jkMpConf_ParseSection(const char *line)
{
    char name[32];

    if (line[0] != '[')
        return JK_MPCONF_SECTION_NONE;
    if (sscanf(line, "[%31[^]]]", name) != 1)
        return JK_MPCONF_SECTION_NONE;

    if (!__strcmpi(name, "join"))
        return JK_MPCONF_SECTION_JOIN;
    if (!__strcmpi(name, "host"))
        return JK_MPCONF_SECTION_HOST;
    if (!__strcmpi(name, "character"))
        return JK_MPCONF_SECTION_CHARACTER;
    return JK_MPCONF_SECTION_NONE;
}

static void jkMpConf_Reset(void)
{
    s_joinLoaded = 0;
    s_hostLoaded = 0;
    s_hostSettingsLoaded = 0;
    s_hasHostEpisode = 0;
    s_hasHostMap = 0;
    s_joinHost[0] = '\0';
    s_joinPassword[0] = 0;
    s_hostPassword[0] = 0;
    s_hostEpisode[0] = '\0';
    s_hostMap[0] = '\0';
    s_hasCharacterName = 0;
    s_hasCharacterRank = 0;
    s_characterName[0] = 0;
    s_characterRank = 0;
}

static void jkMpConf_ParseLine(jkMpConfSection section, const char *key, const char *value)
{
    wchar_t wbuf[64];

    if (!key[0] || !value[0])
        return;

    if (section == JK_MPCONF_SECTION_JOIN) {
        if (!__strcmpi(key, "host")) {
            strncpy(s_joinHost, value, sizeof(s_joinHost) - 1);
            s_joinHost[sizeof(s_joinHost) - 1] = '\0';
            s_joinLoaded = 1;
        } else if (!__strcmpi(key, "password")) {
            stdString_CharToWchar(s_joinPassword, value, 31);
            s_joinPassword[31] = 0;
            s_joinLoaded = 1;
        }
        return;
    }

    if (section == JK_MPCONF_SECTION_CHARACTER) {
        if (!__strcmpi(key, "name")) {
            stdString_CharToWchar(s_characterName, value, 31);
            s_characterName[31] = 0;
            s_hasCharacterName = s_characterName[0] != 0;
        } else if (!__strcmpi(key, "rank")) {
            s_characterRank = atoi(value);
            if (s_characterRank < 0)
                s_characterRank = 0;
            if (s_characterRank > 8)
                s_characterRank = 8;
            s_hasCharacterRank = 1;
        }
        return;
    }

    if (section != JK_MPCONF_SECTION_HOST)
        return;

    s_hostLoaded = 1;
    s_hostSettingsLoaded = 1;

    if (!__strcmpi(key, "game_name")) {
        stdString_CharToWchar(wbuf, value, 31);
        wbuf[31] = 0;
        stdString_SafeWStrCopy(jkGuiNetHost_gameName, wbuf, 32);
    } else if (!__strcmpi(key, "max_players")) {
        jkGuiNetHost_maxPlayers = atoi(value);
    } else if (!__strcmpi(key, "port")) {
        jkGuiNetHost_portNum = atoi(value);
    } else if (!__strcmpi(key, "episode")) {
        strncpy(s_hostEpisode, value, sizeof(s_hostEpisode) - 1);
        s_hostEpisode[sizeof(s_hostEpisode) - 1] = '\0';
        s_hasHostEpisode = 1;
    } else if (!__strcmpi(key, "map")) {
        strncpy(s_hostMap, value, sizeof(s_hostMap) - 1);
        s_hostMap[sizeof(s_hostMap) - 1] = '\0';
        s_hasHostMap = 1;
    } else if (!__strcmpi(key, "max_rank")) {
        jkGuiNetHost_maxRank = atoi(value);
    } else if (!__strcmpi(key, "score_limit")) {
        jkGuiNetHost_scoreLimit = atoi(value);
        if (jkGuiNetHost_scoreLimit > 0)
            jkGuiNetHost_gameFlags |= MULTIMODEFLAG_SCORELIMIT;
        else
            jkGuiNetHost_gameFlags &= ~MULTIMODEFLAG_SCORELIMIT;
    } else if (!__strcmpi(key, "time_limit")) {
        int minutes = atoi(value);
        jkGuiNetHost_timeLimit = minutes * 60000;
        if (minutes > 0)
            jkGuiNetHost_gameFlags |= MULTIMODEFLAG_TIMELIMIT;
        else
            jkGuiNetHost_gameFlags &= ~MULTIMODEFLAG_TIMELIMIT;
    } else if (!__strcmpi(key, "teams")) {
        if (jkMpConf_ParseBool(value))
            jkGuiNetHost_gameFlags |= (MULTIMODEFLAG_100 | MULTIMODEFLAG_2 | MULTIMODEFLAG_TEAMS);
        else
            jkGuiNetHost_gameFlags &= ~(MULTIMODEFLAG_100 | MULTIMODEFLAG_2 | MULTIMODEFLAG_TEAMS);
    } else if (!__strcmpi(key, "single_level")) {
        if (jkMpConf_ParseBool(value))
            jkGuiNetHost_gameFlags |= MULTIMODEFLAG_SINGLE_LEVEL;
        else
            jkGuiNetHost_gameFlags &= ~MULTIMODEFLAG_SINGLE_LEVEL;
    } else if (!__strcmpi(key, "tick_rate")) {
        jkGuiNetHost_tickRate = atoi(value);
    } else if (!__strcmpi(key, "password")) {
        stdString_CharToWchar(s_hostPassword, value, 31);
        s_hostPassword[31] = 0;
    } else if (!__strcmpi(key, "dedicated")) {
        jkGuiNetHost_bIsDedicated = jkMpConf_ParseBool(value);
    } else if (!__strcmpi(key, "coop")) {
        jkGuiNetHost_bIsCoop = jkMpConf_ParseBool(value);
        jkGuiNetHost_bIsEpisodeCoop = jkGuiNetHost_bIsCoop;
        if (jkGuiNetHost_bIsCoop)
            jkGuiNetHost_gameFlags |= MULTIMODEFLAG_COOP;
        else
            jkGuiNetHost_gameFlags &= ~MULTIMODEFLAG_COOP;
    }
}

static int jkMpConf_LoadFile(const char *path)
{
    FILE *fp;
    char line[512];
    char key[128];
    char value[320];
    jkMpConfSection section = JK_MPCONF_SECTION_NONE;
    char *eq;
    int loaded = 0;

    if (!path || !path[0] || !util_FileExists(path))
        return 0;

    fp = fopen(path, "r");
    if (!fp)
        return 0;

    jkMpConf_Reset();

    while (fgets(line, sizeof(line), fp)) {
        jkMpConf_Trim(line);
        if (!line[0] || line[0] == '#')
            continue;

        if (line[0] == '[') {
            section = jkMpConf_ParseSection(line);
            continue;
        }

        eq = strchr(line, '=');
        if (!eq)
            continue;

        *eq = '\0';
        strncpy(key, line, sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';
        strncpy(value, eq + 1, sizeof(value) - 1);
        value[sizeof(value) - 1] = '\0';
        jkMpConf_Trim(key);
        jkMpConf_Trim(value);

        jkMpConf_ParseLine(section, key, value);
        loaded = 1;
    }

    fclose(fp);
    if (loaded)
        stdPlatform_Printf("OpenJKDF2: loaded multiplayer config from %s\n", path);
    return loaded;
}

static void jkMpConf_TryLoad(void)
{
    char path[512];
    const char *env;

    if (jkMpConf_LoadFile("conf/mp.conf"))
        return;

    env = getenv("XDG_DATA_HOME");
    if (env && env[0]) {
        snprintf(path, sizeof(path), "%s/mp.conf", env);
        if (jkMpConf_LoadFile(path))
            return;
    }

    env = getenv("OPENJKDF2_ROOT");
    if (env && env[0]) {
        snprintf(path, sizeof(path), "%s/../conf/mp.conf", env);
        jkMpConf_LoadFile(path);
    }
}

void jkMpConf_Reload(void)
{
    jkMpConf_TryLoad();
}

void jkMpConf_ApplyJoin(void)
{
    wchar_t hostW[256];

    jkMpConf_TryLoad();
    if (!s_joinLoaded)
        return;

    if (s_joinHost[0]) {
        stdString_CharToWchar(hostW, s_joinHost, 255);
        hostW[255] = 0;
        stdString_SafeWStrCopy(jkGuiMultiplayer_ipText, hostW, 0x100);
        stdPlatform_Printf("OpenJKDF2: mp.conf join host=%s\n", s_joinHost);
    }

    if (s_joinPassword[0])
        stdString_SafeWStrCopy(jkGuiMultiplayer_stru_556168.field_300, s_joinPassword, 0x20);
}

void jkMpConf_ApplyHostSettings(void)
{
    jkMpConf_TryLoad();
    if (!s_hostSettingsLoaded)
        return;

    if (jkGuiNetHost_bIsDedicated)
        jkGuiNetHost_sessionFlags |= SESSIONFLAG_ISDEDICATED;
    else
        jkGuiNetHost_sessionFlags &= ~SESSIONFLAG_ISDEDICATED;

    if (s_hostPassword[0])
        jkGuiNetHost_sessionFlags |= SESSIONFLAG_PASSWORD;
}

int jkMpConf_HasHostEpisode(void)
{
    return s_hasHostEpisode;
}

int jkMpConf_HasHostMap(void)
{
    return s_hasHostMap;
}

const char *jkMpConf_GetHostEpisode(void)
{
    return s_hostEpisode;
}

const char *jkMpConf_GetHostMap(void)
{
    return s_hostMap;
}

void jkMpConf_CopyHostPassword(wchar_t *out, int outChars)
{
    if (!out || outChars <= 0)
        return;
    stdString_SafeWStrCopy(out, s_hostPassword, outChars);
}

int jkMpConf_HasCharacterName(void)
{
    jkMpConf_TryLoad();
    return s_hasCharacterName;
}

int jkMpConf_HasCharacterRank(void)
{
    jkMpConf_TryLoad();
    return s_hasCharacterRank;
}

int jkMpConf_GetCharacterRank(void)
{
    jkMpConf_TryLoad();
    return s_characterRank;
}

void jkMpConf_CopyCharacterName(wchar_t *out, int outChars)
{
    if (!out || outChars <= 0)
        return;
    jkMpConf_TryLoad();
    stdString_SafeWStrCopy(out, s_characterName, outChars);
}

void jkMpConf_PrefillNewCharacter(wchar_t *nameOut, int nameChars, int *rankInOut)
{
    int rank;

    if (!nameOut || nameChars <= 0 || !rankInOut)
        return;

    jkMpConf_TryLoad();

    if (s_hasCharacterName)
        stdString_SafeWStrCopy(nameOut, s_characterName, nameChars);

    if (!s_hasCharacterRank)
        return;

    rank = s_characterRank;
    if (*rankInOut >= 0 && rank > *rankInOut)
        rank = *rankInOut;
    *rankInOut = rank;
}

int jkMpConf_TryLoadCharacter(jkPlayerMpcInfo *info)
{
    wchar_t name[32];
    char nameAscii[32];

    if (!info)
        return 0;

    jkMpConf_TryLoad();
    if (!s_hasCharacterName)
        return 0;

    stdString_SafeWStrCopy(name, s_characterName, 32);
    if (!name[0] || !jkPlayer_VerifyWcharName(name))
        return 0;

    if (!jkPlayer_MPCParse(info, &jkPlayer_playerInfos[playerThingIdx], jkPlayer_playerShortName, name, 1))
        return 0;

    stdString_WcharToChar(nameAscii, name, 31);
    nameAscii[31] = 0;
    stdPlatform_Printf("OpenJKDF2: mp.conf loaded multiplayer character '%s'\n", nameAscii);
    return 1;
}
