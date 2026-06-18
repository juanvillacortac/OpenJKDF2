#ifndef OPENJKDF2_GL_BACKEND_H
#define OPENJKDF2_GL_BACKEND_H

#include "types.h"

typedef enum openjkdf2_gl_backend_e {
    OPENJKDF2_GL_BACKEND_DESKTOP = 0,
    OPENJKDF2_GL_BACKEND_GLES = 1,
} openjkdf2_gl_backend_t;

extern openjkdf2_gl_backend_t openjkdf2_gl_backend;

openjkdf2_gl_backend_t openjkdf2_PreferGLBackend(void);
void openjkdf2_SetGLBackend(openjkdf2_gl_backend_t backend);
int openjkdf2_UseGLES(void);
int openjkdf2_UseGLESPath(void);
void openjkdf2_InitGLBackendFromContext(void);

#endif
