#include "Platform/gl_backend.h"
#include "Platform/linux_display.h"

#include "SDL2_helper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

openjkdf2_gl_backend_t openjkdf2_gl_backend = OPENJKDF2_GL_BACKEND_DESKTOP;

static int openjkdf2_parse_force_env(const char *name)
{
    const char *env = getenv(name);
    if (!env || !env[0])
        return 0;
    if (!strcmp(env, "1") || !strcasecmp(env, "true") || !strcasecmp(env, "yes") || !strcasecmp(env, "on"))
        return 1;
    return 0;
}

openjkdf2_gl_backend_t openjkdf2_PreferGLBackend(void)
{
#if defined(OPENJKDF2_RUNTIME_GL)
    if (openjkdf2_parse_force_env("OPENJKDF2_FORCE_GLES"))
        return OPENJKDF2_GL_BACKEND_GLES;
    if (openjkdf2_parse_force_env("OPENJKDF2_FORCE_GL"))
        return OPENJKDF2_GL_BACKEND_DESKTOP;
    if (openjkdf2_IsLinuxKmsDisplay())
        return OPENJKDF2_GL_BACKEND_GLES;
#if defined(__aarch64__)
    return OPENJKDF2_GL_BACKEND_GLES;
#else
    return OPENJKDF2_GL_BACKEND_DESKTOP;
#endif
#elif defined(TARGET_LINUX_GLES) || defined(TARGET_ANDROID)
    return OPENJKDF2_GL_BACKEND_GLES;
#else
    return OPENJKDF2_GL_BACKEND_DESKTOP;
#endif
}

void openjkdf2_SetGLBackend(openjkdf2_gl_backend_t backend)
{
    openjkdf2_gl_backend = backend;
}

int openjkdf2_UseGLES(void)
{
#if defined(TARGET_ANDROID)
    return 1;
#elif defined(TARGET_LINUX_GLES) && !defined(OPENJKDF2_RUNTIME_GL)
    return 1;
#elif defined(OPENJKDF2_RUNTIME_GL)
    return openjkdf2_gl_backend == OPENJKDF2_GL_BACKEND_GLES;
#else
    return 0;
#endif
}

int openjkdf2_UseGLESPath(void)
{
#if defined(TARGET_ANDROID)
    return 1;
#else
    return openjkdf2_UseGLES();
#endif
}

void openjkdf2_InitGLBackendFromContext(void)
{
#if defined(OPENJKDF2_RUNTIME_GL)
    int profile = 0;
    if (SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &profile) == 0
        && profile == SDL_GL_CONTEXT_PROFILE_ES) {
        openjkdf2_gl_backend = OPENJKDF2_GL_BACKEND_GLES;
    } else {
        openjkdf2_gl_backend = OPENJKDF2_GL_BACKEND_DESKTOP;
    }
#endif
}
