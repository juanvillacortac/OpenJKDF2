#if defined(TARGET_LINUX_GLES)

#include "Platform/Posix/gles_loader.h"
#include "Platform/trace_gles.h"

#include <SDL.h>

PFNGLACTIVETEXTUREPROC gles_glActiveTexture;
PFNGLATTACHSHADERPROC gles_glAttachShader;
PFNGLBINDBUFFERPROC gles_glBindBuffer;
PFNGLBINDFRAMEBUFFERPROC gles_glBindFramebuffer;
PFNGLBINDRENDERBUFFERPROC gles_glBindRenderbuffer;
PFNGLBINDTEXTUREPROC gles_glBindTexture;
PFNGLBINDVERTEXARRAYPROC gles_glBindVertexArray;
PFNGLBLENDEQUATIONPROC gles_glBlendEquation;
PFNGLBLENDFUNCPROC gles_glBlendFunc;
PFNGLBUFFERDATAPROC gles_glBufferData;
PFNGLCHECKFRAMEBUFFERSTATUSPROC gles_glCheckFramebufferStatus;
PFNGLCLEARPROC gles_glClear;
PFNGLCLEARCOLORPROC gles_glClearColor;
PFNGLCOMPILESHADERPROC gles_glCompileShader;
PFNGLCREATEPROGRAMPROC gles_glCreateProgram;
PFNGLCREATESHADERPROC gles_glCreateShader;
PFNGLCULLFACEPROC gles_glCullFace;
PFNGLDELETEBUFFERSPROC gles_glDeleteBuffers;
PFNGLDELETEFRAMEBUFFERSPROC gles_glDeleteFramebuffers;
PFNGLDELETEPROGRAMPROC gles_glDeleteProgram;
PFNGLDELETERENDERBUFFERSPROC gles_glDeleteRenderbuffers;
PFNGLDELETESHADERPROC gles_glDeleteShader;
PFNGLDELETETEXTURESPROC gles_glDeleteTextures;
PFNGLDEPTHFUNCPROC gles_glDepthFunc;
PFNGLDEPTHMASKPROC gles_glDepthMask;
PFNGLDISABLEVERTEXATTRIBARRAYPROC gles_glDisableVertexAttribArray;
PFNGLDRAWBUFFERSPROC gles_glDrawBuffers;
PFNGLDRAWELEMENTSPROC gles_glDrawElements;
PFNGLENABLEPROC gles_glEnable;
PFNGLENABLEVERTEXATTRIBARRAYPROC gles_glEnableVertexAttribArray;
PFNGLFLUSHPROC gles_glFlush;
PFNGLFRAMEBUFFERRENDERBUFFERPROC gles_glFramebufferRenderbuffer;
PFNGLFRAMEBUFFERTEXTURE2DPROC gles_glFramebufferTexture2D;
PFNGLGENBUFFERSPROC gles_glGenBuffers;
PFNGLGENERATEMIPMAPPROC gles_glGenerateMipmap;
PFNGLGENFRAMEBUFFERSPROC gles_glGenFramebuffers;
PFNGLGENRENDERBUFFERSPROC gles_glGenRenderbuffers;
PFNGLGENTEXTURESPROC gles_glGenTextures;
PFNGLGENVERTEXARRAYSPROC gles_glGenVertexArrays;
PFNGLGETATTRIBLOCATIONPROC gles_glGetAttribLocation;
PFNGLGETBUFFERPARAMETERIVPROC gles_glGetBufferParameteriv;
PFNGLGETINTEGERVPROC gles_glGetIntegerv;
PFNGLGETPROGRAMINFOLOGPROC gles_glGetProgramInfoLog;
PFNGLGETPROGRAMIVPROC gles_glGetProgramiv;
PFNGLGETSHADERINFOLOGPROC gles_glGetShaderInfoLog;
PFNGLGETSHADERIVPROC gles_glGetShaderiv;
PFNGLGETSTRINGPROC gles_glGetString;
PFNGLGETUNIFORMLOCATIONPROC gles_glGetUniformLocation;
PFNGLISPROGRAMPROC gles_glIsProgram;
PFNGLISSHADERPROC gles_glIsShader;
PFNGLLINKPROGRAMPROC gles_glLinkProgram;
PFNGLPIXELSTOREIPROC gles_glPixelStorei;
PFNGLREADPIXELSPROC gles_glReadPixels;
PFNGLRENDERBUFFERSTORAGEPROC gles_glRenderbufferStorage;
PFNGLSHADERSOURCEPROC gles_glShaderSource;
PFNGLTEXIMAGE2DPROC gles_glTexImage2D;
PFNGLTEXPARAMETERIPROC gles_glTexParameteri;
PFNGLTEXSTORAGE2DPROC gles_glTexStorage2D;
PFNGLTEXSUBIMAGE2DPROC gles_glTexSubImage2D;
PFNGLUNIFORM1FPROC gles_glUniform1f;
PFNGLUNIFORM1IPROC gles_glUniform1i;
PFNGLUNIFORM2FPROC gles_glUniform2f;
PFNGLUNIFORM3FPROC gles_glUniform3f;
PFNGLUNIFORM4FPROC gles_glUniform4f;
PFNGLUNIFORMMATRIX4FVPROC gles_glUniformMatrix4fv;
PFNGLUSEPROGRAMPROC gles_glUseProgram;
PFNGLVERTEXATTRIBPOINTERPROC gles_glVertexAttribPointer;
PFNGLVIEWPORTPROC gles_glViewport;

static void gles_load_one(void **dst, const char *name, int *missing)
{
    *dst = (void *)SDL_GL_GetProcAddress(name);
    if (!*dst) {
        openjkdf2_trace_fmt("gles_loader: missing %s", name);
        (*missing)++;
    }
}

bool gles_loader_init(void)
{
    int missing = 0;

    gles_load_one((void **)&gles_glActiveTexture, "glActiveTexture", &missing);
    gles_load_one((void **)&gles_glAttachShader, "glAttachShader", &missing);
    gles_load_one((void **)&gles_glBindBuffer, "glBindBuffer", &missing);
    gles_load_one((void **)&gles_glBindFramebuffer, "glBindFramebuffer", &missing);
    gles_load_one((void **)&gles_glBindRenderbuffer, "glBindRenderbuffer", &missing);
    gles_load_one((void **)&gles_glBindTexture, "glBindTexture", &missing);
    gles_load_one((void **)&gles_glBindVertexArray, "glBindVertexArray", &missing);
    gles_load_one((void **)&gles_glBlendEquation, "glBlendEquation", &missing);
    gles_load_one((void **)&gles_glBlendFunc, "glBlendFunc", &missing);
    gles_load_one((void **)&gles_glBufferData, "glBufferData", &missing);
    gles_load_one((void **)&gles_glCheckFramebufferStatus, "glCheckFramebufferStatus", &missing);
    gles_load_one((void **)&gles_glClear, "glClear", &missing);
    gles_load_one((void **)&gles_glClearColor, "glClearColor", &missing);
    gles_load_one((void **)&gles_glCompileShader, "glCompileShader", &missing);
    gles_load_one((void **)&gles_glCreateProgram, "glCreateProgram", &missing);
    gles_load_one((void **)&gles_glCreateShader, "glCreateShader", &missing);
    gles_load_one((void **)&gles_glCullFace, "glCullFace", &missing);
    gles_load_one((void **)&gles_glDeleteBuffers, "glDeleteBuffers", &missing);
    gles_load_one((void **)&gles_glDeleteFramebuffers, "glDeleteFramebuffers", &missing);
    gles_load_one((void **)&gles_glDeleteProgram, "glDeleteProgram", &missing);
    gles_load_one((void **)&gles_glDeleteRenderbuffers, "glDeleteRenderbuffers", &missing);
    gles_load_one((void **)&gles_glDeleteShader, "glDeleteShader", &missing);
    gles_load_one((void **)&gles_glDeleteTextures, "glDeleteTextures", &missing);
    gles_load_one((void **)&gles_glDepthFunc, "glDepthFunc", &missing);
    gles_load_one((void **)&gles_glDepthMask, "glDepthMask", &missing);
    gles_load_one((void **)&gles_glDisableVertexAttribArray, "glDisableVertexAttribArray", &missing);
    gles_load_one((void **)&gles_glDrawBuffers, "glDrawBuffers", &missing);
    gles_load_one((void **)&gles_glDrawElements, "glDrawElements", &missing);
    gles_load_one((void **)&gles_glEnable, "glEnable", &missing);
    gles_load_one((void **)&gles_glEnableVertexAttribArray, "glEnableVertexAttribArray", &missing);
    gles_load_one((void **)&gles_glFlush, "glFlush", &missing);
    gles_load_one((void **)&gles_glFramebufferRenderbuffer, "glFramebufferRenderbuffer", &missing);
    gles_load_one((void **)&gles_glFramebufferTexture2D, "glFramebufferTexture2D", &missing);
    gles_load_one((void **)&gles_glGenBuffers, "glGenBuffers", &missing);
    gles_load_one((void **)&gles_glGenerateMipmap, "glGenerateMipmap", &missing);
    gles_load_one((void **)&gles_glGenFramebuffers, "glGenFramebuffers", &missing);
    gles_load_one((void **)&gles_glGenRenderbuffers, "glGenRenderbuffers", &missing);
    gles_load_one((void **)&gles_glGenTextures, "glGenTextures", &missing);
    gles_load_one((void **)&gles_glGenVertexArrays, "glGenVertexArrays", &missing);
    gles_load_one((void **)&gles_glGetAttribLocation, "glGetAttribLocation", &missing);
    gles_load_one((void **)&gles_glGetBufferParameteriv, "glGetBufferParameteriv", &missing);
    gles_load_one((void **)&gles_glGetIntegerv, "glGetIntegerv", &missing);
    gles_load_one((void **)&gles_glGetProgramInfoLog, "glGetProgramInfoLog", &missing);
    gles_load_one((void **)&gles_glGetProgramiv, "glGetProgramiv", &missing);
    gles_load_one((void **)&gles_glGetShaderInfoLog, "glGetShaderInfoLog", &missing);
    gles_load_one((void **)&gles_glGetShaderiv, "glGetShaderiv", &missing);
    gles_load_one((void **)&gles_glGetString, "glGetString", &missing);
    gles_load_one((void **)&gles_glGetUniformLocation, "glGetUniformLocation", &missing);
    gles_load_one((void **)&gles_glIsProgram, "glIsProgram", &missing);
    gles_load_one((void **)&gles_glIsShader, "glIsShader", &missing);
    gles_load_one((void **)&gles_glLinkProgram, "glLinkProgram", &missing);
    gles_load_one((void **)&gles_glPixelStorei, "glPixelStorei", &missing);
    gles_load_one((void **)&gles_glReadPixels, "glReadPixels", &missing);
    gles_load_one((void **)&gles_glRenderbufferStorage, "glRenderbufferStorage", &missing);
    gles_load_one((void **)&gles_glShaderSource, "glShaderSource", &missing);
    gles_load_one((void **)&gles_glTexImage2D, "glTexImage2D", &missing);
    gles_load_one((void **)&gles_glTexParameteri, "glTexParameteri", &missing);
    gles_load_one((void **)&gles_glTexStorage2D, "glTexStorage2D", &missing);
    gles_load_one((void **)&gles_glTexSubImage2D, "glTexSubImage2D", &missing);
    gles_load_one((void **)&gles_glUniform1f, "glUniform1f", &missing);
    gles_load_one((void **)&gles_glUniform1i, "glUniform1i", &missing);
    gles_load_one((void **)&gles_glUniform2f, "glUniform2f", &missing);
    gles_load_one((void **)&gles_glUniform3f, "glUniform3f", &missing);
    gles_load_one((void **)&gles_glUniform4f, "glUniform4f", &missing);
    gles_load_one((void **)&gles_glUniformMatrix4fv, "glUniformMatrix4fv", &missing);
    gles_load_one((void **)&gles_glUseProgram, "glUseProgram", &missing);
    gles_load_one((void **)&gles_glVertexAttribPointer, "glVertexAttribPointer", &missing);
    gles_load_one((void **)&gles_glViewport, "glViewport", &missing);

    openjkdf2_trace_fmt("gles_loader: %d symbols loaded, %d missing", 69 - missing, missing);
    return missing == 0;
}

#endif
