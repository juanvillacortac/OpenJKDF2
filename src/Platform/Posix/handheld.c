#include "Platform/handheld.h"

#include <stdlib.h>
#include <string.h>

static int openjkdf2_handheld_mode;

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
