#include "Platform/std3D.h"

#include "Raster/rdCache.h"
#include "Win95/stdDisplay.h"
#include "Win95/Window.h"
#include "World/sithWorld.h"
#include "Engine/rdColormap.h"
#include "Main/jkGame.h"
#include "World/jkPlayer.h"
#include "General/stdBitmap.h"
#include "stdPlatform.h"

#include "jk.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "Platform/GL/shader_utils.h"
#include "Platform/GL/jkgm.h"

#include "SDL2_helper.h"
#include "Platform/trace_gles.h"
#include "Platform/handheld.h"

#ifdef WIN32
// Force Optimus/AMD to use non-integrated GPUs by default.
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif

#if defined(TARGET_LINUX_GLES)
extern SDL_Window *displayWindow;
extern SDL_GLContext glWindowContext;

static int std3D_glContextCurrent;

static int std3D_EnsureGLContext(void)
{
    if (!displayWindow || !glWindowContext) {
        openjkdf2_trace_fmt("std3D_EnsureGLContext: missing window=%p context=%p",
            (void*)displayWindow, (void*)glWindowContext);
        return 0;
    }
    if (std3D_glContextCurrent)
        return 1;
    if (SDL_GL_MakeCurrent(displayWindow, glWindowContext) != 0) {
        openjkdf2_trace_fmt("std3D_EnsureGLContext: MakeCurrent failed: %s", SDL_GetError());
        return 0;
    }
    std3D_glContextCurrent = 1;
    return 1;
}

void std3D_InvalidateGLContext(void)
{
    std3D_glContextCurrent = 0;
}
#endif

#define STD3D_FB_EXTRA_BLOOM 1
#define STD3D_FB_EXTRA_SSAO_BLUR 2
#define STD3D_FB_EXTRA_POSNORM_GBUFFER 8

static size_t std3D_worldVboCap;
static size_t std3D_worldIboCap;
static size_t std3D_menuVboCap;
static size_t std3D_menuIboCap;
static int std3D_menuBufferDirty = 1;

#if defined(TARGET_LINUX_GLES)
static int std3D_menuTexIsRgba = 0;

extern size_t std3D_loadedTexturesAmt;

static int std3D_bPurgeTexturesOnEnd = 0;
static uint32_t std3D_lastPurgeMs = 0;

static void std3D_RequestDeferredTexturePurge(void)
{
    std3D_bPurgeTexturesOnEnd = 1;
}

static int std3D_GlesCheckUploadError(GLuint *tex, const char *what)
{
    GLenum err = glGetError();

    if (err == GL_NO_ERROR)
        return 1;

    if (tex && *tex) {
        glDeleteTextures(1, tex);
        *tex = 0;
    }
    std3D_RequestDeferredTexturePurge();
    openjkdf2_trace_fmt("std3D_AddToTextureCache: %s GL error 0x%x", what, err);
    return 0;
}

static void std3D_RunDeferredTexturePurge(void)
{
    if (!std3D_bPurgeTexturesOnEnd)
        return;
    if (openjkdf2_IsWorldLoading())
        return;
    if (stdPlatform_GetTimeMsec() - std3D_lastPurgeMs <= 1000)
        return;

    std3D_PurgeEntireTextureCache();
    std3D_bPurgeTexturesOnEnd = 0;
    std3D_lastPurgeMs = stdPlatform_GetTimeMsec();
}

static int std3D_GlesRecoverTextureCacheSlots(void)
{
    if (std3D_loadedTexturesAmt < STD3D_MAX_TEXTURES)
        return 1;

#if defined(RDMATERIAL_LRU_LOAD_UNLOAD)
    if (rdMaterial_PurgeMaterialCache())
        return std3D_loadedTexturesAmt < STD3D_MAX_TEXTURES;
#endif

    std3D_RequestDeferredTexturePurge();
    return 0;
}
#endif

static void std3D_uploadBuffer(GLuint buf, GLenum target, size_t size, const void *data, size_t *cap)
{
    glBindBuffer(target, buf);
#if defined(TARGET_LINUX_GLES)
    /* Mali: glBufferSubData often stalls; orphan via glBufferData is smoother. */
    glBufferData(target, size, data, GL_DYNAMIC_DRAW);
    if (size > *cap)
        *cap = size;
#else
    if (!data || size > *cap) {
        glBufferData(target, size, data, GL_DYNAMIC_DRAW);
        if (size > *cap)
            *cap = size;
    } else {
        glBufferSubData(target, 0, size, data);
    }
#endif
}

void std3D_MarkMenuBufferDirty(void)
{
    std3D_menuBufferDirty = 1;
}

#if defined(TARGET_LINUX_GLES)
static void std3D_SyncDisplayPalette(int force);

void std3D_NotifyMenuPaletteChange(void)
{
    std3D_MarkMenuBufferDirty();
    std3D_SyncDisplayPalette(1);
}
#endif

#define TEX_MODE_TEST 0
#define TEX_MODE_WORLDPAL 1
#define TEX_MODE_BILINEAR 2
#define TEX_MODE_16BPP 5
#define TEX_MODE_BILINEAR_16BPP 6

typedef struct std3DSimpleTexStage
{
    GLuint program;

    GLint attribute_coord3d;
    GLint attribute_v_color;
    GLint attribute_v_uv;
    GLint attribute_v_norm;

    GLint uniform_mvp;
    GLint uniform_tex;
    GLint uniform_tex2;
    GLint uniform_tex3;
    GLint uniform_iResolution;

    GLint uniform_param1;
    GLint uniform_param2;
    GLint uniform_param3;
} std3DSimpleTexStage;

typedef struct std3DIntermediateFbo
{
    GLuint fbo;
    GLuint tex;

    GLuint rbo;
    int32_t w;
    int32_t h;

    int32_t iw;
    int32_t ih;
} std3DIntermediateFbo;

typedef struct std3DFramebuffer
{
    GLuint fbo;
    GLuint tex0;
    GLuint tex1;
    GLuint tex2;
    GLuint tex3;

    std3DIntermediateFbo window;
    std3DIntermediateFbo main;

    int enable_extra;
    std3DIntermediateFbo blur1;
    std3DIntermediateFbo blur2;
    std3DIntermediateFbo blur3;
    std3DIntermediateFbo blur4;
    //std3DIntermediateFbo blurBlend;

    std3DIntermediateFbo ssaoBlur1;
    std3DIntermediateFbo ssaoBlur2;
    //std3DIntermediateFbo ssaoBlur3;

    GLuint rbo;
    int32_t w;
    int32_t h;
} std3DFramebuffer;

GLint std3D_windowFbo = 0;
std3DFramebuffer std3D_framebuffers[2];
std3DFramebuffer *std3D_pFb = NULL;

static bool has_initted = false;

static void* last_overlay = NULL;

static int std3D_activeFb = 1;

int init_once = 0;
GLuint programDefault, programDefaultLite, programMenu;
GLint attribute_coord3d, attribute_v_color, attribute_v_light, attribute_v_uv, attribute_v_norm;
GLint uniform_mvp, uniform_tex, uniform_texEmiss, uniform_displacement_map, uniform_tex_mode, uniform_blend_mode, uniform_worldPalette, uniform_worldPaletteLights;
GLint uniform_tint, uniform_filter, uniform_fade, uniform_add, uniform_emissiveFactor, uniform_albedoFactor;
GLint uniform_light_mult, uniform_displacement_factor, uniform_iResolution;

GLint programMenu_attribute_coord3d, programMenu_attribute_v_color, programMenu_attribute_v_uv, programMenu_attribute_v_norm;
GLint programMenu_uniform_mvp, programMenu_uniform_tex, programMenu_uniform_displayPalette;
GLint programMenu_uniform_worldPalette, programMenu_uniform_menuIndexed;

#if defined(TARGET_LINUX_GLES)
static uint8_t *std3D_cutsceneRgbaCache = NULL;
static size_t std3D_cutsceneRgbaCacheBytes = 0;
#endif

static GLint programDefault_cached_attribute_coord3d, programDefault_cached_attribute_v_color;
static GLint programDefault_cached_attribute_v_light, programDefault_cached_attribute_v_uv;
static GLint programDefault_cached_uniform_mvp, programDefault_cached_uniform_tex, programDefault_cached_uniform_texEmiss;
static GLint programDefault_cached_uniform_worldPalette, programDefault_cached_uniform_worldPaletteLights;
static GLint programDefault_cached_uniform_displacement_map, programDefault_cached_uniform_tex_mode, programDefault_cached_uniform_blend_mode;
static GLint programDefault_cached_uniform_tint, programDefault_cached_uniform_filter, programDefault_cached_uniform_fade, programDefault_cached_uniform_add;
static GLint programDefault_cached_uniform_emissiveFactor, programDefault_cached_uniform_albedoFactor;
static GLint programDefault_cached_uniform_light_mult, programDefault_cached_uniform_displacement_factor, programDefault_cached_uniform_iResolution;

static GLint programDefaultLite_attribute_coord3d, programDefaultLite_attribute_v_color;
static GLint programDefaultLite_attribute_v_light, programDefaultLite_attribute_v_uv;
static GLint programDefaultLite_uniform_mvp, programDefaultLite_uniform_tex, programDefaultLite_uniform_texEmiss;
static GLint programDefaultLite_uniform_worldPalette, programDefaultLite_uniform_worldPaletteLights;
static GLint programDefaultLite_uniform_displacement_map, programDefaultLite_uniform_tex_mode, programDefaultLite_uniform_blend_mode;
static GLint programDefaultLite_uniform_tint, programDefaultLite_uniform_filter, programDefaultLite_uniform_fade, programDefaultLite_uniform_add;
static GLint programDefaultLite_uniform_emissiveFactor, programDefaultLite_uniform_albedoFactor;
static GLint programDefaultLite_uniform_light_mult, programDefaultLite_uniform_displacement_factor, programDefaultLite_uniform_iResolution;

std3DSimpleTexStage std3D_uiProgram;
std3DSimpleTexStage std3D_texFboStage;
std3DSimpleTexStage std3D_texFboSceneStage;
std3DSimpleTexStage std3D_blurStage;
std3DSimpleTexStage std3D_ssaoStage;
std3DSimpleTexStage std3D_ssaoMixStage;

GLuint blank_tex, blank_tex_white;
void* blank_data = NULL, *blank_data_white = NULL;
GLuint worldpal_texture;
void* worldpal_data = NULL;
GLuint worldpal_lights_texture;
void* worldpal_lights_data = NULL;
GLuint displaypal_texture;
void* displaypal_data = NULL;
GLuint tiledrand_texture;
rdVector3* tiledrand_data = NULL;

size_t std3D_loadedUITexturesAmt = 0;
stdBitmap* std3D_aUIBitmaps[STD3D_MAX_TEXTURES] = {0};
GLuint std3D_aUITextures[STD3D_MAX_TEXTURES] = {0};
static rdUITri GL_tmpUITris[STD3D_MAX_UI_TRIS] = {0};
static size_t GL_tmpUITrisAmt = 0;
GLuint last_ui_tex = 0;
int last_ui_flags = 0;
static D3DVERTEX GL_tmpUIVertices[STD3D_MAX_UI_VERTICES] = {0};
static size_t GL_tmpUIVerticesAmt = 0;

rdDDrawSurface* std3D_aLoadedSurfaces[STD3D_MAX_TEXTURES] = {0};
GLuint std3D_aLoadedTextures[STD3D_MAX_TEXTURES] = {0};
size_t std3D_loadedTexturesAmt = 0;
static rdTri GL_tmpTris[STD3D_MAX_TRIS] = {0};
static size_t GL_tmpTrisAmt = 0;
static rdLine GL_tmpLines[STD3D_MAX_VERTICES] = {0};
static size_t GL_tmpLinesAmt = 0;
static D3DVERTEX GL_tmpVertices[STD3D_MAX_VERTICES] = {0};
static size_t GL_tmpVerticesAmt = 0;
static size_t rendered_tris = 0;

static void* loaded_colormap = NULL;

rdDDrawSurface* last_tex = NULL;
int last_flags = 0;

D3DVERTEX* world_data_all = NULL;
GLushort* world_data_elements = NULL;
GLuint world_vbo_all;
GLuint world_ibo_triangle;

D3DVERTEX* menu_data_all = NULL;
GLushort* menu_data_elements = NULL;
GLuint menu_vbo_all;
GLuint menu_ibo_triangle;

extern int jkGuiBuildMulti_bRendering;

int std3D_bInitted = 0;
rdColormap std3D_ui_colormap;
int std3D_bReinitHudElements = 0;

void std3D_generateIntermediateFbo(int32_t width, int32_t height, std3DIntermediateFbo* pFbo, int isFloat)
{
    // Generate the framebuffer
    memset(pFbo, 0, sizeof(*pFbo));

    pFbo->w = width;
    pFbo->h = height;
    pFbo->iw = width;
    pFbo->ih = height;

    glActiveTexture(GL_TEXTURE0);

    glGenFramebuffers(1, &pFbo->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, pFbo->fbo);
    
    // Set up our framebuffer texture
    glGenTextures(1, &pFbo->tex);
    glBindTexture(GL_TEXTURE_2D, pFbo->tex);
#if defined(TARGET_LINUX_GLES)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
#else
    glTexImage2D(GL_TEXTURE_2D, 0, isFloat ? GL_RGBA16F : GL_RGBA8, width, height, 0, GL_RGBA, isFloat ? GL_FLOAT : GL_UNSIGNED_BYTE, NULL);
#endif
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
#if !defined(TARGET_LINUX_GLES)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
    glGenerateMipmap(GL_TEXTURE_2D);
#else
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
#endif
    
    // Attach fbTex to our currently bound framebuffer fb
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pFbo->tex, 0);

    // Set up our render buffer
    glGenRenderbuffers(1, &pFbo->rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, pFbo->rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    
    // Bind it to our framebuffer fb
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, pFbo->rbo);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        stdPlatform_Printf("std3D: ERROR, Framebuffer is incomplete!\n");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void std3D_deleteIntermediateFbo(std3DIntermediateFbo* pFbo)
{
    glDeleteFramebuffers(1, &pFbo->fbo);
    glDeleteTextures(1, &pFbo->tex);
    glDeleteRenderbuffers(1, &pFbo->rbo);
}

void std3D_generateFramebuffer(int32_t width, int32_t height, std3DFramebuffer* pFb)
{
    // Generate the framebuffer
    memset(pFb, 0, sizeof(*pFb));

    pFb->w = width;
    pFb->h = height;

    glActiveTexture(GL_TEXTURE0);

    glGenFramebuffers(1, &pFb->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, pFb->fbo);
    
    // Set up our framebuffer texture
    glGenTextures(1, &pFb->tex0);
    glBindTexture(GL_TEXTURE_2D, pFb->tex0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Attach fbTex to our currently bound framebuffer fb
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pFb->tex0, 0);

    // Set up our emissive fb texture
    glGenTextures(1, &pFb->tex1);
    glBindTexture(GL_TEXTURE_2D, pFb->tex1);
#if defined(TARGET_LINUX_GLES)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
#else
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
#endif
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
#if !defined(TARGET_LINUX_GLES)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
    glGenerateMipmap(GL_TEXTURE_2D);
#else
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
#endif
    
    // Attach fbTex to our currently bound framebuffer fb
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, pFb->tex1, 0);

    pFb->tex2 = 0;
    pFb->tex3 = 0;
    if (jkPlayer_enableSSAO) {
        // Set up our position fb texture
        glGenTextures(1, &pFb->tex2);
        glBindTexture(GL_TEXTURE_2D, pFb->tex2);
#if defined(TARGET_LINUX_GLES)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
#else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
#endif
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, pFb->tex2, 0);

        // Set up our normal fb texture
        glGenTextures(1, &pFb->tex3);
        glBindTexture(GL_TEXTURE_2D, pFb->tex3);
#if defined(TARGET_LINUX_GLES)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
#else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
#endif
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, pFb->tex3, 0);

        pFb->enable_extra |= STD3D_FB_EXTRA_POSNORM_GBUFFER;
    }

    // Set up our render buffer
    glGenRenderbuffers(1, &pFb->rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, pFb->rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    
    // Bind it to our framebuffer fb
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, pFb->rbo);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        stdPlatform_Printf("std3D: ERROR, Framebuffer is incomplete!\n");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (jkPlayer_enableSSAO)
    {
        std3D_generateIntermediateFbo(width, height, &pFb->ssaoBlur1, 0);
        std3D_generateIntermediateFbo(pFb->ssaoBlur1.w/2, pFb->ssaoBlur1.h/2, &pFb->ssaoBlur2, 0);
        pFb->enable_extra |= STD3D_FB_EXTRA_SSAO_BLUR;
    }

    if (jkPlayer_enableBloom)
    {
        pFb->enable_extra |= STD3D_FB_EXTRA_BLOOM;
        std3D_generateIntermediateFbo(width, height, &pFb->blur1, 1);
        //std3D_generateIntermediateFbo(width, height, &pFb->blurBlend, 1);
        std3D_generateIntermediateFbo(pFb->blur1.w/4, pFb->blur1.h/4, &pFb->blur2, 1);
        std3D_generateIntermediateFbo(pFb->blur2.w/4, pFb->blur2.h/4, &pFb->blur3, 1);
        std3D_generateIntermediateFbo(pFb->blur3.w/4, pFb->blur3.h/4, &pFb->blur4, 1);

        /*pFb->blur1.iw = width;
        pFb->blur1.ih = height;
        pFb->blur2.iw = width;
        pFb->blur2.ih = height;
        pFb->blur3.iw = width;
        pFb->blur3.ih = height;
        pFb->blur4.iw = width;
        pFb->blur4.ih = height;*/
    }

    pFb->main.fbo = pFb->fbo;
    pFb->main.tex = pFb->tex1;
    pFb->main.rbo = pFb->rbo;
    pFb->main.w = pFb->w;
    pFb->main.h = pFb->h;
    pFb->main.iw = pFb->w;
    pFb->main.ih = pFb->h;

    pFb->window.fbo = std3D_windowFbo;
    pFb->window.w = Window_xSize;
    pFb->window.h = Window_ySize;
    pFb->window.iw = Window_xSize;
    pFb->window.ih = Window_ySize;
}

void std3D_deleteFramebuffer(std3DFramebuffer* pFb)
{
    glDeleteFramebuffers(1, &pFb->fbo);
    glDeleteTextures(1, &pFb->tex0);
    glDeleteTextures(1, &pFb->tex1);
    if (pFb->tex2)
        glDeleteTextures(1, &pFb->tex2);
    if (pFb->tex3)
        glDeleteTextures(1, &pFb->tex3);
    glDeleteRenderbuffers(1, &pFb->rbo);

    std3D_deleteIntermediateFbo(&pFb->blur1);
    std3D_deleteIntermediateFbo(&pFb->blur2);
    std3D_deleteIntermediateFbo(&pFb->blur3);
    std3D_deleteIntermediateFbo(&pFb->blur4);
    //std3D_deleteIntermediateFbo(&pFb->blurBlend);

    std3D_deleteIntermediateFbo(&pFb->ssaoBlur1);
    std3D_deleteIntermediateFbo(&pFb->ssaoBlur2);
    //std3D_deleteIntermediateFbo(&pFb->ssaoBlur3);
}

void std3D_swapFramebuffers()
{
    if (std3D_activeFb == 2)
    {
        std3D_activeFb = 1;
        std3D_pFb = &std3D_framebuffers[0];
    }
    else
    {
        std3D_activeFb = 2;
        std3D_pFb = &std3D_framebuffers[1];
    }
}

GLuint std3D_loadProgram(const char* fpath_base)
{
    GLuint out;
    GLint link_ok = GL_FALSE;
    
    char* tmp_vert = (char*)malloc(strlen(fpath_base) + 32);
    char* tmp_frag = (char*)malloc(strlen(fpath_base) + 32);
    
    strcpy(tmp_vert, fpath_base);
    strcat(tmp_vert, "_v.glsl");
    
    strcpy(tmp_frag, fpath_base);
    strcat(tmp_frag, "_f.glsl");
    
    GLuint vs, fs;
    if ((vs = load_shader_file(tmp_vert, GL_VERTEX_SHADER))   == 0) return 0;
    if ((fs = load_shader_file(tmp_frag, GL_FRAGMENT_SHADER)) == 0) return 0;
    
    free(tmp_vert);
    free(tmp_frag);
    
    out = glCreateProgram();
    glAttachShader(out, vs);
    glAttachShader(out, fs);
    glLinkProgram(out);
    glGetProgramiv(out, GL_LINK_STATUS, &link_ok);
    if (!link_ok) 
    {
        print_log(out);
        return 0;
    }
    
    return out;
}

GLint std3D_tryFindAttribute(GLuint program, const char* attribute_name)
{
    GLint out = glGetAttribLocation(program, attribute_name);
    if (out == -1) {
        stdPlatform_Printf("std3D: Could not bind attribute %s!\n", attribute_name);
    }
    return out;
}

GLint std3D_tryFindUniform(GLuint program, const char* uniform_name)
{
    GLint out = glGetUniformLocation(program, uniform_name);
    if (out == -1) {
        stdPlatform_Printf("std3D: Could not bind uniform %s!\n", uniform_name);
    }
    return out;
}

static void std3D_bindDefaultProgram(int use_full_mrt)
{
    if (use_full_mrt) {
        glUseProgram(programDefault);
        attribute_coord3d = programDefault_cached_attribute_coord3d;
        attribute_v_color = programDefault_cached_attribute_v_color;
        attribute_v_light = programDefault_cached_attribute_v_light;
        attribute_v_uv = programDefault_cached_attribute_v_uv;
        uniform_mvp = programDefault_cached_uniform_mvp;
        uniform_tex = programDefault_cached_uniform_tex;
        uniform_texEmiss = programDefault_cached_uniform_texEmiss;
        uniform_worldPalette = programDefault_cached_uniform_worldPalette;
        uniform_worldPaletteLights = programDefault_cached_uniform_worldPaletteLights;
        uniform_displacement_map = programDefault_cached_uniform_displacement_map;
        uniform_tex_mode = programDefault_cached_uniform_tex_mode;
        uniform_blend_mode = programDefault_cached_uniform_blend_mode;
        uniform_tint = programDefault_cached_uniform_tint;
        uniform_filter = programDefault_cached_uniform_filter;
        uniform_fade = programDefault_cached_uniform_fade;
        uniform_add = programDefault_cached_uniform_add;
        uniform_emissiveFactor = programDefault_cached_uniform_emissiveFactor;
        uniform_albedoFactor = programDefault_cached_uniform_albedoFactor;
        uniform_light_mult = programDefault_cached_uniform_light_mult;
        uniform_displacement_factor = programDefault_cached_uniform_displacement_factor;
        uniform_iResolution = programDefault_cached_uniform_iResolution;
        return;
    }

    glUseProgram(programDefaultLite);
    attribute_coord3d = programDefaultLite_attribute_coord3d;
    attribute_v_color = programDefaultLite_attribute_v_color;
    attribute_v_light = programDefaultLite_attribute_v_light;
    attribute_v_uv = programDefaultLite_attribute_v_uv;
    uniform_mvp = programDefaultLite_uniform_mvp;
    uniform_tex = programDefaultLite_uniform_tex;
    uniform_texEmiss = programDefaultLite_uniform_texEmiss;
    uniform_worldPalette = programDefaultLite_uniform_worldPalette;
    uniform_worldPaletteLights = programDefaultLite_uniform_worldPaletteLights;
    uniform_displacement_map = programDefaultLite_uniform_displacement_map;
    uniform_tex_mode = programDefaultLite_uniform_tex_mode;
    uniform_blend_mode = programDefaultLite_uniform_blend_mode;
    uniform_tint = programDefaultLite_uniform_tint;
    uniform_filter = programDefaultLite_uniform_filter;
    uniform_fade = programDefaultLite_uniform_fade;
    uniform_add = programDefaultLite_uniform_add;
    uniform_emissiveFactor = programDefaultLite_uniform_emissiveFactor;
    uniform_albedoFactor = programDefaultLite_uniform_albedoFactor;
    uniform_light_mult = programDefaultLite_uniform_light_mult;
    uniform_displacement_factor = programDefaultLite_uniform_displacement_factor;
    uniform_iResolution = programDefaultLite_uniform_iResolution;
}

bool std3D_loadSimpleTexProgram(const char* fpath_base, std3DSimpleTexStage* pOut)
{
    if (!pOut) return false;
    if ((pOut->program = std3D_loadProgram(fpath_base)) == 0) return false;
    
    pOut->attribute_coord3d = std3D_tryFindAttribute(pOut->program, "coord3d");
    pOut->attribute_v_color = std3D_tryFindAttribute(pOut->program, "v_color");
    pOut->attribute_v_uv = std3D_tryFindAttribute(pOut->program, "v_uv");
    pOut->uniform_mvp = std3D_tryFindUniform(pOut->program, "mvp");
    pOut->uniform_iResolution = std3D_tryFindUniform(pOut->program, "iResolution");
    pOut->uniform_tex = std3D_tryFindUniform(pOut->program, "tex");
    pOut->uniform_tex2 = std3D_tryFindUniform(pOut->program, "tex2");
    pOut->uniform_tex3 = std3D_tryFindUniform(pOut->program, "tex3");

    pOut->uniform_param1 = std3D_tryFindUniform(pOut->program, "param1");
    pOut->uniform_param2 = std3D_tryFindUniform(pOut->program, "param2");
    pOut->uniform_param3 = std3D_tryFindUniform(pOut->program, "param3");

    return true;
}

int init_resources()
{
    stdPlatform_Printf("std3D: OpenGL init...\n");
    openjkdf2_trace("init_resources: enter");

#if defined(TARGET_LINUX_GLES)
    if (!std3D_EnsureGLContext()) {
        openjkdf2_trace("init_resources: no GL context");
        return false;
    }
    openjkdf2_trace("init_resources: GL context current");
    {
        const GLubyte *ver = glGetString(GL_VERSION);
        const GLubyte *renderer = glGetString(GL_RENDERER);
        openjkdf2_trace_fmt("init_resources: GL_VERSION=%s", ver ? (const char*)ver : "null");
        openjkdf2_trace_fmt("init_resources: GL_RENDERER=%s", renderer ? (const char*)renderer : "null");
    }
#endif

    std3D_bReinitHudElements = 1;

    memset(std3D_aUITextures, 0, sizeof(std3D_aUITextures));
    openjkdf2_trace("init_resources: after memset");

#if defined(TARGET_LINUX_GLES)
    // KMSDRM/Mali: default framebuffer is 0; glGetIntegerv can crash on some drivers
    std3D_windowFbo = 0;
    openjkdf2_trace("init_resources: window fbo=0 (GLES)");
#else
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &std3D_windowFbo);
    openjkdf2_trace("init_resources: got window fbo");
#endif

    int32_t tex_w = Window_xSize;
    int32_t tex_h = Window_ySize;

    std3D_generateFramebuffer(tex_w, tex_h, &std3D_framebuffers[0]);
    openjkdf2_trace("init_resources: framebuffer 0 ok");
    std3D_generateFramebuffer(tex_w, tex_h, &std3D_framebuffers[1]);
    openjkdf2_trace("init_resources: framebuffers ok");

    std3D_activeFb = 1;
    std3D_pFb = &std3D_framebuffers[0];
    
    if ((programDefault = std3D_loadProgram("shaders/default")) == 0) return false;
    openjkdf2_trace("init_resources: shader default ok");
    if ((programDefaultLite = std3D_loadProgram("shaders/default_lite")) == 0) return false;
    openjkdf2_trace("init_resources: shader default_lite ok");
    if ((programMenu = std3D_loadProgram("shaders/menu")) == 0) return false;
    openjkdf2_trace("init_resources: shader menu ok");
    if (!std3D_loadSimpleTexProgram("shaders/ui", &std3D_uiProgram)) return false;
    if (!std3D_loadSimpleTexProgram("shaders/texfbo", &std3D_texFboStage)) return false;
    if (!std3D_loadSimpleTexProgram("shaders/texfbo_scene", &std3D_texFboSceneStage)) return false;
    if (!std3D_loadSimpleTexProgram("shaders/blur", &std3D_blurStage)) return false;
    if (!std3D_loadSimpleTexProgram("shaders/ssao", &std3D_ssaoStage)) return false;
    if (!std3D_loadSimpleTexProgram("shaders/ssao_mix", &std3D_ssaoMixStage)) return false;

    // Attributes/uniforms
    attribute_coord3d = std3D_tryFindAttribute(programDefault, "coord3d");
    attribute_v_color = std3D_tryFindAttribute(programDefault, "v_color");
    attribute_v_light = std3D_tryFindAttribute(programDefault, "v_light");
    attribute_v_uv = std3D_tryFindAttribute(programDefault, "v_uv");
    uniform_mvp = std3D_tryFindUniform(programDefault, "mvp");
    uniform_tex = std3D_tryFindUniform(programDefault, "tex");
    uniform_texEmiss = std3D_tryFindUniform(programDefault, "texEmiss");
    uniform_worldPalette = std3D_tryFindUniform(programDefault, "worldPalette");
    uniform_worldPaletteLights = std3D_tryFindUniform(programDefault, "worldPaletteLights");
    uniform_displacement_map = std3D_tryFindUniform(programDefault, "displacement_map");
    uniform_tex_mode = std3D_tryFindUniform(programDefault, "tex_mode");
    uniform_blend_mode = std3D_tryFindUniform(programDefault, "blend_mode");
    uniform_tint = std3D_tryFindUniform(programDefault, "colorEffects_tint");
    uniform_filter = std3D_tryFindUniform(programDefault, "colorEffects_filter");
    uniform_fade = std3D_tryFindUniform(programDefault, "colorEffects_fade");
    uniform_add = std3D_tryFindUniform(programDefault, "colorEffects_add");
    uniform_emissiveFactor = std3D_tryFindUniform(programDefault, "emissiveFactor");
    uniform_albedoFactor = std3D_tryFindUniform(programDefault, "albedoFactor");
    uniform_light_mult = std3D_tryFindUniform(programDefault, "light_mult");
    uniform_displacement_factor = std3D_tryFindUniform(programDefault, "displacement_factor");
    uniform_iResolution = std3D_tryFindUniform(programDefault, "iResolution");

    programDefault_cached_attribute_coord3d = attribute_coord3d;
    programDefault_cached_attribute_v_color = attribute_v_color;
    programDefault_cached_attribute_v_light = attribute_v_light;
    programDefault_cached_attribute_v_uv = attribute_v_uv;
    programDefault_cached_uniform_mvp = uniform_mvp;
    programDefault_cached_uniform_tex = uniform_tex;
    programDefault_cached_uniform_texEmiss = uniform_texEmiss;
    programDefault_cached_uniform_worldPalette = uniform_worldPalette;
    programDefault_cached_uniform_worldPaletteLights = uniform_worldPaletteLights;
    programDefault_cached_uniform_displacement_map = uniform_displacement_map;
    programDefault_cached_uniform_tex_mode = uniform_tex_mode;
    programDefault_cached_uniform_blend_mode = uniform_blend_mode;
    programDefault_cached_uniform_tint = uniform_tint;
    programDefault_cached_uniform_filter = uniform_filter;
    programDefault_cached_uniform_fade = uniform_fade;
    programDefault_cached_uniform_add = uniform_add;
    programDefault_cached_uniform_emissiveFactor = uniform_emissiveFactor;
    programDefault_cached_uniform_albedoFactor = uniform_albedoFactor;
    programDefault_cached_uniform_light_mult = uniform_light_mult;
    programDefault_cached_uniform_displacement_factor = uniform_displacement_factor;
    programDefault_cached_uniform_iResolution = uniform_iResolution;

    programDefaultLite_attribute_coord3d = std3D_tryFindAttribute(programDefaultLite, "coord3d");
    programDefaultLite_attribute_v_color = std3D_tryFindAttribute(programDefaultLite, "v_color");
    programDefaultLite_attribute_v_light = std3D_tryFindAttribute(programDefaultLite, "v_light");
    programDefaultLite_attribute_v_uv = std3D_tryFindAttribute(programDefaultLite, "v_uv");
    programDefaultLite_uniform_mvp = std3D_tryFindUniform(programDefaultLite, "mvp");
    programDefaultLite_uniform_tex = std3D_tryFindUniform(programDefaultLite, "tex");
    programDefaultLite_uniform_texEmiss = std3D_tryFindUniform(programDefaultLite, "texEmiss");
    programDefaultLite_uniform_worldPalette = std3D_tryFindUniform(programDefaultLite, "worldPalette");
    programDefaultLite_uniform_worldPaletteLights = std3D_tryFindUniform(programDefaultLite, "worldPaletteLights");
    programDefaultLite_uniform_displacement_map = std3D_tryFindUniform(programDefaultLite, "displacement_map");
    programDefaultLite_uniform_tex_mode = std3D_tryFindUniform(programDefaultLite, "tex_mode");
    programDefaultLite_uniform_blend_mode = std3D_tryFindUniform(programDefaultLite, "blend_mode");
    programDefaultLite_uniform_tint = std3D_tryFindUniform(programDefaultLite, "colorEffects_tint");
    programDefaultLite_uniform_filter = std3D_tryFindUniform(programDefaultLite, "colorEffects_filter");
    programDefaultLite_uniform_fade = std3D_tryFindUniform(programDefaultLite, "colorEffects_fade");
    programDefaultLite_uniform_add = std3D_tryFindUniform(programDefaultLite, "colorEffects_add");
    programDefaultLite_uniform_emissiveFactor = std3D_tryFindUniform(programDefaultLite, "emissiveFactor");
    programDefaultLite_uniform_albedoFactor = std3D_tryFindUniform(programDefaultLite, "albedoFactor");
    programDefaultLite_uniform_light_mult = std3D_tryFindUniform(programDefaultLite, "light_mult");
    programDefaultLite_uniform_displacement_factor = std3D_tryFindUniform(programDefaultLite, "displacement_factor");
    programDefaultLite_uniform_iResolution = std3D_tryFindUniform(programDefaultLite, "iResolution");
    
    programMenu_attribute_coord3d = std3D_tryFindAttribute(programMenu, "coord3d");
    programMenu_attribute_v_color = std3D_tryFindAttribute(programMenu, "v_color");
    programMenu_attribute_v_uv = std3D_tryFindAttribute(programMenu, "v_uv");
    programMenu_uniform_mvp = std3D_tryFindUniform(programMenu, "mvp");
    programMenu_uniform_tex = std3D_tryFindUniform(programMenu, "tex");
    programMenu_uniform_displayPalette = std3D_tryFindUniform(programMenu, "displayPalette");
    programMenu_uniform_worldPalette = std3D_tryFindUniform(programMenu, "worldPalette");
    programMenu_uniform_menuIndexed = std3D_tryFindUniform(programMenu, "u_menuIndexed");
    
    // Blank texture
    glGenTextures(1, &blank_tex);
    blank_data = jkgm_alloc_aligned(0x400);
    memset(blank_data, 0x0, 0x400);
    
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 16, 16, 0, GL_RGB, GL_UNSIGNED_BYTE, blank_data);

    // Blank texture
    glGenTextures(1, &blank_tex_white);
    blank_data_white = jkgm_alloc_aligned(0x400);
    memset(blank_data_white, 0xFF, 0x400);
    
    glBindTexture(GL_TEXTURE_2D, blank_tex_white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 16, 16, 0, GL_RGB, GL_UNSIGNED_BYTE, blank_data_white);

    // World palette
    glGenTextures(1, &worldpal_texture);
    worldpal_data = jkgm_alloc_aligned(0x300);
    memset(worldpal_data, 0xFF, 0x300);
    
    glBindTexture(GL_TEXTURE_2D, worldpal_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    //glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    //glPixelStorei(GL_PACK_ALIGNMENT, 1);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 256, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, worldpal_data);

    // World palette lights
    glGenTextures(1, &worldpal_lights_texture);
    worldpal_lights_data = jkgm_alloc_aligned(0x4000);
    memset(worldpal_lights_data, 0xFF, 0x4000);
    
    glBindTexture(GL_TEXTURE_2D, worldpal_lights_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    //glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    //glPixelStorei(GL_PACK_ALIGNMENT, 1);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 256, 0x40, 0, GL_RED, GL_UNSIGNED_BYTE, worldpal_lights_data);
    
    
    // Display palette
    glGenTextures(1, &displaypal_texture);
    displaypal_data = jkgm_alloc_aligned(0x400);
    memset(displaypal_data, 0xFF, 0x300);
    
    glBindTexture(GL_TEXTURE_2D, displaypal_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    //glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    //glPixelStorei(GL_PACK_ALIGNMENT, 1);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 256, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, displaypal_data);

    // Tiled random
    // FLEXTODO
    glGenTextures(1, &tiledrand_texture);
    if (tiledrand_data) {
        free(tiledrand_data);
    }
    tiledrand_data = (rdVector3*)malloc(3 * 4 * 4 * sizeof(float));
    memset(tiledrand_data, 0, 3 * 4 * 4 * sizeof(float));

    for (int i = 0; i < 4*4; i++)
    {
        tiledrand_data[i].x = (_frand() * 2.0) - 1.0;
        tiledrand_data[i].y = (_frand() * 2.0) - 1.0;
        tiledrand_data[i].z = 0.0;
        rdVector_Normalize3Acc(&tiledrand_data[i]);
    }

    glBindTexture(GL_TEXTURE_2D, tiledrand_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, 4, 4, 0, GL_RGB, GL_FLOAT, tiledrand_data);

    unsigned int vao;
    glGenVertexArrays( 1, &vao );
    glBindVertexArray( vao ); 

    world_data_all = (D3DVERTEX*)malloc(STD3D_MAX_VERTICES * sizeof(D3DVERTEX));
    world_data_elements = (GLushort*)malloc(sizeof(GLushort) * 3 * STD3D_MAX_TRIS);

    menu_data_all = (D3DVERTEX*)malloc(STD3D_MAX_UI_VERTICES * sizeof(D3DVERTEX));
    menu_data_elements = (GLushort*)malloc(sizeof(GLushort) * 3 * STD3D_MAX_UI_TRIS);

    glGenBuffers(1, &world_vbo_all);
    glGenBuffers(1, &world_ibo_triangle);

    glGenBuffers(1, &menu_vbo_all);
    glGenBuffers(1, &menu_ibo_triangle);

    std3D_worldVboCap = 0;
    std3D_worldIboCap = 0;
    std3D_menuVboCap = 0;
    std3D_menuIboCap = 0;
    std3D_uploadBuffer(world_vbo_all, GL_ARRAY_BUFFER, STD3D_MAX_VERTICES * sizeof(D3DVERTEX), NULL, &std3D_worldVboCap);
    std3D_uploadBuffer(world_ibo_triangle, GL_ELEMENT_ARRAY_BUFFER, sizeof(GLushort) * 3 * STD3D_MAX_TRIS, NULL, &std3D_worldIboCap);
    std3D_uploadBuffer(menu_vbo_all, GL_ARRAY_BUFFER, STD3D_MAX_UI_VERTICES * sizeof(D3DVERTEX), NULL, &std3D_menuVboCap);
    std3D_uploadBuffer(menu_ibo_triangle, GL_ELEMENT_ARRAY_BUFFER, sizeof(GLushort) * 3 * STD3D_MAX_UI_TRIS, NULL, &std3D_menuIboCap);

    has_initted = true;
    openjkdf2_trace("init_resources: done");
    return true;
}

int std3D_Startup()
{
    if (std3D_bInitted) {
        return 1;
    }

#ifdef TARGET_CAN_JKGM
    jkgm_startup();
#endif

    memset(&std3D_ui_colormap, 0, sizeof(std3D_ui_colormap));
    rdColormap_LoadEntry("misc\\cmp\\UIColormap.cmp", &std3D_ui_colormap);

    std3D_bReinitHudElements = 1;

    std3D_bInitted = 1;
    return 1;
}

void std3D_Shutdown()
{
    if (!std3D_bInitted) {
        return;
    }

    std3D_bReinitHudElements = 0;

    rdColormap_FreeEntry(&std3D_ui_colormap);
    std3D_bInitted = 0;
}

void std3D_FreeResources()
{
    if (!has_initted) {
        return;
    }

#if defined(TARGET_LINUX_GLES)
    std3D_EnsureGLContext();
#endif

    std3D_PurgeEntireTextureCache();

    glDeleteProgram(programDefault);
    glDeleteProgram(programDefaultLite);
    glDeleteProgram(programMenu);
    std3D_deleteFramebuffer(&std3D_framebuffers[0]);
    std3D_deleteFramebuffer(&std3D_framebuffers[1]);
    glDeleteTextures(1, &blank_tex);
    glDeleteTextures(1, &blank_tex_white);
    glDeleteTextures(1, &worldpal_texture);
    glDeleteTextures(1, &worldpal_lights_texture);
    glDeleteTextures(1, &displaypal_texture);
    if (blank_data)
        jkgm_aligned_free(blank_data);
    if (blank_data_white)
        jkgm_aligned_free(blank_data_white);
    if (worldpal_data)
        jkgm_aligned_free(worldpal_data);
    if (worldpal_lights_data)
        jkgm_aligned_free(worldpal_lights_data);
    if (displaypal_data)
        jkgm_aligned_free(displaypal_data);
#if defined(TARGET_LINUX_GLES)
    free(std3D_cutsceneRgbaCache);
    std3D_cutsceneRgbaCache = NULL;
    std3D_cutsceneRgbaCacheBytes = 0;
#endif

    blank_data = NULL;
    blank_data_white = NULL;
    worldpal_data = NULL;
    worldpal_lights_data = NULL;
    displaypal_data = NULL;

    if (world_data_all)
        free(world_data_all);
    world_data_all = NULL;

    if (world_data_elements)
        free(world_data_elements);
    world_data_elements = NULL;

    if (menu_data_all)
        free(menu_data_all);
    menu_data_all = NULL;

    if (menu_data_elements)
        free(menu_data_elements);
    menu_data_elements = NULL;

    loaded_colormap = NULL;

    glDeleteBuffers(1, &world_vbo_all);
    glDeleteBuffers(1, &world_ibo_triangle);

    glDeleteBuffers(1, &menu_vbo_all);

    std3D_worldVboCap = 0;
    std3D_worldIboCap = 0;
    std3D_menuVboCap = 0;
    std3D_menuIboCap = 0;
#if defined(TARGET_LINUX_GLES)
    std3D_InvalidateGLContext();
#endif
    std3D_bReinitHudElements = 1;

    has_initted = false;
}

static void std3D_SyncDisplayPalette(int force)
{
    if (!displaypal_data)
        return;
    if (force || memcmp(displaypal_data, stdDisplay_masterPalette, 0x300)) {
        glBindTexture(GL_TEXTURE_2D, displaypal_texture);
        memcpy(displaypal_data, stdDisplay_masterPalette, 0x300);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGB, GL_UNSIGNED_BYTE, displaypal_data);
    }
}

#if defined(TARGET_LINUX_GLES)
static int std3D_glesUploadCutsceneMenuAsRgba(uint8_t *menuPixels, uint32_t width, uint32_t height, uint32_t rowStride)
{
    size_t rgbaBytes;
    uint32_t x;
    uint32_t y;
    uint8_t *dst;
    uint8_t idx;
    rdColor24 *pal;

    if (!menuPixels || !width || !height || rowStride < width)
        return 0;

    rgbaBytes = (size_t)width * (size_t)height * 4u;
    if (!std3D_cutsceneRgbaCache || std3D_cutsceneRgbaCacheBytes < rgbaBytes) {
        free(std3D_cutsceneRgbaCache);
        std3D_cutsceneRgbaCache = (uint8_t *)malloc(rgbaBytes);
        if (!std3D_cutsceneRgbaCache) {
            std3D_cutsceneRgbaCacheBytes = 0;
            return 0;
        }
        std3D_cutsceneRgbaCacheBytes = rgbaBytes;
    }

    pal = stdDisplay_masterPalette;
    dst = std3D_cutsceneRgbaCache;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            idx = menuPixels[(y * rowStride) + x];
            if (!idx) {
                dst[0] = 0;
                dst[1] = 0;
                dst[2] = 0;
                dst[3] = 255;
            } else {
                dst[0] = pal[idx].r;
                dst[1] = pal[idx].g;
                dst[2] = pal[idx].b;
                dst[3] = 255;
            }
            dst += 4;
        }
    }

    glBindTexture(GL_TEXTURE_2D, Video_menuTexId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, std3D_cutsceneRgbaCache);
    std3D_menuTexIsRgba = 1;
    return 1;
}

static void std3D_glesEnsureMenuTexIndexed(uint8_t *menuPixels, uint32_t width, uint32_t height, uint32_t rowStride)
{
    if (!Video_menuTexId) {
        stdDisplay_EnsureMenuGLTextures();
        if (!Video_menuTexId)
            return;
    }

    glBindTexture(GL_TEXTURE_2D, Video_menuTexId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (rowStride != width)
        glPixelStorei(GL_UNPACK_ROW_LENGTH, rowStride);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, menuPixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    std3D_menuTexIsRgba = 0;
}

void std3D_LeaveCutsceneMenuMode(void)
{
    uint8_t *menuPixels;
    uint32_t menuW;
    uint32_t menuH;
    uint32_t menuStride;

    free(std3D_cutsceneRgbaCache);
    std3D_cutsceneRgbaCache = NULL;
    std3D_cutsceneRgbaCacheBytes = 0;

    menuPixels = stdDisplay_VBufferPixels(&Video_menuBuffer);
    menuW = Video_menuBuffer.format.width;
    menuH = Video_menuBuffer.format.height;
    menuStride = Video_menuBuffer.format.width_in_bytes;
    if (menuPixels && menuW && menuH)
        std3D_glesEnsureMenuTexIndexed(menuPixels, menuW, menuH, menuStride);
    else
        std3D_menuTexIsRgba = 1;

    std3D_MarkMenuBufferDirty();
}
#endif

int std3D_StartScene()
{
    if (Main_bHeadless) return 1;

#if defined(TARGET_LINUX_GLES)
    std3D_EnsureGLContext();
#endif

    ++std3D_frameCount;

    //printf("Begin draw\n");
    if (!has_initted)
    {
        openjkdf2_trace("std3D_StartScene: init_resources");
        if (!init_resources()) {
            stdPlatform_Printf("std3D: Failed to init resources, exiting...");
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", "Failed to init resources, exiting...", NULL);
            exit(-1);
        }
    }
    
    rendered_tris = 0;
    
    std3D_swapFramebuffers();
    
    double supersample_level = jkPlayer_ssaaMultiple; // Can also be set lower
    int32_t tex_w = (int32_t)((double)Window_xSize * supersample_level);
    int32_t tex_h = (int32_t)((double)Window_ySize * supersample_level);

    {
        int want_posnorm = jkPlayer_enableSSAO ? STD3D_FB_EXTRA_POSNORM_GBUFFER : 0;
        int have_posnorm = std3D_pFb->enable_extra & STD3D_FB_EXTRA_POSNORM_GBUFFER;

        if (tex_w != std3D_pFb->w || tex_h != std3D_pFb->h
            || (!(std3D_pFb->enable_extra & STD3D_FB_EXTRA_BLOOM) && jkPlayer_enableBloom)
            || (!(std3D_pFb->enable_extra & STD3D_FB_EXTRA_SSAO_BLUR) && jkPlayer_enableSSAO)
            || have_posnorm != want_posnorm)
        {
            std3D_deleteFramebuffer(std3D_pFb);
            std3D_generateFramebuffer(tex_w, tex_h, std3D_pFb);
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->fbo);
    glEnable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glDepthFunc(GL_LESS);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    glCullFace(GL_FRONT);
    //glClampColor(GL_CLAMP_FRAGMENT_COLOR, GL_FALSE);

    // Technically this should be from Clear2
    glClearColor(0.0, 0.0, 0.0, 1.0);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    
    if (jkGuiBuildMulti_bRendering && rdColormap_pCurMap && loaded_colormap != rdColormap_pCurMap)
    {
        glBindTexture(GL_TEXTURE_2D, worldpal_texture);
        memcpy(worldpal_data, rdColormap_pCurMap->colors, 0x300);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGB, GL_UNSIGNED_BYTE, worldpal_data);
    
        if (rdColormap_pCurMap->lightlevel)
        {
            glBindTexture(GL_TEXTURE_2D, worldpal_lights_texture);
            memcpy(worldpal_lights_data, rdColormap_pCurMap->lightlevel, 0x4000);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 0x40, GL_RED, GL_UNSIGNED_BYTE, worldpal_lights_data);
        }

        loaded_colormap = rdColormap_pCurMap;
    }
    else if (sithWorld_pCurrentWorld && sithWorld_pCurrentWorld->colormaps && loaded_colormap != sithWorld_pCurrentWorld->colormaps)
    {
        glBindTexture(GL_TEXTURE_2D, worldpal_texture);
        memcpy(worldpal_data, sithWorld_pCurrentWorld->colormaps->colors, 0x300);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGB, GL_UNSIGNED_BYTE, worldpal_data);
    
        if (sithWorld_pCurrentWorld->colormaps->lightlevel)
        {
            glBindTexture(GL_TEXTURE_2D, worldpal_lights_texture);
            memcpy(worldpal_lights_data, sithWorld_pCurrentWorld->colormaps->lightlevel, 0x4000);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 0x40, GL_RED, GL_UNSIGNED_BYTE, worldpal_lights_data);
        }

        loaded_colormap = sithWorld_pCurrentWorld->colormaps;
    }

    std3D_SyncDisplayPalette(0);

#if 0
    // New random values
    glBindTexture(GL_TEXTURE_2D, tiledrand_texture);
    for (int i = 0; i < 4*4; i++)
    {
        tiledrand_data[i].x = (_frand() * 2.0) - 1.0;
        tiledrand_data[i].y = (_frand() * 2.0) - 1.0;
        tiledrand_data[i].z = 0.0;
        rdVector_Normalize3Acc(&tiledrand_data[i]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGB, GL_FLOAT, tiledrand_data);
    }
#endif

    // Describe our vertices array to OpenGL (it can't guess its format automatically)
    glBindBuffer(GL_ARRAY_BUFFER, world_vbo_all);
    std3D_uploadBuffer(world_vbo_all, GL_ARRAY_BUFFER, 1 * sizeof(D3DVERTEX), GL_tmpVertices, &std3D_worldVboCap);
    glVertexAttribPointer(
        attribute_coord3d, // attribute
        3,                 // number of elements per vertex, here (x,y,z)
        GL_FLOAT,          // the type of each element
        GL_FALSE,          // normalize fixed-point data?
        sizeof(D3DVERTEX),                 // data stride
        (GLvoid*)offsetof(D3DVERTEX, x)                  // offset of first element
    );
    
    glVertexAttribPointer(
        attribute_v_color, // attribute
        4,                 // number of elements per vertex, here (R,G,B,A)
        GL_UNSIGNED_BYTE,  // the type of each element
        GL_TRUE,          // normalize fixed-point data?
        sizeof(D3DVERTEX),                 // no extra data between each position
        (GLvoid*)offsetof(D3DVERTEX, color) // offset of first element
    );

    glVertexAttribPointer(
        attribute_v_light, // attribute
        1,                 // number of elements per vertex, here (L)
        GL_FLOAT,  // the type of each element
        GL_FALSE,          // normalize fixed-point data?
        sizeof(D3DVERTEX),                 // no extra data between each position
        (GLvoid*)offsetof(D3DVERTEX, lightLevel) // offset of first element
    );

    glVertexAttribPointer(
        attribute_v_uv,    // attribute
        2,                 // number of elements per vertex, here (U,V)
        GL_FLOAT,          // the type of each element
        GL_FALSE,          // take our values as-is
        sizeof(D3DVERTEX),                 // no extra data between each position
        (GLvoid*)offsetof(D3DVERTEX, tu)                  // offset of first element
    );

    glEnableVertexAttribArray(attribute_coord3d);
    glEnableVertexAttribArray(attribute_v_color);
    glEnableVertexAttribArray(attribute_v_light);
    glEnableVertexAttribArray(attribute_v_uv);
    
    return 1;
}

int std3D_EndScene()
{
    if (Main_bHeadless) {
        last_tex = NULL;
        last_flags = 0;
        std3D_ResetRenderList();
        return 1;
    }

    glDisableVertexAttribArray(attribute_v_uv);
    glDisableVertexAttribArray(attribute_v_color);
    glDisableVertexAttribArray(attribute_coord3d);

#if defined(TARGET_LINUX_GLES)
    std3D_RunDeferredTexturePurge();
#endif

    //printf("End draw\n");
    last_tex = NULL;
    last_flags = 0;
    std3D_ResetRenderList();
    //printf("%u tris\n", rendered_tris);
    return 1;
}

void std3D_ResetUIRenderList()
{
    rendered_tris += GL_tmpUITrisAmt;

    GL_tmpUIVerticesAmt = 0;
    GL_tmpUITrisAmt = 0;
    //GL_tmpLinesAmt = 0;
    
    //memset(GL_tmpTris, 0, sizeof(GL_tmpTris));
    //memset(GL_tmpVertices, 0, sizeof(GL_tmpVertices));
}

void std3D_ResetRenderList()
{
    rendered_tris += GL_tmpTrisAmt;

    GL_tmpVerticesAmt = 0;
    GL_tmpTrisAmt = 0;
    GL_tmpLinesAmt = 0;
    
    //memset(GL_tmpTris, 0, sizeof(GL_tmpTris));
    //memset(GL_tmpVertices, 0, sizeof(GL_tmpVertices));
}

int std3D_RenderListVerticesFinish()
{
    return 1;
}

int std3D_ShouldFitMenu4x3(double winW, double winH)
{
    const double ar43 = 640.0 / 480.0;
    double aspect = winW / winH;
    /* Menus/cutscenes: 4:3 on every non-4:3 panel (pillarbox on 3:2, letterbox on 1:1). */
    return aspect < ar43 - 0.001 || aspect > ar43 + 0.001;
}

void std3D_FitMenu4x3(double winW, double winH, double *outX, double *outY, double *outW, double *outH)
{
    const double ar43 = 640.0 / 480.0;

    if (winW / winH > ar43) {
        *outH = winH;
        *outW = winH * ar43;
        *outX = (winW - *outW) / 2.0;
        *outY = 0.0;
    } else {
        *outW = winW;
        *outH = winW / ar43;
        *outX = 0.0;
        *outY = (winH - *outH) / 2.0;
    }
}

void std3D_ComputeMenuRect(double winW, double winH, double *outX, double *outY, double *outW, double *outH)
{
    if (std3D_ShouldFitMenu4x3(winW, winH)) {
        std3D_FitMenu4x3(winW, winH, outX, outY, outW, outH);
    } else {
        *outX = 0.0;
        *outY = 0.0;
        *outW = winW;
        *outH = winH;
    }
}

void std3D_DrawMenuSubrect(flex_t x, flex_t y, flex_t w, flex_t h, flex_t dstX, flex_t dstY, flex_t scale)
{
    //double tex_w = (double)Window_xSize;
    //double tex_h = (double)Window_ySize;
    double tex_w = Video_menuBuffer.format.width;
    double tex_h = Video_menuBuffer.format.height;

    float w_dst = w;
    float h_dst = h;

    if (scale == 0.0)
    {
        w_dst = (w / tex_w) * (double)Window_xSize;
        h_dst = (h / tex_h) * (double)Window_ySize;

        dstX = (dstX / tex_w) * (double)Window_xSize;
        dstY = (dstY / tex_h) * (double)Window_ySize;

        scale = 1.0;
    }

    double u1 = (x / tex_w);
    double u2 = ((x+w) / tex_w);
    double v1 = (y / tex_h);
    double v2 = ((y+h) / tex_h);

    GL_tmpVertices[GL_tmpVerticesAmt+0].x = dstX;
    GL_tmpVertices[GL_tmpVerticesAmt+0].y = dstY;
    GL_tmpVertices[GL_tmpVerticesAmt+0].z = 0.0;
    GL_tmpVertices[GL_tmpVerticesAmt+0].tu = u1;
    GL_tmpVertices[GL_tmpVerticesAmt+0].tv = v1;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+0].nx = 0;
    GL_tmpVertices[GL_tmpVerticesAmt+0].color = 0xFFFFFFFF;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+0].nz = 0;
    
    GL_tmpVertices[GL_tmpVerticesAmt+1].x = dstX;
    GL_tmpVertices[GL_tmpVerticesAmt+1].y = dstY + (scale * h_dst);
    GL_tmpVertices[GL_tmpVerticesAmt+1].z = 0.0;
    GL_tmpVertices[GL_tmpVerticesAmt+1].tu = u1;
    GL_tmpVertices[GL_tmpVerticesAmt+1].tv = v2;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+1].nx = 0;
    GL_tmpVertices[GL_tmpVerticesAmt+1].color = 0xFFFFFFFF;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+1].nz = 0;
    
    GL_tmpVertices[GL_tmpVerticesAmt+2].x = dstX + (scale * w_dst);
    GL_tmpVertices[GL_tmpVerticesAmt+2].y = dstY + (scale * h_dst);
    GL_tmpVertices[GL_tmpVerticesAmt+2].z = 0.0;
    GL_tmpVertices[GL_tmpVerticesAmt+2].tu = u2;
    GL_tmpVertices[GL_tmpVerticesAmt+2].tv = v2;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+2].nx = 0;
    GL_tmpVertices[GL_tmpVerticesAmt+2].color = 0xFFFFFFFF;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+2].nz = 0;
    
    GL_tmpVertices[GL_tmpVerticesAmt+3].x = dstX + (scale * w_dst);
    GL_tmpVertices[GL_tmpVerticesAmt+3].y = dstY;
    GL_tmpVertices[GL_tmpVerticesAmt+3].z = 0.0;
    GL_tmpVertices[GL_tmpVerticesAmt+3].tu = u2;
    GL_tmpVertices[GL_tmpVerticesAmt+3].tv = v1;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+3].nx = 0;
    GL_tmpVertices[GL_tmpVerticesAmt+3].color = 0xFFFFFFFF;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+3].nz = 0;
    
    GL_tmpTris[GL_tmpTrisAmt+0].v1 = GL_tmpVerticesAmt+1;
    GL_tmpTris[GL_tmpTrisAmt+0].v2 = GL_tmpVerticesAmt+0;
    GL_tmpTris[GL_tmpTrisAmt+0].v3 = GL_tmpVerticesAmt+2;
    
    GL_tmpTris[GL_tmpTrisAmt+1].v1 = GL_tmpVerticesAmt+0;
    GL_tmpTris[GL_tmpTrisAmt+1].v2 = GL_tmpVerticesAmt+3;
    GL_tmpTris[GL_tmpTrisAmt+1].v3 = GL_tmpVerticesAmt+2;
    
    GL_tmpVerticesAmt += 4;
    GL_tmpTrisAmt += 2;
}

void std3D_DrawMenuSubrect2(flex_t x, flex_t y, flex_t w, flex_t h, flex_t dstX, flex_t dstY, flex_t scale)
{
    //double tex_w = (double)Window_xSize;
    //double tex_h = (double)Window_ySize;
    double tex_w = Video_menuBuffer.format.width;
    double tex_h = Video_menuBuffer.format.height;

    float w_dst = w;
    float h_dst = h;

    if (scale == 0.0)
    {
        w_dst = (w / tex_w) * (double)Window_xSize;
        h_dst = (h / tex_h) * (double)Window_ySize;

        dstX = (dstX / tex_w) * (double)Window_xSize;
        dstY = (dstY / tex_h) * (double)Window_ySize;

        scale = 1.0;
    }

    double u1 = (x / tex_w);
    double u2 = ((x+w) / tex_w);
    double v1 = (y / tex_h);
    double v2 = ((y+h) / tex_h);

    GL_tmpVertices[GL_tmpVerticesAmt+0].x = dstX;
    GL_tmpVertices[GL_tmpVerticesAmt+0].y = dstY;
    GL_tmpVertices[GL_tmpVerticesAmt+0].z = 0.0;
    GL_tmpVertices[GL_tmpVerticesAmt+0].tu = u1;
    GL_tmpVertices[GL_tmpVerticesAmt+0].tv = v1;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+0].nx = 0;
    GL_tmpVertices[GL_tmpVerticesAmt+0].color = 0x000000FF;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+0].nz = 0;
    
    GL_tmpVertices[GL_tmpVerticesAmt+1].x = dstX;
    GL_tmpVertices[GL_tmpVerticesAmt+1].y = dstY + (scale * h_dst);
    GL_tmpVertices[GL_tmpVerticesAmt+1].z = 0.0;
    GL_tmpVertices[GL_tmpVerticesAmt+1].tu = u1;
    GL_tmpVertices[GL_tmpVerticesAmt+1].tv = v2;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+1].nx = 0;
    GL_tmpVertices[GL_tmpVerticesAmt+1].color = 0x000000FF;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+1].nz = 0;
    
    GL_tmpVertices[GL_tmpVerticesAmt+2].x = dstX + (scale * w_dst);
    GL_tmpVertices[GL_tmpVerticesAmt+2].y = dstY + (scale * h_dst);
    GL_tmpVertices[GL_tmpVerticesAmt+2].z = 0.0;
    GL_tmpVertices[GL_tmpVerticesAmt+2].tu = u2;
    GL_tmpVertices[GL_tmpVerticesAmt+2].tv = v2;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+2].nx = 0;
    GL_tmpVertices[GL_tmpVerticesAmt+2].color = 0x000000FF;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+2].nz = 0;
    
    GL_tmpVertices[GL_tmpVerticesAmt+3].x = dstX + (scale * w_dst);
    GL_tmpVertices[GL_tmpVerticesAmt+3].y = dstY;
    GL_tmpVertices[GL_tmpVerticesAmt+3].z = 0.0;
    GL_tmpVertices[GL_tmpVerticesAmt+3].tu = u2;
    GL_tmpVertices[GL_tmpVerticesAmt+3].tv = v1;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+3].nx = 0;
    GL_tmpVertices[GL_tmpVerticesAmt+3].color = 0x000000FF;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+3].nz = 0;
    
    GL_tmpTris[GL_tmpTrisAmt+0].v1 = GL_tmpVerticesAmt+1;
    GL_tmpTris[GL_tmpTrisAmt+0].v2 = GL_tmpVerticesAmt+0;
    GL_tmpTris[GL_tmpTrisAmt+0].v3 = GL_tmpVerticesAmt+2;
    
    GL_tmpTris[GL_tmpTrisAmt+1].v1 = GL_tmpVerticesAmt+0;
    GL_tmpTris[GL_tmpTrisAmt+1].v2 = GL_tmpVerticesAmt+3;
    GL_tmpTris[GL_tmpTrisAmt+1].v3 = GL_tmpVerticesAmt+2;
    
    GL_tmpVerticesAmt += 4;
    GL_tmpTrisAmt += 2;
}

static rdDDrawSurface* test_idk = NULL;
void std3D_DrawSimpleTex(std3DSimpleTexStage* pStage, std3DIntermediateFbo* pFbo, GLuint texId, GLuint texId2, GLuint texId3, flex_t param1, flex_t param2, flex_t param3, int gen_mips);
void std3D_DrawMapOverlay();
void std3D_DrawUIRenderList();

void std3D_DrawMenu()
{
    if (Main_bHeadless) return;

#if defined(TARGET_LINUX_GLES)
    stdDisplay_EnsureMenuGLTextures();
#endif
    stdDisplay_SyncMenuBufferFormat();

    //printf("Draw menu\n");
    std3D_DrawSceneFbo();
    //glFlush();

    glBindFramebuffer(GL_FRAMEBUFFER, std3D_windowFbo);
    glDepthMask(GL_TRUE);
    glCullFace(GL_FRONT);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthFunc(GL_ALWAYS);
    glUseProgram(programMenu);
    
    float menu_w, menu_h, menu_u, menu_v, menu_x, menu_y;
    double menuRectX, menuRectY, menuRectW, menuRectH;
    menu_w = (double)Window_xSize;
    menu_h = (double)Window_ySize;
    menu_u = 1.0;
    menu_v = 1.0;
    menu_x = 0.0;
    menu_y = 0.0;
    
    int bFixHudScale = 0;

    double fake_windowW = (double)Window_xSize;
    double fake_windowH = (double)Window_ySize;

    if (!jkGame_isDDraw && !jkGuiBuildMulti_bRendering && !jkCutscene_isRendering)
    {
        menu_u = 1.0;
        menu_v = 1.0;

        std3D_ComputeMenuRect((double)Window_xSize, (double)Window_ySize, &menuRectX, &menuRectY, &menuRectW, &menuRectH);
        menu_x = (float)menuRectX;
        menu_y = (float)menuRectY;
        menu_w = (float)menuRectW;
        menu_h = (float)menuRectH;
    }
    else if (jkCutscene_isRendering) {
        bFixHudScale = 1;

        std3D_ComputeMenuRect((double)Window_xSize, (double)Window_ySize, &menuRectX, &menuRectY, &menuRectW, &menuRectH);
        menu_x = (float)menuRectX;
        menu_y = (float)menuRectY;
        menu_w = (float)menuRectW;
        menu_h = (float)menuRectH;

    }
    else if (jkGuiBuildMulti_bRendering)
    {
        bFixHudScale = 1;

        menu_u = 1.0;
        menu_v = 1.0;

        std3D_ComputeMenuRect((double)Window_xSize, (double)Window_ySize, &menuRectX, &menuRectY, &menuRectW, &menuRectH);
        menu_x = (float)menuRectX;
        menu_y = (float)menuRectY;
        menu_w = (float)menuRectW;
        menu_h = (float)menuRectH;
    }
    else
    {
        bFixHudScale = 0;

        if (openjkdf2_IsHandheld()) {
            /* Stretch the 640x480 game/HUD buffer to the (possibly downscaled) window. */
            menu_w = (double)Window_xSize;
            menu_h = (double)Window_ySize;
            menu_u = 1.0;
            menu_v = 1.0;
        } else {
            menu_w = Video_menuBuffer.format.width;
            menu_h = Video_menuBuffer.format.height;
        }
    }

    if (!bFixHudScale)
    {
        GL_tmpVertices[0].x = menu_x;
        GL_tmpVertices[0].y = menu_y;
        GL_tmpVertices[0].z = 0.0;
        GL_tmpVertices[0].tu = 0.0;
        GL_tmpVertices[0].tv = 0.0;
        *(uint32_t*)&GL_tmpVertices[0].nx = 0;
        GL_tmpVertices[0].color = 0xFFFFFFFF;
        *(uint32_t*)&GL_tmpVertices[0].nz = 0;
        
        GL_tmpVertices[1].x = menu_x;
        GL_tmpVertices[1].y = menu_y + menu_h;
        GL_tmpVertices[1].z = 0.0;
        GL_tmpVertices[1].tu = 0.0;
        GL_tmpVertices[1].tv = menu_v;
        *(uint32_t*)&GL_tmpVertices[1].nx = 0;
        GL_tmpVertices[1].color = 0xFFFFFFFF;
        *(uint32_t*)&GL_tmpVertices[1].nz = 0;
        
        GL_tmpVertices[2].x = menu_x + menu_w;
        GL_tmpVertices[2].y = menu_y + menu_h;
        GL_tmpVertices[2].z = 0.0;
        GL_tmpVertices[2].tu = menu_u;
        GL_tmpVertices[2].tv = menu_v;
        *(uint32_t*)&GL_tmpVertices[2].nx = 0;
        GL_tmpVertices[2].color = 0xFFFFFFFF;
        *(uint32_t*)&GL_tmpVertices[2].nz = 0;
        
        GL_tmpVertices[3].x = menu_x + menu_w;
        GL_tmpVertices[3].y = menu_y;
        GL_tmpVertices[3].z = 0.0;
        GL_tmpVertices[3].tu = menu_u;
        GL_tmpVertices[3].tv = 0.0;
        *(uint32_t*)&GL_tmpVertices[3].nx = 0;
        GL_tmpVertices[3].color = 0xFFFFFFFF;
        *(uint32_t*)&GL_tmpVertices[3].nz = 0;
        
        GL_tmpTris[0].v1 = 1;
        GL_tmpTris[0].v2 = 0;
        GL_tmpTris[0].v3 = 2;
        
        GL_tmpTris[1].v1 = 0;
        GL_tmpTris[1].v2 = 3;
        GL_tmpTris[1].v3 = 2;
        
        GL_tmpVerticesAmt = 4;
        GL_tmpTrisAmt = 2;
    }
    else if (jkGuiBuildMulti_bRendering)
    {
        GL_tmpVerticesAmt = 0;
        GL_tmpTrisAmt = 0;

        // Main View
        std3D_DrawMenuSubrect(0, 0, 640, 480, menu_x, menu_y, menu_w/640.0);
    }
    else if (jkCutscene_isRendering)
    {
        GL_tmpVerticesAmt = 0;
        GL_tmpTrisAmt = 0;

        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        /* Full 640x480 buffer (video centered in GLES jkCutscene) — same as menus. */
        std3D_DrawMenuSubrect(0, 0, 640, 480, menu_x, menu_y, menu_w / 640.0);
    }
    else
    {
        GL_tmpVerticesAmt = 0;
        GL_tmpTrisAmt = 0;

#if !defined(TARGET_LINUX_GLES)
        // Main View
        std3D_DrawMenuSubrect(0, 128, menu_w, menu_h-256, 0, 128, 0.0);

        float hudScale = Window_ySize / 480.0;

        // Left and Right HUD
        std3D_DrawMenuSubrect(0, menu_h - 64, 64, 64, 0, Window_ySize - 64*hudScale, hudScale);
        std3D_DrawMenuSubrect(menu_w - 64, menu_h - 64, 64, 64, Window_xSize - 64*hudScale, Window_ySize - 64*hudScale, hudScale);

        // Items (inventory is drawn via std3D_DrawUIBitmap on GLES)
        std3D_DrawMenuSubrect((menu_w / 2) - 128, menu_h - 64, 256, 64, (Window_xSize / 2) - (128*hudScale), Window_ySize - 64*hudScale, hudScale);

        // Text
        float textScale = hudScale;
        if (jkDev_BMFontHeight > 11) {
            textScale *= 11.0 / (float)jkDev_BMFontHeight;
        }
        float textWidth = menu_w - (48*2);
        float textHeight = jkDev_BMFontHeight * 5.5;
        float destTextWidth = textWidth * textScale;
        std3D_DrawMenuSubrect(48, 0, menu_w - (48*2), textHeight, (Window_xSize / 2) - (destTextWidth / 2), 0, textScale);

        // Active forcepowers/items
        std3D_DrawMenuSubrect(menu_w - 48, 0, 48, 128, Window_xSize - (48*hudScale), 0, hudScale);
#endif
    }

    glActiveTexture(GL_TEXTURE0 + 4);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    
    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, Video_menuTexId);
    {
        uint8_t *menuPixels = stdDisplay_VBufferPixels(&Video_menuBuffer);
        uint32_t menuW = Video_menuBuffer.format.width;
        uint32_t menuH = Video_menuBuffer.format.height;
        uint32_t menuStride = Video_menuBuffer.format.width_in_bytes;
        int menuIndexed = 1;

#if defined(TARGET_LINUX_GLES)
        std3D_SyncDisplayPalette(jkCutscene_isRendering);
        if (jkCutscene_isRendering && menuPixels && menuW && menuH) {
            if (std3D_glesUploadCutsceneMenuAsRgba(menuPixels, menuW, menuH, menuStride))
                menuIndexed = 0;
        } else if (std3D_menuTexIsRgba) {
            std3D_glesEnsureMenuTexIndexed(menuPixels, menuW, menuH, menuStride);
            menuIndexed = 1;
        } else if (menuIndexed && menuPixels) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, menuStride);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, menuW, menuH, GL_RED, GL_UNSIGNED_BYTE, menuPixels);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        }
#else
#if !defined(TARGET_LINUX_GLES)
        if (std3D_menuBufferDirty)
#endif
        if (menuIndexed && menuPixels) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, menuStride);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, menuW, menuH, GL_RED, GL_UNSIGNED_BYTE, menuPixels);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        }
#endif
        std3D_menuBufferDirty = 0;

        glUniform1i(programMenu_uniform_menuIndexed, menuIndexed);
    }

    //GLushort data_elements[32 * 3];
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, displaypal_texture);

    glActiveTexture(GL_TEXTURE0 + 0);
    glUniform1i(programMenu_uniform_tex, 0);
    glUniform1i(programMenu_uniform_displayPalette, 1);
    if (programMenu_uniform_worldPalette >= 0)
        glUniform1i(programMenu_uniform_worldPalette, 1);

    D3DVERTEX* vertexes = GL_tmpVertices;

    glBindBuffer(GL_ARRAY_BUFFER, menu_vbo_all);
    std3D_uploadBuffer(menu_vbo_all, GL_ARRAY_BUFFER, GL_tmpVerticesAmt * sizeof(D3DVERTEX), GL_tmpVertices, &std3D_menuVboCap);
    glVertexAttribPointer(
        programMenu_attribute_coord3d, // attribute
        3,                 // number of elements per vertex, here (x,y,z)
        GL_FLOAT,          // the type of each element
        GL_FALSE,          // normalize fixed-point data?
        sizeof(D3DVERTEX),                 // data stride
        (GLvoid*)offsetof(D3DVERTEX, x)                  // offset of first element
    );
    
    glVertexAttribPointer(
        programMenu_attribute_v_color, // attribute
        4,                 // number of elements per vertex, here (R,G,B,A)
        GL_UNSIGNED_BYTE,  // the type of each element
        GL_TRUE,          // normalize fixed-point data?
        sizeof(D3DVERTEX),                 // no extra data between each position
        (GLvoid*)offsetof(D3DVERTEX, color) // offset of first element
    );

    /*glVertexAttribPointer(
        std3D_texFboStage.attribute_v_light, // attribute
        1,                 // number of elements per vertex, here (L)
        GL_FLOAT,  // the type of each element
        GL_FALSE,          // normalize fixed-point data?
        sizeof(D3DVERTEX),                 // no extra data between each position
        (GLvoid*)offsetof(D3DVERTEX, lightLevel) // offset of first element
    );*/

    glVertexAttribPointer(
        programMenu_attribute_v_uv,    // attribute
        2,                 // number of elements per vertex, here (U,V)
        GL_FLOAT,          // the type of each element
        GL_FALSE,          // take our values as-is
        sizeof(D3DVERTEX),                 // no extra data between each position
        (GLvoid*)offsetof(D3DVERTEX, tu)                  // offset of first element
    );

    glEnableVertexAttribArray(programMenu_attribute_coord3d);
    glEnableVertexAttribArray(programMenu_attribute_v_color);
    glEnableVertexAttribArray(programMenu_attribute_v_uv);

    {

    float maxX, maxY, scaleX, scaleY, width, height;

    scaleX = 1.0/((double)Window_xSize / 2.0);
    scaleY = 1.0/((double)Window_ySize / 2.0);
    maxX = 1.0;
    maxY = 1.0;
    width = Window_xSize;
    height = Window_ySize;
    
    float d3dmat[16] = {
       maxX*scaleX,      0,                                          0,      0, // right
       0,                                       -maxY*scaleY,               0,      0, // up
       0,                                       0,                                          1,     0, // forward
       -(width/2)*scaleX,  (height/2)*scaleY,     -1,      1  // pos
    };
    
    glUniformMatrix4fv(programMenu_uniform_mvp, 1, GL_FALSE, d3dmat);
#if defined(TARGET_LINUX_GLES)
    Window_SetPresentViewport();
#else
    glViewport(0, 0, width, height);
#endif

    }
    
    rdTri* tris = GL_tmpTris;
    
    rdDDrawSurface* last_tex = (rdDDrawSurface*)(intptr_t)-1;
    int last_tex_idx = 0;
    //GLushort* data_elements = malloc(sizeof(GLushort) * 3 * GL_tmpTrisAmt);
    for (int j = 0; j < GL_tmpTrisAmt; j++)
    {
        menu_data_elements[(j*3)+0] = tris[j].v1;
        menu_data_elements[(j*3)+1] = tris[j].v2;
        menu_data_elements[(j*3)+2] = tris[j].v3;
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, menu_ibo_triangle);
    std3D_uploadBuffer(menu_ibo_triangle, GL_ELEMENT_ARRAY_BUFFER, GL_tmpTrisAmt * 3 * sizeof(GLushort), menu_data_elements, &std3D_menuIboCap);

    int tris_size = 0;  
    glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &tris_size);
    glDrawElements(GL_TRIANGLES, tris_size / sizeof(GLushort), GL_UNSIGNED_SHORT, 0);

    glDisableVertexAttribArray(programMenu_attribute_v_uv);
    glDisableVertexAttribArray(programMenu_attribute_v_color);
    glDisableVertexAttribArray(programMenu_attribute_coord3d);

    std3D_DrawMapOverlay();
    std3D_DrawUIRenderList();

    last_flags = 0;
}

void std3D_DrawMapOverlay()
{
    if (Main_bHeadless) return;

    //glFlush();

    glBindFramebuffer(GL_FRAMEBUFFER, std3D_windowFbo);
    glDepthMask(GL_TRUE);
    glCullFace(GL_FRONT);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthFunc(GL_ALWAYS);
    glUseProgram(programMenu);
    
    float menu_w = (double)Window_xSize;
    float menu_h = (double)Window_ySize;

    if (!jkGame_isDDraw)
    {
        return;
    }

    menu_w = Video_overlayMapBuffer.format.width;
    menu_h = Video_overlayMapBuffer.format.height;

    GL_tmpVerticesAmt = 0;
    GL_tmpTrisAmt = 0;

    // Main View
    std3D_DrawMenuSubrect(0, 0, menu_w, menu_h, 0, 0, 0.0);

    glActiveTexture(GL_TEXTURE0 + 4);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    
    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, Video_overlayTexId);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, Video_overlayMapBuffer.format.width, Video_overlayMapBuffer.format.height, GL_RED, GL_UNSIGNED_BYTE, stdDisplay_VBufferPixels(&Video_overlayMapBuffer));

    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, displaypal_texture);

    glActiveTexture(GL_TEXTURE0 + 0);
    glUniform1i(programMenu_uniform_tex, 0);
    glUniform1i(programMenu_uniform_displayPalette, 1);

    D3DVERTEX* vertexes = GL_tmpVertices;
    glBindBuffer(GL_ARRAY_BUFFER, menu_vbo_all);
    std3D_uploadBuffer(menu_vbo_all, GL_ARRAY_BUFFER, GL_tmpVerticesAmt * sizeof(D3DVERTEX), GL_tmpVertices, &std3D_menuVboCap);
    glVertexAttribPointer(
        programMenu_attribute_coord3d, // attribute
        3,                 // number of elements per vertex, here (x,y,z)
        GL_FLOAT,          // the type of each element
        GL_FALSE,          // normalize fixed-point data?
        sizeof(D3DVERTEX),                 // data stride
        (GLvoid*)offsetof(D3DVERTEX, x)                  // offset of first element
    );
    
    glVertexAttribPointer(
        programMenu_attribute_v_color, // attribute
        4,                 // number of elements per vertex, here (R,G,B,A)
        GL_UNSIGNED_BYTE,  // the type of each element
        GL_TRUE,          // normalize fixed-point data?
        sizeof(D3DVERTEX),                 // no extra data between each position
        (GLvoid*)offsetof(D3DVERTEX, color) // offset of first element
    );

    glVertexAttribPointer(
        programMenu_attribute_v_uv,    // attribute
        2,                 // number of elements per vertex, here (U,V)
        GL_FLOAT,          // the type of each element
        GL_FALSE,          // take our values as-is
        sizeof(D3DVERTEX),                 // no extra data between each position
        (GLvoid*)offsetof(D3DVERTEX, tu)                  // offset of first element
    );

    glEnableVertexAttribArray(programMenu_attribute_coord3d);
    glEnableVertexAttribArray(programMenu_attribute_v_color);
    glEnableVertexAttribArray(programMenu_attribute_v_uv);


    {

    float maxX, maxY, scaleX, scaleY, width, height;

    scaleX = 1.0/((double)Window_xSize / 2.0);
    scaleY = 1.0/((double)Window_ySize / 2.0);
    maxX = 1.0;
    maxY = 1.0;
    width = Window_xSize;
    height = Window_ySize;
    
    float d3dmat[16] = {
       maxX*scaleX,      0,                                          0,      0, // right
       0,                                       -maxY*scaleY,               0,      0, // up
       0,                                       0,                                          1,     0, // forward
       -(width/2)*scaleX,  (height/2)*scaleY,     -1,      1  // pos
    };
    
    glUniformMatrix4fv(programMenu_uniform_mvp, 1, GL_FALSE, d3dmat);
#if defined(TARGET_LINUX_GLES)
    Window_SetPresentViewport();
#else
    glViewport(0, 0, width, height);
#endif

    }
    
    rdTri* tris = GL_tmpTris;
    
    rdDDrawSurface* last_tex = (rdDDrawSurface*)(intptr_t)-1;
    int last_tex_idx = 0;
    //GLushort* data_elements = malloc(sizeof(GLushort) * 3 * GL_tmpTrisAmt);
    for (int j = 0; j < GL_tmpTrisAmt; j++)
    {
        menu_data_elements[(j*3)+0] = tris[j].v1;
        menu_data_elements[(j*3)+1] = tris[j].v2;
        menu_data_elements[(j*3)+2] = tris[j].v3;
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, menu_ibo_triangle);
    std3D_uploadBuffer(menu_ibo_triangle, GL_ELEMENT_ARRAY_BUFFER, GL_tmpTrisAmt * 3 * sizeof(GLushort), menu_data_elements, &std3D_menuIboCap);

    int tris_size = 0;  
    glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &tris_size);
    glDrawElements(GL_TRIANGLES, tris_size / sizeof(GLushort), GL_UNSIGNED_SHORT, 0);

    glDisableVertexAttribArray(programMenu_attribute_v_uv);
    glDisableVertexAttribArray(programMenu_attribute_v_color);
    glDisableVertexAttribArray(programMenu_attribute_coord3d);
}

static void std3D_GetUILayoutSize(double *outW, double *outH)
{
    if (jkGuiBuildMulti_bRendering || jkCutscene_isRendering) {
        *outW = 640.0;
        *outH = 480.0;
        return;
    }
    if (jkGame_isDDraw && Video_format.width > 0 && Video_format.height > 0) {
        *outW = (double)Video_format.width;
        *outH = (double)Video_format.height;
        return;
    }
    *outW = (double)Video_menuBuffer.format.width;
    *outH = (double)Video_menuBuffer.format.height;
    if (*outW <= 0.0 || *outH <= 0.0) {
        *outW = 640.0;
        *outH = 480.0;
    }
}

void std3D_DrawUIBitmapRGBA(stdBitmap* pBmp, int mipIdx, flex_t dstX, flex_t dstY, rdRect* srcRect, flex_t scaleX, flex_t scaleY, int bAlphaOverwrite, uint8_t color_r, uint8_t color_g, uint8_t color_b, uint8_t color_a)
{
    double internalWidth;
    double internalHeight;

    if (!pBmp) return;
    if (!pBmp->abLoadedToGPU[mipIdx]) {
        std3D_AddBitmapToTextureCache(pBmp, mipIdx, !(pBmp->palFmt & 1), 0);
    }

    std3D_GetUILayoutSize(&internalWidth, &internalHeight);

    double scaleX_ = (double)Window_xSize/internalWidth;
    double scaleY_ = (double)Window_ySize/internalHeight;

    dstX *= scaleX_;
    dstY *= scaleY_;

    //double tex_w = (double)Window_xSize;
    //double tex_h = (double)Window_ySize;
    double tex_w = pBmp->mipSurfaces[0]->format.width;
    double tex_h = pBmp->mipSurfaces[0]->format.height;

    double w = tex_w;
    double h = tex_h;
    double x = 0;
    double y = 0;

    if (srcRect) {
        x = srcRect->x;
        y = srcRect->y;
        w = srcRect->width;
        h = srcRect->height;
    }

    float w_dst = w;
    float h_dst = h;

    if (scaleX == 0.0 && scaleY == 0.0)
    {
        w_dst = (w / tex_w) * (double)Window_xSize;
        h_dst = (h / tex_h) * (double)Window_ySize;

        dstX = (dstX / tex_w) * (double)Window_xSize;
        dstY = (dstY / tex_h) * (double)Window_ySize;

        scaleX = 1.0;
        scaleY = 1.0;
    }

    double dstScaleX = scaleX;
    double dstScaleY = scaleY;
    dstScaleX *= scaleX_;
    dstScaleY *= scaleY_;

    double u1 = (x / tex_w);
    double u2 = ((x+w) / tex_w);
    double v1 = (y / tex_h);
    double v2 = ((y+h) / tex_h);

    uint32_t color = 0;

    color |= (color_r << 0);
    color |= (color_g << 8);
    color |= (color_b << 16);
    color |= (color_a << 24);

    if (GL_tmpUIVerticesAmt + 4 > STD3D_MAX_UI_VERTICES) {
        return;
    }
    if (GL_tmpUITrisAmt + 2 > STD3D_MAX_UI_TRIS) {
        return;
    }

    if (dstY + (dstScaleY * h_dst) < 0.0 || dstX + (dstScaleX * w_dst) < 0.0) {
        return;
    }
    if (dstY > Window_ySize || dstX > Window_xSize) {
        return;
    }

    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].x = dstX;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].y = dstY;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].z = 0.0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].tu = u1;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].tv = v1;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].nx = 0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].color = color;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].nz = 0;
    
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].x = dstX;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].y = dstY + (dstScaleY * h_dst);
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].z = 0.0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].tu = u1;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].tv = v2;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].nx = 0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].color = color;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].nz = 0;
    
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].x = dstX + (dstScaleX * w_dst);
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].y = dstY + (dstScaleY * h_dst);
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].z = 0.0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].tu = u2;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].tv = v2;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].nx = 0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].color = color;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].nz = 0;
    
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].x = dstX + (dstScaleX * w_dst);
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].y = dstY;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].z = 0.0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].tu = u2;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].tv = v1;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].nx = 0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].color = color;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].nz = 0;
    
    GL_tmpUITris[GL_tmpUITrisAmt+0].v1 = GL_tmpUIVerticesAmt+1;
    GL_tmpUITris[GL_tmpUITrisAmt+0].v2 = GL_tmpUIVerticesAmt+0;
    GL_tmpUITris[GL_tmpUITrisAmt+0].v3 = GL_tmpUIVerticesAmt+2;
    GL_tmpUITris[GL_tmpUITrisAmt+0].flags = bAlphaOverwrite;
    GL_tmpUITris[GL_tmpUITrisAmt+0].texture = pBmp->aTextureIds[mipIdx];
    
    GL_tmpUITris[GL_tmpUITrisAmt+1].v1 = GL_tmpUIVerticesAmt+0;
    GL_tmpUITris[GL_tmpUITrisAmt+1].v2 = GL_tmpUIVerticesAmt+3;
    GL_tmpUITris[GL_tmpUITrisAmt+1].v3 = GL_tmpUIVerticesAmt+2;
    GL_tmpUITris[GL_tmpUITrisAmt+1].flags = bAlphaOverwrite;
    GL_tmpUITris[GL_tmpUITrisAmt+1].texture = pBmp->aTextureIds[mipIdx];
    
    GL_tmpUIVerticesAmt += 4;
    GL_tmpUITrisAmt += 2;
}

void std3D_DrawUIBitmap(stdBitmap* pBmp, int mipIdx, flex_t dstX, flex_t dstY, rdRect* srcRect, flex_t scale, int bAlphaOverwrite)
{
    std3D_DrawUIBitmapRGBA(pBmp, mipIdx, dstX, dstY, srcRect, scale, scale, bAlphaOverwrite, 0xFF, 0xFF, 0xFF, 0xFF);
}

void std3D_DrawUIClearedRect(uint8_t palIdx, rdRect* dstRect)
{
    if (!displaypal_data) return;
    uint32_t color = 0;
    uint8_t color_r = ((uint8_t*)displaypal_data)[(palIdx*3) + 0];
    uint8_t color_g = ((uint8_t*)displaypal_data)[(palIdx*3) + 1];
    uint8_t color_b = ((uint8_t*)displaypal_data)[(palIdx*3) + 2];

    std3D_DrawUIClearedRectRGBA(color_r, color_g, color_b, 0xFF, dstRect);
}

void std3D_DrawUIClearedRectRGBA(uint8_t color_r, uint8_t color_g, uint8_t color_b, uint8_t color_a, rdRect* dstRect)
{
    if (!has_initted) return;
    if (!dstRect) return;
    double dstX = dstRect->x;
    double dstY = dstRect->y;

    double internalWidth;
    double internalHeight;
    std3D_GetUILayoutSize(&internalWidth, &internalHeight);
    if (!internalWidth || !internalHeight) return;

    double scaleX = (double)Window_xSize/internalWidth;
    double scaleY = (double)Window_ySize/internalHeight;

    dstX *= scaleX;
    dstY *= scaleY;

    //double tex_w = (double)Window_xSize;
    //double tex_h = (double)Window_ySize;
    double tex_w = dstRect->width;
    double tex_h = dstRect->height;
    if (!tex_w || !tex_h) return;

    double w = tex_w;
    double h = tex_h;
    double x = 0;
    double y = 0;

    float w_dst = w;
    float h_dst = h;
    double scale = 1.0;

    double dstScaleX = scale;
    double dstScaleY = scale;
    dstScaleX *= scaleX;
    dstScaleY *= scaleY;

    double u1 = (x / tex_w);
    double u2 = ((x+w) / tex_w);
    double v1 = (y / tex_h);
    double v2 = ((y+h) / tex_h);

    uint32_t color = 0;

    color |= (color_r << 0);
    color |= (color_g << 8);
    color |= (color_b << 16);
    color |= (color_a << 24);
    if (GL_tmpUIVerticesAmt + 4 > STD3D_MAX_UI_VERTICES) {
        return;
    }
    if (GL_tmpUITrisAmt + 2 > STD3D_MAX_UI_TRIS) {
        return;
    }

    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].x = dstX;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].y = dstY;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].z = 0.0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].tu = u1;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].tv = v1;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].nx = 0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].color = color;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].nz = 0;
    
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].x = dstX;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].y = dstY + (dstScaleY * h_dst);
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].z = 0.0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].tu = u1;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].tv = v2;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].nx = 0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].color = color;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].nz = 0;
    
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].x = dstX + (dstScaleX * w_dst);
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].y = dstY + (dstScaleY * h_dst);
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].z = 0.0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].tu = u2;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].tv = v2;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].nx = 0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].color = color;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].nz = 0;
    
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].x = dstX + (dstScaleX * w_dst);
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].y = dstY;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].z = 0.0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].tu = u2;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].tv = v1;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].nx = 0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].color = color;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].nz = 0;
    
    GL_tmpUITris[GL_tmpUITrisAmt+0].v1 = GL_tmpUIVerticesAmt+1;
    GL_tmpUITris[GL_tmpUITrisAmt+0].v2 = GL_tmpUIVerticesAmt+0;
    GL_tmpUITris[GL_tmpUITrisAmt+0].v3 = GL_tmpUIVerticesAmt+2;
    GL_tmpUITris[GL_tmpUITrisAmt+0].flags = 0;
    GL_tmpUITris[GL_tmpUITrisAmt+0].texture = blank_tex_white;
    
    GL_tmpUITris[GL_tmpUITrisAmt+1].v1 = GL_tmpUIVerticesAmt+0;
    GL_tmpUITris[GL_tmpUITrisAmt+1].v2 = GL_tmpUIVerticesAmt+3;
    GL_tmpUITris[GL_tmpUITrisAmt+1].v3 = GL_tmpUIVerticesAmt+2;
    GL_tmpUITris[GL_tmpUITrisAmt+1].flags = 0;
    GL_tmpUITris[GL_tmpUITrisAmt+1].texture = blank_tex_white;
    
    GL_tmpUIVerticesAmt += 4;
    GL_tmpUITrisAmt += 2;
}

void std3D_DrawUIRenderList()
{
    if (Main_bHeadless) return;
    if (!GL_tmpUITrisAmt) return;

    //glFlush();

    //printf("Draw render list\n");
    glBindFramebuffer(GL_FRAMEBUFFER, std3D_windowFbo);
    glDepthMask(GL_TRUE);
    glCullFace(GL_FRONT);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDepthFunc(GL_ALWAYS);
    glUseProgram(std3D_uiProgram.program); // TODO: simpler shader
    
    last_ui_tex = 0;
    last_ui_flags = -1;

    // Generate vertices list
    D3DVERTEX* vertexes = GL_tmpUIVertices;

    float maxX, maxY, scaleX, scaleY, width, height;

    float internalWidth = Window_xSize;//Video_menuBuffer.format.width;
    float internalHeight = Window_ySize;//Video_menuBuffer.format.height;

    if (jkGuiBuildMulti_bRendering) {
        internalWidth = 640.0;
        internalHeight = 480.0;
    }

    maxX = 1.0;
    maxY = 1.0;
    scaleX = 1.0/((double)internalWidth / 2.0);
    scaleY = 1.0/((double)internalHeight / 2.0);
    width = Window_xSize;
    height = Window_ySize;

    if (jkGuiBuildMulti_bRendering) {
        width = 640;
        height = 480;
    }

    // JKDF2's vertical FOV is fixed with their projection, for whatever reason. 
    // This ends up resulting in the view looking squished vertically at wide/ultrawide aspect ratios.
    // To compensate, we zoom the y axis here.
    // I also went ahead and fixed vertical displays in the same way because it seems to look better.
    float zoom_yaspect = 1.0;
    float zoom_xaspect = 1.0;
    
    float shift_add_x = 0;
    float shift_add_y = 0;
    
    glUniform1i(std3D_uiProgram.uniform_tex, 0);
    glUniform1i(std3D_uiProgram.uniform_tex2, 1);
    glUniform1i(std3D_uiProgram.uniform_tex3, 2);

    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, blank_tex_white);
    
    {
    
    float d3dmat[16] = {
       maxX*scaleX,      0,                                          0,      0, // right
       0,                                       -maxY*scaleY,               0,      0, // up
       0,                                       0,                                          1,     0, // forward
       -(width/2)*scaleX,  (height/2)*scaleY,     -1,      1  // pos
    };
    
    glUniformMatrix4fv(std3D_uiProgram.uniform_mvp, 1, GL_FALSE, d3dmat);
#if defined(TARGET_LINUX_GLES)
    Window_SetPresentViewport();
#else
    glViewport(0, 0, width, height);
#endif
    glUniform2f(std3D_uiProgram.uniform_iResolution, internalWidth, internalHeight);

    float param1 = 1.0;
    float param2 = 1.0;

    glUniform1f(std3D_uiProgram.uniform_param1, param1);
    glUniform1f(std3D_uiProgram.uniform_param2, param2);
    glUniform1f(std3D_uiProgram.uniform_param3, jkPlayer_gamma);
    
    }

    rdUITri* tris = GL_tmpUITris;
    glEnableVertexAttribArray(std3D_uiProgram.attribute_coord3d);
    glEnableVertexAttribArray(std3D_uiProgram.attribute_v_color);
    glEnableVertexAttribArray(std3D_uiProgram.attribute_v_uv);

    glBindBuffer(GL_ARRAY_BUFFER, menu_vbo_all);
    std3D_uploadBuffer(menu_vbo_all, GL_ARRAY_BUFFER, GL_tmpUIVerticesAmt * sizeof(D3DVERTEX), GL_tmpUIVertices, &std3D_menuVboCap);
    glVertexAttribPointer(
        std3D_uiProgram.attribute_coord3d, // attribute
        3,                 // number of elements per vertex, here (x,y,z)
        GL_FLOAT,          // the type of each element
        GL_FALSE,          // normalize fixed-point data?
        sizeof(D3DVERTEX),                 // data stride
        (GLvoid*)offsetof(D3DVERTEX, x)                  // offset of first element
    );
    
    glVertexAttribPointer(
        std3D_uiProgram.attribute_v_color, // attribute
        4,                 // number of elements per vertex, here (R,G,B,A)
        GL_UNSIGNED_BYTE,  // the type of each element
        GL_TRUE,          // normalize fixed-point data?
        sizeof(D3DVERTEX),                 // no extra data between each position
        (GLvoid*)offsetof(D3DVERTEX, color) // offset of first element
    );

    /*glVertexAttribPointer(
        std3D_uiProgram.attribute_v_light, // attribute
        1,                 // number of elements per vertex, here (L)
        GL_FLOAT,  // the type of each element
        GL_FALSE,          // normalize fixed-point data?
        sizeof(D3DVERTEX),                 // no extra data between each position
        (GLvoid*)offsetof(D3DVERTEX, lightLevel) // offset of first element
    );*/

    glVertexAttribPointer(
        std3D_uiProgram.attribute_v_uv,    // attribute
        2,                 // number of elements per vertex, here (U,V)
        GL_FLOAT,          // the type of each element
        GL_FALSE,          // take our values as-is
        sizeof(D3DVERTEX),                 // no extra data between each position
        (GLvoid*)offsetof(D3DVERTEX, tu)                  // offset of first element
    );
    
    //glEnableVertexAttribArray(attribute_v_norm);

    int last_flags = 0;
    int last_tex_idx = 0;
    //GLushort* menu_data_elements = malloc(sizeof(GLushort) * 3 * GL_tmpTrisAmt);
    for (int j = 0; j < GL_tmpUITrisAmt; j++)
    {
        menu_data_elements[(j*3)+0] = tris[j].v1;
        menu_data_elements[(j*3)+1] = tris[j].v2;
        menu_data_elements[(j*3)+2] = tris[j].v3;
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, menu_ibo_triangle);
    std3D_uploadBuffer(menu_ibo_triangle, GL_ELEMENT_ARRAY_BUFFER, GL_tmpUITrisAmt * 3 * sizeof(GLushort), menu_data_elements, &std3D_menuIboCap);
    
    int do_batch = 0;

    int tex_id = tris[0].texture;
    glActiveTexture(GL_TEXTURE0 + 0);
    if (tex_id == 0)
        glBindTexture(GL_TEXTURE_2D, blank_tex_white);
    else
        glBindTexture(GL_TEXTURE_2D, tex_id);

    if (tris[0].flags) {
        glUniform1f(std3D_uiProgram.uniform_param1, 1.0);
    }
    else {
        glUniform1f(std3D_uiProgram.uniform_param1, 0.0);
    }
    
    for (int j = 0; j < GL_tmpUITrisAmt; j++)
    {
        if (tris[j].texture != last_ui_tex || tris[j].flags != last_ui_flags)
        {
            do_batch = 1;
        }
        
        if (do_batch)
        {
            int num_tris_batch = j - last_tex_idx;

            if (num_tris_batch)
            {
                //printf("batch %u~%u\n", last_tex_idx, j);
                glDrawElements(GL_TRIANGLES, num_tris_batch * 3, GL_UNSIGNED_SHORT, (GLvoid*)((intptr_t)&menu_data_elements[last_tex_idx * 3] - (intptr_t)&menu_data_elements[0]));
            }

            int tex_id = tris[j].texture;
            glActiveTexture(GL_TEXTURE0 + 0);
            if (tex_id == 0)
                glBindTexture(GL_TEXTURE_2D, blank_tex_white);
            else
                glBindTexture(GL_TEXTURE_2D, tex_id);

            if (tris[j].flags) {
                glUniform1f(std3D_uiProgram.uniform_param1, 1.0);
            }
            else {
                glUniform1f(std3D_uiProgram.uniform_param1, 0.0);
            }
            
            last_ui_tex = tris[j].texture;
            last_ui_flags = tris[j].flags;
            last_tex_idx = j;

            do_batch = 0;
        }
        //printf("tri %u: %u,%u,%u\n", j, tris[j].v1, tris[j].v2, tris[j].v3);
        
        
        /*int vert = tris[j].v1;
        stdPlatform_Printf("%u: %f %f %f, %f %f %f, %f %f\n", vert, vertexes[vert].x, vertexes[vert].y, vertexes[vert].z,
                                      vertexes[vert].nx, vertexes[vert].ny, vertexes[vert].nz,
                                      vertexes[vert].tu, vertexes[vert].tv);
        
        vert = tris[j].v2;
        stdPlatform_Printf("%u: %f %f %f, %f %f %f, %f %f\n", vert, vertexes[vert].x, vertexes[vert].y, vertexes[vert].z,
                                      vertexes[vert].nx, vertexes[vert].ny, vertexes[vert].nz,
                                      vertexes[vert].tu, vertexes[vert].tv);
        
        vert = tris[j].v3;
        stdPlatform_Printf("%u: %f %f %f, %f %f %f, %f %f\n", vert, vertexes[vert].x, vertexes[vert].y, vertexes[vert].z,
                                      vertexes[vert].nx, vertexes[vert].ny, vertexes[vert].nz,
                                      vertexes[vert].tu, vertexes[vert].tv);*/
    }
    
    int remaining_batch = GL_tmpUITrisAmt - last_tex_idx;

    if (remaining_batch)
    {
        glDrawElements(GL_TRIANGLES, remaining_batch * 3, GL_UNSIGNED_SHORT, (GLvoid*)((intptr_t)&menu_data_elements[last_tex_idx * 3] - (intptr_t)&menu_data_elements[0]));
    }

    // Done drawing    
    glBindTexture(GL_TEXTURE_2D, blank_tex_white);

    glDisableVertexAttribArray(std3D_uiProgram.attribute_coord3d);
    glDisableVertexAttribArray(std3D_uiProgram.attribute_v_color);
    glDisableVertexAttribArray(std3D_uiProgram.attribute_v_uv);
    
    std3D_ResetUIRenderList();
}

void std3D_DrawSimpleTex(std3DSimpleTexStage* pStage, std3DIntermediateFbo* pFbo, GLuint texId, GLuint texId2, GLuint texId3, flex_t param1, flex_t param2, flex_t param3, int gen_mips)
{
    glBindFramebuffer(GL_FRAMEBUFFER, pFbo->fbo);
    glDepthFunc(GL_ALWAYS);
    glUseProgram(pStage->program);
    
    float menu_w, menu_h, menu_u, menu_v, menu_x;
    menu_w = (double)pFbo->w;
    menu_h = (double)pFbo->h;
    menu_u = 1.0;
    menu_v = 1.0;
    menu_x = 0.0;

    GL_tmpVertices[0].x = menu_x;
    GL_tmpVertices[0].y = 0.0;
    GL_tmpVertices[0].z = 0.0;
    GL_tmpVertices[0].tu = 0.0;
    GL_tmpVertices[0].tv = menu_v;
    *(uint32_t*)&GL_tmpVertices[0].nx = 0;
    GL_tmpVertices[0].color = 0xFFFFFFFF;
    *(uint32_t*)&GL_tmpVertices[0].nz = 0;
    
    GL_tmpVertices[1].x = menu_x;
    GL_tmpVertices[1].y = menu_h;
    GL_tmpVertices[1].z = 0.0;
    GL_tmpVertices[1].tu = 0.0;
    GL_tmpVertices[1].tv = 0.0;
    *(uint32_t*)&GL_tmpVertices[1].nx = 0;
    GL_tmpVertices[1].color = 0xFFFFFFFF;
    *(uint32_t*)&GL_tmpVertices[1].nz = 0;
    
    GL_tmpVertices[2].x = menu_x + menu_w;
    GL_tmpVertices[2].y = menu_h;
    GL_tmpVertices[2].z = 0.0;
    GL_tmpVertices[2].tu = menu_u;
    GL_tmpVertices[2].tv = 0.0;
    *(uint32_t*)&GL_tmpVertices[2].nx = 0;
    GL_tmpVertices[2].color = 0xFFFFFFFF;
    *(uint32_t*)&GL_tmpVertices[2].nz = 0;
    
    GL_tmpVertices[3].x = menu_x + menu_w;
    GL_tmpVertices[3].y = 0.0;
    GL_tmpVertices[3].z = 0.0;
    GL_tmpVertices[3].tu = menu_u;
    GL_tmpVertices[3].tv = menu_v;
    *(uint32_t*)&GL_tmpVertices[3].nx = 0;
    GL_tmpVertices[3].color = 0xFFFFFFFF;
    *(uint32_t*)&GL_tmpVertices[3].nz = 0;
    
    GL_tmpTris[0].v1 = 1;
    GL_tmpTris[0].v2 = 0;
    GL_tmpTris[0].v3 = 2;
    
    GL_tmpTris[1].v1 = 0;
    GL_tmpTris[1].v2 = 3;
    GL_tmpTris[1].v3 = 2;
    
    GL_tmpVerticesAmt = 4;
    GL_tmpTrisAmt = 2;
    
    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, texId);
    if (gen_mips)
        glGenerateMipmap(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, texId2 ? texId2 : blank_tex);
    if (texId2 && gen_mips)
        glGenerateMipmap(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, texId3 ? texId3 : blank_tex);
    if (texId3 && gen_mips)
        glGenerateMipmap(GL_TEXTURE_2D);

    GLushort data_elements[32 * 3];

    D3DVERTEX* vertexes = GL_tmpVertices;
    
    glUniform1i(pStage->uniform_tex, 0);
    glUniform1i(pStage->uniform_tex2, 1);
    glUniform1i(pStage->uniform_tex3, 2);
    
    {

    float maxX, maxY, scaleX, scaleY, width, height;

    scaleX = 1.0/((double)pFbo->w / 2.0);
    scaleY = 1.0/((double)pFbo->h / 2.0);
    maxX = 1.0;
    maxY = 1.0;
    width = pFbo->w;
    height = pFbo->h;
    
    float d3dmat[16] = {
       maxX*scaleX,      0,                                          0,      0, // right
       0,                                       -maxY*scaleY,               0,      0, // up
       0,                                       0,                                          1,     0, // forward
       -(width/2)*scaleX,  (height/2)*scaleY,     -1,      1  // pos
    };
    
    glUniformMatrix4fv(pStage->uniform_mvp, 1, GL_FALSE, d3dmat);
    if (pFbo->fbo == std3D_windowFbo) {
#if defined(TARGET_LINUX_GLES)
        Window_SetPresentViewport();
#else
        glViewport(0, 0, width, height);
#endif
    } else {
        glViewport(0, 0, width, height);
    }
    glUniform2f(pStage->uniform_iResolution, pFbo->iw, pFbo->ih);

    glUniform1f(pStage->uniform_param1, param1);
    glUniform1f(pStage->uniform_param2, param2);
    glUniform1f(pStage->uniform_param3, param3);

    }
    
    rdTri* tris = GL_tmpTris;
    glEnableVertexAttribArray(pStage->attribute_coord3d);
    glEnableVertexAttribArray(pStage->attribute_v_color);
    glEnableVertexAttribArray(pStage->attribute_v_uv);

    glBindBuffer(GL_ARRAY_BUFFER, menu_vbo_all);
    std3D_uploadBuffer(menu_vbo_all, GL_ARRAY_BUFFER, GL_tmpVerticesAmt * sizeof(D3DVERTEX), GL_tmpVertices, &std3D_menuVboCap);
    glVertexAttribPointer(
        pStage->attribute_coord3d,
        3, GL_FLOAT, GL_FALSE, sizeof(D3DVERTEX), (GLvoid*)offsetof(D3DVERTEX, x));
    glVertexAttribPointer(
        pStage->attribute_v_color,
        4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(D3DVERTEX), (GLvoid*)offsetof(D3DVERTEX, color));
    glVertexAttribPointer(
        pStage->attribute_v_uv,
        2, GL_FLOAT, GL_FALSE, sizeof(D3DVERTEX), (GLvoid*)offsetof(D3DVERTEX, tu));
    
    rdDDrawSurface* last_tex = (rdDDrawSurface*)(intptr_t)-1;
    int last_tex_idx = 0;
    //GLushort* data_elements = malloc(sizeof(GLushort) * 3 * GL_tmpTrisAmt);
    for (int j = 0; j < GL_tmpTrisAmt; j++)
    {
        data_elements[(j*3)+0] = tris[j].v1;
        data_elements[(j*3)+1] = tris[j].v2;
        data_elements[(j*3)+2] = tris[j].v3;
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, menu_ibo_triangle);
    std3D_uploadBuffer(menu_ibo_triangle, GL_ELEMENT_ARRAY_BUFFER, GL_tmpTrisAmt * 3 * sizeof(GLushort), data_elements, &std3D_menuIboCap);
    glDrawElements(GL_TRIANGLES, GL_tmpTrisAmt * 3, GL_UNSIGNED_SHORT, 0);

    glDisableVertexAttribArray(pStage->attribute_v_uv);
    glDisableVertexAttribArray(pStage->attribute_v_color);
    glDisableVertexAttribArray(pStage->attribute_coord3d);
    
    //free(data_elements);
        
    //glBindTexture(GL_TEXTURE_2D, 0);
}

static void std3D_DrawSceneComposite(std3DIntermediateFbo *pFbo, GLuint albedoTex, GLuint emissiveTex, flex_t gamma)
{
    std3DSimpleTexStage *pStage = &std3D_texFboSceneStage;

    glBindFramebuffer(GL_FRAMEBUFFER, pFbo->fbo);
    glDepthFunc(GL_ALWAYS);
    glUseProgram(pStage->program);

    GL_tmpVertices[0].x = 0.0f;
    GL_tmpVertices[0].y = 0.0f;
    GL_tmpVertices[0].z = 0.0f;
    GL_tmpVertices[0].tu = 0.0f;
    GL_tmpVertices[0].tv = 1.0f;
    GL_tmpVertices[0].color = 0xFFFFFFFF;

    GL_tmpVertices[1].x = 0.0f;
    GL_tmpVertices[1].y = (float)pFbo->h;
    GL_tmpVertices[1].z = 0.0f;
    GL_tmpVertices[1].tu = 0.0f;
    GL_tmpVertices[1].tv = 0.0f;
    GL_tmpVertices[1].color = 0xFFFFFFFF;

    GL_tmpVertices[2].x = (float)pFbo->w;
    GL_tmpVertices[2].y = (float)pFbo->h;
    GL_tmpVertices[2].z = 0.0f;
    GL_tmpVertices[2].tu = 1.0f;
    GL_tmpVertices[2].tv = 0.0f;
    GL_tmpVertices[2].color = 0xFFFFFFFF;

    GL_tmpVertices[3].x = (float)pFbo->w;
    GL_tmpVertices[3].y = 0.0f;
    GL_tmpVertices[3].z = 0.0f;
    GL_tmpVertices[3].tu = 1.0f;
    GL_tmpVertices[3].tv = 1.0f;
    GL_tmpVertices[3].color = 0xFFFFFFFF;

    GL_tmpTris[0].v1 = 1;
    GL_tmpTris[0].v2 = 0;
    GL_tmpTris[0].v3 = 2;
    GL_tmpTris[1].v1 = 0;
    GL_tmpTris[1].v2 = 3;
    GL_tmpTris[1].v3 = 2;
    GL_tmpVerticesAmt = 4;
    GL_tmpTrisAmt = 2;

    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, albedoTex);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, emissiveTex);

    {
        float scaleX = 1.0f / ((float)pFbo->w / 2.0f);
        float scaleY = 1.0f / ((float)pFbo->h / 2.0f);
        float d3dmat[16] = {
            scaleX, 0, 0, 0,
            0, -scaleY, 0, 0,
            0, 0, 1, 0,
            -((float)pFbo->w / 2.0f) * scaleX, ((float)pFbo->h / 2.0f) * scaleY, -1, 1
        };
        glUniformMatrix4fv(pStage->uniform_mvp, 1, GL_FALSE, d3dmat);
#if defined(TARGET_LINUX_GLES)
        Window_SetPresentViewport();
#else
        glViewport(0, 0, pFbo->w, pFbo->h);
#endif
        glUniform2f(pStage->uniform_iResolution, (float)pFbo->iw, (float)pFbo->ih);
        glUniform1i(pStage->uniform_tex, 0);
        glUniform1i(pStage->uniform_tex2, 1);
        glUniform1f(pStage->uniform_param3, gamma);
    }

    glEnableVertexAttribArray(pStage->attribute_coord3d);
    glEnableVertexAttribArray(pStage->attribute_v_color);
    glEnableVertexAttribArray(pStage->attribute_v_uv);

    glBindBuffer(GL_ARRAY_BUFFER, menu_vbo_all);
    std3D_uploadBuffer(menu_vbo_all, GL_ARRAY_BUFFER, GL_tmpVerticesAmt * sizeof(D3DVERTEX), GL_tmpVertices, &std3D_menuVboCap);
    glVertexAttribPointer(pStage->attribute_coord3d, 3, GL_FLOAT, GL_FALSE, sizeof(D3DVERTEX), (GLvoid*)offsetof(D3DVERTEX, x));
    glVertexAttribPointer(pStage->attribute_v_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(D3DVERTEX), (GLvoid*)offsetof(D3DVERTEX, color));
    glVertexAttribPointer(pStage->attribute_v_uv, 2, GL_FLOAT, GL_FALSE, sizeof(D3DVERTEX), (GLvoid*)offsetof(D3DVERTEX, tu));

    GLushort data_elements[32 * 3];
    for (int j = 0; j < GL_tmpTrisAmt; j++) {
        data_elements[(j * 3) + 0] = GL_tmpTris[j].v1;
        data_elements[(j * 3) + 1] = GL_tmpTris[j].v2;
        data_elements[(j * 3) + 2] = GL_tmpTris[j].v3;
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, menu_ibo_triangle);
    std3D_uploadBuffer(menu_ibo_triangle, GL_ELEMENT_ARRAY_BUFFER, GL_tmpTrisAmt * 3 * sizeof(GLushort), data_elements, &std3D_menuIboCap);
    glDrawElements(GL_TRIANGLES, GL_tmpTrisAmt * 3, GL_UNSIGNED_SHORT, 0);

    glDisableVertexAttribArray(pStage->attribute_v_uv);
    glDisableVertexAttribArray(pStage->attribute_v_color);
    glDisableVertexAttribArray(pStage->attribute_coord3d);
}

void std3D_DrawSceneFbo()
{
    //printf("Draw scene FBO\n");
    glEnable(GL_BLEND);
    
    glBlendEquation(GL_FUNC_ADD);

    glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->window.fbo);
    Window_BeginScreenDraw();

    static float frameNum = 1.0;
    //frameNum += (rand() % 16);

    int draw_ssao = jkPlayer_enableSSAO;
    int draw_bloom = jkPlayer_enableBloom;

    float add_luma = (((float)rdroid_curColorEffects.add.x / 255.0f) * 0.2125)
                     + (((float)rdroid_curColorEffects.add.y / 255.0f)* 0.7154)
                     + (((float)rdroid_curColorEffects.add.z / 255.0f) * 0.0721); // FLEXTODO

    // HACK: Force blinding shouldn't show the SSAO
    if (add_luma >= 0.7) {
        draw_ssao = 0;
    }

    if (!jkGame_isDDraw && !jkGuiBuildMulti_bRendering)
    {
        return;
    }

    if (jkGuiBuildMulti_bRendering) {
        draw_ssao = 0;
        //draw_bloom = 0;
    }

    if (draw_bloom)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->blur1.fbo);
        glClear( GL_COLOR_BUFFER_BIT );
        glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->blur2.fbo);
        glClear( GL_COLOR_BUFFER_BIT );
        glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->blur3.fbo);
        glClear( GL_COLOR_BUFFER_BIT );
        glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->blur4.fbo);
        glClear( GL_COLOR_BUFFER_BIT );
        //glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->blurBlend.fbo);
        //glClear( GL_COLOR_BUFFER_BIT );
    }

    // Clear SSAO stuff
    if (draw_ssao)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->ssaoBlur1.fbo);
        glClear( GL_COLOR_BUFFER_BIT );
        glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->ssaoBlur2.fbo);
        glClear( GL_COLOR_BUFFER_BIT );
    }

    float rad_scale = (float)std3D_pFb->w / 640.0;

    if (!draw_ssao)
    {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        if (!draw_bloom) {
            std3D_DrawSceneComposite(&std3D_pFb->window, std3D_pFb->tex0, std3D_pFb->tex1, jkPlayer_gamma);
        } else {
            std3D_DrawSimpleTex(&std3D_texFboStage, &std3D_pFb->window, std3D_pFb->tex0, 0, 0, 1.0, 1.0, jkPlayer_gamma, 0);
        }
    }
    else
    {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        
        std3D_DrawSimpleTex(&std3D_ssaoStage, &std3D_pFb->ssaoBlur1, std3D_pFb->tex2, std3D_pFb->tex3, tiledrand_texture, frameNum, 0.0, 0.0, 0); // test ssao output
        std3D_DrawSimpleTex(&std3D_blurStage, &std3D_pFb->ssaoBlur2, std3D_pFb->ssaoBlur1.tex, 0, 0, 14.0, 3.0, 1.0 * rad_scale, 1);
        //std3D_DrawSimpleTex(&std3D_blurStage, &std3D_pFb->ssaoBlur3, std3D_pFb->ssaoBlur2.tex, 0, 0, 8.0, 3.0, 4.0);

        glBlendFunc(GL_SRC_ALPHA, GL_SRC_ALPHA);
        std3D_DrawSimpleTex(&std3D_ssaoMixStage, &std3D_pFb->window, std3D_pFb->ssaoBlur2.tex, std3D_pFb->tex0, 0, 0.0, 0.0, jkPlayer_gamma, 0);
    }

    glBlendFunc(GL_SRC_ALPHA, GL_SRC_ALPHA);
    if (!draw_bloom && draw_ssao)
        std3D_DrawSimpleTex(&std3D_texFboStage, &std3D_pFb->window, std3D_pFb->tex1, 0, 0, 1.0, 1.0, jkPlayer_gamma, 0);

    if (draw_bloom)
    {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        
        std3D_DrawSimpleTex(&std3D_blurStage, &std3D_pFb->blur1, std3D_pFb->tex1, 0, 0, 16.0, 3.0, 2.0 * rad_scale, 1);
        std3D_DrawSimpleTex(&std3D_blurStage, &std3D_pFb->blur2, std3D_pFb->blur1.tex, 0, 0, 16.0, 3.0, 2.0 * rad_scale, 1);
        std3D_DrawSimpleTex(&std3D_blurStage, &std3D_pFb->blur3, std3D_pFb->blur2.tex, 0, 0, 16.0, 3.0, 2.0 * rad_scale, 1);
        std3D_DrawSimpleTex(&std3D_blurStage, &std3D_pFb->blur4, std3D_pFb->blur3.tex, 0, 0, 16.0, 3.0, 2.0 * rad_scale, 1);

        float bloom_intensity = 1.0;
        float bloom_gamma = 1.0;
        glBlendFunc(GL_SRC_ALPHA, GL_SRC_ALPHA);
        /*std3D_DrawSimpleTex(&std3D_texFboStage, &std3D_pFb->blurBlend, std3D_pFb->tex1, 0, 0, 1.0, bloom_intensity * 1.0, bloom_gamma, 0);
        std3D_DrawSimpleTex(&std3D_texFboStage, &std3D_pFb->blurBlend, std3D_pFb->blur1.tex, 0, 0, 1.0, bloom_intensity * 1.0, bloom_gamma, 0);
        std3D_DrawSimpleTex(&std3D_texFboStage, &std3D_pFb->blurBlend, std3D_pFb->blur2.tex, 0, 0, 1.0, bloom_intensity * 1.2, bloom_gamma, 0);
        std3D_DrawSimpleTex(&std3D_texFboStage, &std3D_pFb->blurBlend, std3D_pFb->blur3.tex, 0, 0, 1.0, bloom_intensity * 1.0, bloom_gamma, 0);
        std3D_DrawSimpleTex(&std3D_texFboStage, &std3D_pFb->blurBlend, std3D_pFb->blur4.tex, 0, 0, 1.0, bloom_intensity * 1.2, bloom_gamma, 0);
        */

        glBlendFunc(GL_SRC_ALPHA, GL_SRC_ALPHA);
        //std3D_DrawSimpleTex(&std3D_texFboStage, &std3D_pFb->window, std3D_pFb->blurBlend.tex, 0, 0, 1.0, 1.0, 1.0, 0);
        std3D_DrawSimpleTex(&std3D_texFboStage, &std3D_pFb->window, std3D_pFb->tex1, 0, 0, 1.0, 1.5, jkPlayer_gamma, 0);
        std3D_DrawSimpleTex(&std3D_texFboStage, &std3D_pFb->window, std3D_pFb->blur1.tex, 0, 0, 1.0, bloom_intensity * 1.5, jkPlayer_gamma, 0);
        std3D_DrawSimpleTex(&std3D_texFboStage, &std3D_pFb->window, std3D_pFb->blur2.tex, 0, 0, 1.0, bloom_intensity * 1.0, jkPlayer_gamma, 0);
        std3D_DrawSimpleTex(&std3D_texFboStage, &std3D_pFb->window, std3D_pFb->blur3.tex, 0, 0, 1.0, bloom_intensity * 1.0, jkPlayer_gamma, 0);
        std3D_DrawSimpleTex(&std3D_texFboStage, &std3D_pFb->window, std3D_pFb->blur4.tex, 0, 0, 1.0, bloom_intensity * 0.8, jkPlayer_gamma, 0);
    }
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void std3D_DoTex(rdDDrawSurface* tex, rdTri* tri, int tris_left)
{
    if (!tex) {
        glActiveTexture(GL_TEXTURE0 + 3);
        glBindTexture(GL_TEXTURE_2D, blank_tex); // emissive
        glActiveTexture(GL_TEXTURE0 + 4);
        glBindTexture(GL_TEXTURE_2D, blank_tex); // displace

        glActiveTexture(GL_TEXTURE0 + 0);
        glBindTexture(GL_TEXTURE_2D, blank_tex_white);
        glUniform1i(uniform_tex_mode, TEX_MODE_TEST);
        glUniform1i(uniform_blend_mode, 2);
        return;
    }
    int tex_id = tex->texture_id;
    glActiveTexture(GL_TEXTURE0 + 0);
    if (tex_id == 0)
        glBindTexture(GL_TEXTURE_2D, blank_tex_white);
    else
        glBindTexture(GL_TEXTURE_2D, tex_id);

    int emiss_tex_id = tex->emissive_texture_id;
    glActiveTexture(GL_TEXTURE0 + 3);
    if (emiss_tex_id == 0) {
        glBindTexture(GL_TEXTURE_2D, blank_tex);
    }
    else {
        //printf("emissive tex id %x\n", emiss_tex_id);
        glBindTexture(GL_TEXTURE_2D, emiss_tex_id);

        // HACK
        if (tri[0].flags & 0x600) {
            //glUniform1i(uniform_blend_mode, 6);
            //last_flags |= 0x200;
        }

        
        for (int i = 0; i < tris_left; i++) {
            if (tri[i].texture != tex) break;
            if (tri[i].flags & 0x600) {
                //tri[i].flags |= 0x200;
            }
        }
    }

    int displace_tex_id = tex->displacement_texture_id;
    glActiveTexture(GL_TEXTURE0 + 4);
    if (displace_tex_id == 0) {
        glBindTexture(GL_TEXTURE_2D, blank_tex);
    }
    else {
        glBindTexture(GL_TEXTURE_2D, displace_tex_id);
    }
    //if (tex->emissive_factor[0] != 0.0 || tex->emissive_factor[1] != 0.0 || tex->emissive_factor[2] != 0.0)
    //    stdPlatform_Printf("%f %f %f\n", tex->emissive_factor[0], tex->emissive_factor[1], tex->emissive_factor[2]);
    float emissive_mult = (jkPlayer_enableBloom ? 1.0 : 5.0);
    glUniform3f(uniform_emissiveFactor, tex->emissive_factor[0] * emissive_mult, tex->emissive_factor[1] * emissive_mult, tex->emissive_factor[2] * emissive_mult);
    glUniform4f(uniform_albedoFactor, tex->albedo_factor[0], tex->albedo_factor[1], tex->albedo_factor[2], tex->albedo_factor[3]);
    if (tex->displacement_factor) {
        //printf("%f\n", tex->displacement_factor);
        //tex->displacement_factor = -0.4;
    }
    glUniform1f(uniform_displacement_factor, tex->displacement_factor);
    glActiveTexture(GL_TEXTURE0 + 0);

    if (!jkPlayer_enableTextureFilter)
        glUniform1i(uniform_tex_mode, tex->is_16bit ? TEX_MODE_16BPP : TEX_MODE_WORLDPAL);
    else
        glUniform1i(uniform_tex_mode, tex->is_16bit ? TEX_MODE_BILINEAR_16BPP : TEX_MODE_BILINEAR);
    
     glActiveTexture(GL_TEXTURE0 + 0);

    if (tex_id == 0)
        glUniform1i(uniform_tex_mode, TEX_MODE_TEST);
}

void std3D_DrawRenderList()
{
    if (Main_bHeadless) return;

    //printf("Draw render list\n");
    glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->fbo);
    std3D_bindDefaultProgram(jkPlayer_enableSSAO);

    if (jkPlayer_enableSSAO) {
        GLenum bufs[4] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
        glDrawBuffers(4, bufs);
    } else {
        GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(2, bufs);
    }
    
    last_tex = NULL;

    // Generate vertices list
    D3DVERTEX* vertexes = GL_tmpVertices;

    float maxX, maxY, scaleX, scaleY, width, height;

    float internalWidth = Video_menuBuffer.format.width;
    float internalHeight = Video_menuBuffer.format.height;

    if (jkGuiBuildMulti_bRendering) {
        internalWidth = 640.0;
        internalHeight = 480.0;
    }

    maxX = 1.0;
    maxY = 1.0;
    scaleX = 1.0/((double)internalWidth / 2.0);
    scaleY = 1.0/((double)internalHeight / 2.0);
    width = std3D_pFb->w;
    height = std3D_pFb->h;

    if (jkGuiBuildMulti_bRendering) {
        width = 640;
        height = 480;
    }

    // JKDF2's vertical FOV is fixed with their projection, for whatever reason. 
    // This ends up resulting in the view looking squished vertically at wide/ultrawide aspect ratios.
    // To compensate, we zoom the y axis here.
    // I also went ahead and fixed vertical displays in the same way because it seems to look better.
    float zoom_yaspect = 1.0;//(width/height);
    float zoom_xaspect = 1.0;//(height/width);

    if (height > width)
    {
        zoom_yaspect = 1.0;
    }

    if (width > height)
    {
        zoom_xaspect = 1.0;
    }

    // We no longer need all the weird squishing
    if (!jkGuiBuildMulti_bRendering) {
        zoom_yaspect = 1.0;
        zoom_xaspect = 1.0;
    }
    
    float shift_add_x = 0;
    float shift_add_y = 0;

    if (jkGuiBuildMulti_bRendering) {
        float menu_w, menu_h, menu_x;
        menu_w = (double)std3D_pFb->w;
        menu_h = (double)std3D_pFb->h;

        // Keep 4:3 aspect
        menu_x = (menu_w - (menu_h * (640.0 / 480.0))) / 2.0;

        width = std3D_pFb->w;
        height = std3D_pFb->h;

        zoom_xaspect = (height/width);

        shift_add_x = (((1.0 - ((menu_x * zoom_xaspect) / std3D_pFb->w)) + 0.15) * zoom_xaspect);
        shift_add_y = -0.5;
        zoom_yaspect = 1.0;
    }

    glBindBuffer(GL_ARRAY_BUFFER, world_vbo_all);
    std3D_uploadBuffer(world_vbo_all, GL_ARRAY_BUFFER, GL_tmpVerticesAmt * sizeof(D3DVERTEX), vertexes, &std3D_worldVboCap);
    
    glUniform1i(uniform_tex_mode, TEX_MODE_TEST);
    glUniform1i(uniform_blend_mode, 2);
    glActiveTexture(GL_TEXTURE0 + 4);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, worldpal_lights_texture);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, worldpal_texture);
    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, blank_tex_white);
    
    glUniform1i(uniform_tex, 0);
    glUniform1i(uniform_worldPalette, 1);
    glUniform1i(uniform_worldPaletteLights, 2);
    glUniform1i(uniform_texEmiss, 3);
    glUniform1i(uniform_displacement_map, 4);
    
    {
    
    float d3dmat[16] = {
       (float)(maxX*scaleX*zoom_xaspect),      0,                                          0,      0, // right
       0,                                       (float)(-maxY*scaleY*zoom_yaspect),               0,      0, // up
       0,                                       0,                                          1,     0, // forward
       (float)(-(internalWidth/2)*scaleX*zoom_xaspect + shift_add_x),  (float)((internalHeight/2)*scaleY*zoom_yaspect + shift_add_y),     (float)((!rdCamera_pCurCamera || rdCamera_pCurCamera->projectType == rdCameraProjectType_Perspective) ? -1 : 1),      1  // pos
    };
    
    glUniformMatrix4fv(uniform_mvp, 1, GL_FALSE, d3dmat);
    glViewport(0, 0, width, height);
    
    }

    glUniform2f(uniform_iResolution, width, height);

    //rdroid_curColorEffects.tint.x = 0.0;
    //rdroid_curColorEffects.tint.y = 0.5;
    //rdroid_curColorEffects.tint.z = 0.5;

#if 0
    //if (rdroid_curColorEffects.filter.x || rdroid_curColorEffects.filter.y || rdroid_curColorEffects.filter.z)
    //if (rdroid_curColorEffects.tint.x || rdroid_curColorEffects.tint.y || rdroid_curColorEffects.tint.z)
    if (rdroid_curColorEffects.add.x || rdroid_curColorEffects.add.y || rdroid_curColorEffects.add.z)
    {
        stdPlatform_Printf("a %f %f %f ", rdroid_curColorEffects.tint.x, rdroid_curColorEffects.tint.y, rdroid_curColorEffects.tint.z);
        stdPlatform_Printf("%d %d %d ", rdroid_curColorEffects.filter.x, rdroid_curColorEffects.filter.y, rdroid_curColorEffects.filter.z);
        stdPlatform_Printf("%d %d %d ", rdroid_curColorEffects.add.x, rdroid_curColorEffects.add.y, rdroid_curColorEffects.add.z);
        stdPlatform_Printf("%f\n", rdroid_curColorEffects.fade);
    }
#endif

    glUniform3f(uniform_tint, rdroid_curColorEffects.tint.x, rdroid_curColorEffects.tint.y, rdroid_curColorEffects.tint.z);
    if (rdroid_curColorEffects.filter.x || rdroid_curColorEffects.filter.y || rdroid_curColorEffects.filter.z)
        glUniform3f(uniform_filter, rdroid_curColorEffects.filter.x ? 1.0 : 0.25, rdroid_curColorEffects.filter.y ? 1.0 : 0.25, rdroid_curColorEffects.filter.z ? 1.0 : 0.25);
    else
        glUniform3f(uniform_filter, 1.0, 1.0, 1.0);
    glUniform1f(uniform_fade, rdroid_curColorEffects.fade);
    glUniform3f(uniform_add, (float)rdroid_curColorEffects.add.x / 255.0f, (float)rdroid_curColorEffects.add.y / 255.0f, (float)rdroid_curColorEffects.add.z / 255.0f);
    glUniform3f(uniform_emissiveFactor, 0.0, 0.0, 0.0);
    glUniform4f(uniform_albedoFactor, 1.0, 1.0, 1.0, 1.0);
    glUniform1f(uniform_light_mult, jkGuiBuildMulti_bRendering ? 0.85 : (jkPlayer_enableBloom ? 0.9 : 0.85));
    glUniform1f(uniform_displacement_factor, 1.0);

    rdTri* tris = GL_tmpTris;
    rdLine* lines = GL_tmpLines;
    
    //glEnableVertexAttribArray(attribute_v_norm);

    int last_tex_idx = 0;
    //GLushort* world_data_elements = malloc(sizeof(GLushort) * 3 * GL_tmpTrisAmt);
    for (int j = 0; j < GL_tmpTrisAmt; j++)
    {
        world_data_elements[(j*3)+0] = tris[j].v1;
        world_data_elements[(j*3)+1] = tris[j].v2;
        world_data_elements[(j*3)+2] = tris[j].v3;
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, world_ibo_triangle);
    std3D_uploadBuffer(world_ibo_triangle, GL_ELEMENT_ARRAY_BUFFER, GL_tmpTrisAmt * 3 * sizeof(GLushort), world_data_elements, &std3D_worldIboCap);
    
    int do_batch = 0;
    
    //glDepthFunc(GL_LESS);
    //glDepthMask(GL_TRUE);
    //glCullFace(GL_FRONT);

    if (last_tex) {
        std3D_DoTex(last_tex, &tris[0], GL_tmpTrisAmt);
    }

    last_flags = tris[0].flags;

    if (last_flags & 0x800) {
        glDepthFunc(GL_LESS);
        //glClear(GL_DEPTH_BUFFER_BIT);
    }
    else {
        glDepthFunc(GL_ALWAYS);
    }

    if (last_flags & 0x600) {
        
        if (last_flags & 0x200) {
            glUniform1i(uniform_blend_mode, D3DBLEND_INVSRCALPHA);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        else {
            glUniform1i(uniform_blend_mode, D3DBLEND_SRCALPHA);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
    }
    else {
        glUniform1i(uniform_blend_mode, D3DBLEND_ONE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    if (last_flags & 0x1000)
    {
        glDepthMask(GL_TRUE);
    }
    else
    {
        glDepthMask(GL_FALSE);
    }

    if (last_flags & 0x10000) {
        glCullFace(GL_BACK);
    }
    else
    {
        glCullFace(GL_FRONT);
    }
    
    for (int j = 0; j < GL_tmpTrisAmt; j++)
    {
        if (tris[j].texture != last_tex || tris[j].flags != last_flags)
        {
            do_batch = 1;
        }
        
        if (do_batch)
        {
            int num_tris_batch = j - last_tex_idx;
            rdDDrawSurface* tex = tris[j].texture;


            
            test_idk = tex;

            if (num_tris_batch)
            {
                //printf("batch %u~%u\n", last_tex_idx, j);
                glDrawElements(GL_TRIANGLES, num_tris_batch * 3, GL_UNSIGNED_SHORT, (GLvoid*)((intptr_t)&world_data_elements[last_tex_idx * 3] - (intptr_t)&world_data_elements[0]));
            }

            std3D_DoTex(tex, &tris[j], GL_tmpTrisAmt-j);
            
            int changed_flags = (last_flags ^ tris[j].flags);

            if (changed_flags & 0x600)
            {
                if (tris[j].flags & 0x600) {
                    
                    if (tris[j].flags & 0x200) {
                        glUniform1i(uniform_blend_mode, D3DBLEND_INVSRCALPHA);
                        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    }
                    else {
                        //printf ("flags %x\n", tris[j].flags);
                        glUniform1i(uniform_blend_mode, D3DBLEND_SRCALPHA);
                        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    }
                }
                else {
                    glUniform1i(uniform_blend_mode, D3DBLEND_ONE);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                }
            }
            
            if (changed_flags & 0x1800)
            {
                if (tris[j].flags & 0x800)
                {
                    glDepthFunc(GL_LESS);
                }
                else
                {
                    glDepthFunc(GL_ALWAYS);
                    //glClear(GL_DEPTH_BUFFER_BIT);
                }
                
                if (tris[j].flags & 0x1000)
                {
                    glDepthMask(GL_TRUE);
                }
                else
                {
                    glDepthMask(GL_FALSE);
                }
            }

            if (changed_flags & 0x10000)
            {
                if (tris[j].flags & 0x10000) {
                    glCullFace(GL_BACK);
                }
                else
                {
                    glCullFace(GL_FRONT);
                }
            }
            
            last_tex = tris[j].texture;
            last_flags = tris[j].flags;
            last_tex_idx = j;

            do_batch = 0;
        }
        //printf("tri %u: %u,%u,%u, flags %x\n", j, tris[j].v1, tris[j].v2, tris[j].v3, tris[j].flags);
        
        
        /*int vert = tris[j].v1;
        stdPlatform_Printf("%u: %f %f %f, %f %f %f, %f %f\n", vert, vertexes[vert].x, vertexes[vert].y, vertexes[vert].z,
                                      vertexes[vert].nx, vertexes[vert].ny, vertexes[vert].nz,
                                      vertexes[vert].tu, vertexes[vert].tv);
        
        vert = tris[j].v2;
        stdPlatform_Printf("%u: %f %f %f, %f %f %f, %f %f\n", vert, vertexes[vert].x, vertexes[vert].y, vertexes[vert].z,
                                      vertexes[vert].nx, vertexes[vert].ny, vertexes[vert].nz,
                                      vertexes[vert].tu, vertexes[vert].tv);
        
        vert = tris[j].v3;
        stdPlatform_Printf("%u: %f %f %f, %f %f %f, %f %f\n", vert, vertexes[vert].x, vertexes[vert].y, vertexes[vert].z,
                                      vertexes[vert].nx, vertexes[vert].ny, vertexes[vert].nz,
                                      vertexes[vert].tu, vertexes[vert].tv);*/
    }
    
    int remaining_batch = GL_tmpTrisAmt - last_tex_idx;

    if (remaining_batch)
    {
        glDrawElements(GL_TRIANGLES, remaining_batch * 3, GL_UNSIGNED_SHORT, (GLvoid*)((intptr_t)&world_data_elements[last_tex_idx * 3] - (intptr_t)&world_data_elements[0]));
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    
    
#if 0
    // Draw all lines
    world_data_elements = malloc(sizeof(GLushort) * 2 * GL_tmpLinesAmt);
    for (int j = 0; j < GL_tmpLinesAmt; j++)
    {
        world_data_elements[(j*2)+0] = lines[j].v1;
        world_data_elements[(j*2)+1] = lines[j].v2;
    }
    
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, world_ibo_triangle);
    std3D_uploadBuffer(world_ibo_triangle, GL_ELEMENT_ARRAY_BUFFER, GL_tmpLinesAmt * 2 * sizeof(GLushort), world_data_elements, &std3D_worldIboCap);

    int lines_size;
    glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &lines_size);
    glDrawElements(GL_LINES, lines_size / sizeof(GLushort), GL_UNSIGNED_SHORT, 0);
#endif
        
    // Done drawing    
    glBindTexture(GL_TEXTURE_2D, worldpal_texture);
    glCullFace(GL_FRONT);
    
    std3D_ResetRenderList();
}

int std3D_SetCurrentPalette(rdColor24 *a1, int a2)
{
    return 1;
}

void std3D_GetValidDimension(unsigned int inW, unsigned int inH, unsigned int *outW, unsigned int *outH)
{
    // TODO hack for JKE? I don't know what they're doing
    *outW = inW > 256 ? 256 : inW;
    *outH = inH > 256 ? 256 : inH;
}

int std3D_DrawOverlay()
{
    return 1;
}

void std3D_UnloadAllTextures()
{
#ifndef SDL2_RENDER
    if (!Main_bHeadless) {
        glDeleteTextures(std3D_loadedTexturesAmt, std3D_aLoadedTextures);
    }
    std3D_loadedTexturesAmt = 0;
#else
    std3D_UpdateSettings();
#endif
}

void std3D_AddRenderListTris(rdTri *tris, unsigned int num_tris)
{
    if (Main_bHeadless) return;

    if (GL_tmpTrisAmt + num_tris > STD3D_MAX_TRIS)
    {
        return;
    }
    
    memcpy(&GL_tmpTris[GL_tmpTrisAmt], tris, sizeof(rdTri) * num_tris);
    
    GL_tmpTrisAmt += num_tris;
}

void std3D_AddRenderListLines(rdLine* lines, uint32_t num_lines)
{
    if (Main_bHeadless) return;

    if (GL_tmpLinesAmt + num_lines > STD3D_MAX_VERTICES)
    {
        return;
    }
    
    memcpy(&GL_tmpLines[GL_tmpLinesAmt], lines, sizeof(rdLine) * num_lines);
    GL_tmpLinesAmt += num_lines;
}

int std3D_AddRenderListVertices(D3DVERTEX *vertices, int count)
{
    if (Main_bHeadless) return 1;

    if (GL_tmpVerticesAmt + count >= STD3D_MAX_VERTICES)
    {
        return 0;
    }
    
    memcpy(&GL_tmpVertices[GL_tmpVerticesAmt], vertices, sizeof(D3DVERTEX) * count);
    
    GL_tmpVerticesAmt += count;
    
    return 1;
}

void std3D_AddRenderListUITris(rdUITri *tris, unsigned int num_tris)
{
    if (Main_bHeadless) return;

    if (GL_tmpUITrisAmt + num_tris > STD3D_MAX_TRIS)
    {
        return;
    }
    
    memcpy(&GL_tmpUITris[GL_tmpUITrisAmt], tris, sizeof(rdUITri) * num_tris);
    
    GL_tmpUITrisAmt += num_tris;
}

int std3D_ClearZBuffer()
{
    glDepthMask(GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->fbo);
    glClear(GL_DEPTH_BUFFER_BIT);
    return 1;
}

#if defined(TARGET_LINUX_GLES)
static int std3D_glesIsRgb565(stdVBuffer *vbuf)
{
    return vbuf->format.format.r_bits == 5
        && vbuf->format.format.g_bits == 6
        && vbuf->format.format.b_bits == 5;
}

static int std3D_glesIsRgb1555(stdVBuffer *vbuf)
{
    return vbuf->format.format.r_bits == 5
        && vbuf->format.format.g_bits == 5
        && vbuf->format.format.b_bits == 5;
}

static int std3D_glesTryUpload16bppNative(stdVBuffer *vbuf, uint32_t width, uint32_t height, void *pixels, int is_alpha_tex)
{
    uint32_t row_stride_pixels = vbuf->format.width_in_bytes / 2;
    GLenum internal_format;
    GLenum format;
    GLenum pixel_types[2];
    int num_pixel_types;
    int i;

    if (!pixels || !width || !height || row_stride_pixels < width) {
        return 0;
    }

    if (!is_alpha_tex && std3D_glesIsRgb565(vbuf)) {
        internal_format = GL_RGB8;
        format = GL_RGB;
        pixel_types[0] = GL_UNSIGNED_SHORT_5_6_5_REV;
        pixel_types[1] = GL_UNSIGNED_SHORT_5_6_5;
        num_pixel_types = 2;
    } else if (is_alpha_tex || std3D_glesIsRgb1555(vbuf)) {
        internal_format = GL_RGBA8;
        format = GL_RGBA;
        pixel_types[0] = GL_UNSIGNED_SHORT_1_5_5_5_REV;
        pixel_types[1] = GL_UNSIGNED_SHORT_5_5_5_1;
        num_pixel_types = 2;
    } else {
        return 0;
    }

    if (row_stride_pixels != width) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, row_stride_pixels);
    }

    for (i = 0; i < num_pixel_types; i++) {
        while (glGetError() != GL_NO_ERROR) {}
        glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, pixel_types[i], pixels);
        if (glGetError() == GL_NO_ERROR) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            return 1;
        }
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    return 0;
}

static void* std3D_glesConvert16bppToRgba8(stdVBuffer *vbuf, uint32_t width, uint32_t height, uint16_t *image_16bpp)
{
    uint32_t row_stride = vbuf->format.width_in_bytes / 2;
    uint32_t tex_row_stride = width;
    void* image_data = malloc(width * height * 4);
    if (!image_data) {
        return NULL;
    }
    memset(image_data, 0, width * height * 4);

    for (uint32_t j = 0; j < height; j++)
    {
        for (uint32_t i = 0; i < width; i++)
        {
            uint32_t src_index = (j * row_stride) + i;
            uint32_t tex_index = (j * tex_row_stride) + i;
            uint32_t val_rgba = 0x00000000;
            uint16_t val = image_16bpp[src_index];

            if (std3D_glesIsRgb565(vbuf))
            {
                uint8_t val_a8 = 0xFF;
                uint8_t val_r5 = (val >> 11) & 0x1F;
                uint8_t val_g6 = (val >> 5) & 0x3F;
                uint8_t val_b5 = (val >> 0) & 0x1F;
                uint8_t val_r8 = (val_r5 * 527 + 23) >> 6;
                uint8_t val_g8 = (val_g6 * 259 + 33) >> 6;
                uint8_t val_b8 = (val_b5 * 527 + 23) >> 6;

                if (vbuf->transparent_color) {
                    uint8_t transparent_r5 = (vbuf->transparent_color >> 11) & 0x1F;
                    uint8_t transparent_g6 = (vbuf->transparent_color >> 5) & 0x3F;
                    uint8_t transparent_b5 = (vbuf->transparent_color >> 0) & 0x1F;
                    if (val_r5 == transparent_r5 && val_g6 == transparent_g6 && val_b5 == transparent_b5) {
                        val_a8 = 0;
                    }
                }

                val_rgba |= (val_a8 << 24);
                val_rgba |= (val_b8 << 16);
                val_rgba |= (val_g8 << 8);
                val_rgba |= (val_r8 << 0);
            }
            else if (std3D_glesIsRgb1555(vbuf))
            {
                uint8_t val_a1 = (val >> 15);
                uint8_t val_r5 = (val >> 10) & 0x1F;
                uint8_t val_g5 = (val >> 5) & 0x1F;
                uint8_t val_b5 = (val >> 0) & 0x1F;
                uint8_t val_a8 = val_a1 ? 0xFF : 0x0;
                uint8_t val_r8 = (val_r5 * 527 + 23) >> 6;
                uint8_t val_g8 = (val_g5 * 527 + 23) >> 6;
                uint8_t val_b8 = (val_b5 * 527 + 23) >> 6;

                val_rgba |= (val_a8 << 24);
                val_rgba |= (val_b8 << 16);
                val_rgba |= (val_g8 << 8);
                val_rgba |= (val_r8 << 0);
            }

            ((uint32_t*)image_data)[tex_index] = val_rgba;
        }
    }

    return image_data;
}
#endif

int std3D_AddToTextureCache(stdVBuffer *vbuf, rdDDrawSurface *texture, int is_alpha_tex, int no_alpha)
{
    int i;

    if (Main_bHeadless) return 1;
    if (!vbuf || !texture) return 1;
    if (texture->texture_loaded) {
        for (i = 0; i < std3D_loadedTexturesAmt; i++) {
            if (std3D_aLoadedSurfaces[i] == texture)
                return 1;
        }
        texture->texture_loaded = 0;
        texture->texture_id = 0;
    }

#if defined(TARGET_LINUX_GLES)
    if (!has_initted) {
        openjkdf2_trace("std3D_AddToTextureCache: GL not initted yet");
        return 0;
    }
    if (!std3D_EnsureGLContext()) {
        openjkdf2_trace("std3D_AddToTextureCache: no GL context");
        return 0;
    }
#endif

    if (std3D_loadedTexturesAmt >= STD3D_MAX_TEXTURES) {
        stdPlatform_Printf("ERROR: Texture cache exhausted!! Ask ShinyQuagsire to increase the size.\n");
#if defined(TARGET_LINUX_GLES)
        if (!std3D_GlesRecoverTextureCacheSlots()) {
            return 0;
        }
#else
        return 1;
#endif
    }
    //printf("Add to texture cache\n");
    
    GLuint image_texture = 0;
    glGenTextures(1, &image_texture);
#if defined(TARGET_LINUX_GLES)
    if (!image_texture) {
#if defined(RDMATERIAL_LRU_LOAD_UNLOAD)
        if (rdMaterial_PurgeMaterialCache())
            glGenTextures(1, &image_texture);
#endif
        if (!image_texture) {
            std3D_RequestDeferredTexturePurge();
            return 0;
        }
    }
#endif
    uint8_t* image_8bpp = stdDisplay_VBufferPixels(vbuf);
    uint16_t* image_16bpp = (uint16_t*)image_8bpp;
    uint8_t* pal = (uint8_t*)vbuf->palette;
    
    uint32_t width, height;
    width = vbuf->format.width;
    height = vbuf->format.height;

    if (!image_8bpp) {
        return 1;
    }

    glBindTexture(GL_TEXTURE_2D, image_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    //glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    //glPixelStorei(GL_PACK_ALIGNMENT, 1);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    if (jkPlayer_enableTextureFilter && texture->is_16bit)
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    else
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    }

    if (vbuf->format.format.is16bit)
    {
        texture->is_16bit = 1;
#if defined(TARGET_LINUX_GLES)
        {
            void* image_data = NULL;
            int use_native = 0;

            if (std3D_glesIsRgb565(vbuf) && !vbuf->transparent_color) {
                use_native = std3D_glesTryUpload16bppNative(vbuf, width, height, image_8bpp, 0);
            } else if (is_alpha_tex || std3D_glesIsRgb1555(vbuf)) {
                use_native = std3D_glesTryUpload16bppNative(vbuf, width, height, image_8bpp, 1);
            }

            if (!use_native) {
                image_data = std3D_glesConvert16bppToRgba8(vbuf, width, height, image_16bpp);
                if (!image_data) {
#if defined(TARGET_LINUX_GLES)
                    std3D_RequestDeferredTexturePurge();
#endif
                    return 0;
                }
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data);
#if defined(TARGET_LINUX_GLES)
                if (!std3D_GlesCheckUploadError(&image_texture, "16bpp rgba upload")) {
                    free(image_data);
                    return 0;
                }
#endif
            }

            texture->pDataDepthConverted = image_data;
        }
#else
        if (!is_alpha_tex)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0,  GL_RGB, GL_UNSIGNED_SHORT_5_6_5_REV, image_8bpp);
        else
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,  GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, image_8bpp);
#endif

#ifdef __NOTDEF_FORMAT_CONVERSION
        void* image_data = malloc(width*height*4);
    
        for (int j = 0; j < height; j++)
        {
            for (int i = 0; i < width; i++)
            {
                uint32_t index = (i*height) + j;
                uint32_t val_rgba = 0x00000000;
                
                uint16_t val = image_16bpp[index];
                if (!is_alpha_tex) // RGB565
                {
                    uint8_t val_a1 = 1;
                    uint8_t val_r5 = (val >> 11) & 0x1F;
                    uint8_t val_g6 = (val >> 5) & 0x3F;
                    uint8_t val_b5 = (val >> 0) & 0x1F;

                    uint8_t val_a8 = val_a1 ? 0xFF : 0x0;
                    uint8_t val_r8 = ( val_r5 * 527 + 23 ) >> 6;
                    uint8_t val_g8 = ( val_g6 * 259 + 33 ) >> 6;
                    uint8_t val_b8 = ( val_b5 * 527 + 23 ) >> 6;

#ifdef __NOTDEF_TRANSPARENT_BLACK
                    uint8_t transparent_r8 = (vbuf->transparent_color >> 16) & 0xFF;
                    uint8_t transparent_g8 = (vbuf->transparent_color >> 8) & 0xFF;
                    uint8_t transparent_b8 = (vbuf->transparent_color >> 0) & 0xFF;

                    if (val_r8 == transparent_r8 && val_g8 == transparent_g8 && val_b8 == transparent_b8) {
                        val_a1 = 0;
                    }
#endif // __NOTDEF_TRANSPARENT_BLACK

                    val_rgba |= (val_a8 << 24);
                    val_rgba |= (val_b8 << 16);
                    val_rgba |= (val_g8 << 8);
                    val_rgba |= (val_r8 << 0);
                }
                else // RGB1555
                {
                    uint8_t val_a1 = (val >> 15);
                    uint8_t val_r5 = (val >> 10) & 0x1F;
                    uint8_t val_g5 = (val >> 5) & 0x1F;
                    uint8_t val_b5 = (val >> 0) & 0x1F;

                    uint8_t val_a8 = val_a1 ? 0xFF : 0x0;
                    uint8_t val_r8 = ( val_r5 * 527 + 23 ) >> 6;
                    uint8_t val_g8 = ( val_g5 * 527 + 23 ) >> 6;
                    uint8_t val_b8 = ( val_b5 * 527 + 23 ) >> 6;

                    val_rgba |= (val_a8 << 24);
                    val_rgba |= (val_b8 << 16);
                    val_rgba |= (val_g8 << 8);
                    val_rgba |= (val_r8 << 0);
                }
                    
                *(uint32_t*)(image_data + index*4) = val_rgba;
            }
        }
        
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, image_data);

        texture->pDataDepthConverted = image_data;
#endif // __NOTDEF_FORMAT_CONVERSION
    }
    else {
#if 0
        void* image_data = malloc(width*height*4);
    
        for (int j = 0; j < height; j++)
        {
            for (int i = 0; i < width; i++)
            {
                uint32_t index = (i*height) + j;
                uint32_t val_rgba = 0xFF000000;
                
                if (pal)
                {
                    uint8_t val = image_8bpp[index];
                    val_rgba |= (pal[(val * 3) + 2] << 16);
                    val_rgba |= (pal[(val * 3) + 1] << 8);
                    val_rgba |= (pal[(val * 3) + 0] << 0);
                }
                else
                {
                    uint8_t val = image_8bpp[index];
                    rdColor24* pal_master = (rdColor24*)sithWorld_pCurrentWorld->colormaps->colors;//stdDisplay_gammaPalette;
                    rdColor24* color = &pal_master[val];
                    val_rgba |= (color->r << 16);
                    val_rgba |= (color->g << 8);
                    val_rgba |= (color->b << 0);
                }
                
                *(uint32_t*)(image_data + index*4) = val_rgba;
            }
        }
        
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data);

        texture->pDataDepthConverted = image_data;
#endif
        texture->is_16bit = 0;
#if defined(TARGET_LINUX_GLES)
        if (vbuf->format.width_in_bytes != width) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, vbuf->format.width_in_bytes);
        }
#endif
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, image_8bpp);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#if defined(TARGET_LINUX_GLES)
        if (!std3D_GlesCheckUploadError(&image_texture, "8bpp upload")) {
            return 0;
        }
#endif

        texture->pDataDepthConverted = NULL;
    }

    
    std3D_aLoadedSurfaces[std3D_loadedTexturesAmt] = texture;
    std3D_aLoadedTextures[std3D_loadedTexturesAmt++] = image_texture;
    /*ext->surfacebuf = image_data;
    ext->surfacetex = image_texture;
    ext->surfacepaltex = pal_texture;*/
    
    texture->texture_id = image_texture;
    texture->emissive_texture_id = 0;
    texture->displacement_texture_id = 0;
    texture->texture_loaded = 1;
    texture->emissive_factor[0] = 0.0;
    texture->emissive_factor[1] = 0.0;
    texture->emissive_factor[2] = 0.0;
    texture->albedo_factor[0] = 1.0;
    texture->albedo_factor[1] = 1.0;
    texture->albedo_factor[2] = 1.0;
    texture->albedo_factor[3] = 1.0;
    texture->displacement_factor = 0.0;
    texture->albedo_data = NULL;
    texture->displacement_data = NULL;
    texture->emissive_data = NULL;

    glBindTexture(GL_TEXTURE_2D, blank_tex);
    
    return 1;
}

int std3D_GetBitmapCacheIdx()
{
    for (int i = 0; i < STD3D_MAX_TEXTURES; i++)
    {
        if (!std3D_aUIBitmaps[i]) {
            return i;
        }
    }
    return -1;
}

int std3D_AddBitmapToTextureCache(stdBitmap *texture, int mipIdx, int is_alpha_tex, int no_alpha)
{
    if (Main_bHeadless) return 1;
    if (!has_initted) return 0;
#if defined(TARGET_LINUX_GLES)
    if (!std3D_EnsureGLContext()) {
        openjkdf2_trace("std3D_AddBitmapToTextureCache: no GL context");
        return 0;
    }
#endif
    if (!texture) return 1;
    if (mipIdx >= texture->numMips) return 1;
    if (!texture->abLoadedToGPU || texture->abLoadedToGPU[mipIdx]) return 1;

    stdVBuffer *vbuf = texture->mipSurfaces[mipIdx];
     if (!vbuf) return 1;

    int cacheIdx = std3D_GetBitmapCacheIdx();
    if (cacheIdx < 0) {
        stdPlatform_Printf("ERROR: Texture cache exhausted!! Ask ShinyQuagsire to increase the size.\n");
#if defined(TARGET_LINUX_GLES)
        if (!std3D_GlesRecoverTextureCacheSlots()) {
            return 0;
        }
        cacheIdx = std3D_GetBitmapCacheIdx();
        if (cacheIdx < 0) {
            return 0;
        }
#else
        return 1;
#endif
    }
    //printf("Add to texture cache\n");
    
    GLuint image_texture;
    glGenTextures(1, &image_texture);
    uint8_t* image_8bpp = stdDisplay_VBufferPixels(vbuf);
    uint16_t* image_16bpp = (uint16_t*)image_8bpp;
    uint8_t* pal = (uint8_t*)vbuf->palette;
    
    uint32_t width, height;
    width = vbuf->format.width;
    height = vbuf->format.height;

    if (!image_8bpp) {
        return 1;
    }

    glBindTexture(GL_TEXTURE_2D, image_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    //glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    //glPixelStorei(GL_PACK_ALIGNMENT, 1);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    /*if (jkPlayer_enableTextureFilter && texture->is_16bit)
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    else*/
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    }

    if (vbuf->format.format.is16bit || texture->format.bpp == 16)
    {
        texture->is_16bit = 1;

#if defined(TARGET_LINUX_GLES)
        {
            void* image_data = std3D_glesConvert16bppToRgba8(vbuf, width, height, image_16bpp);
            if (!image_data) {
#if defined(TARGET_LINUX_GLES)
                std3D_RequestDeferredTexturePurge();
#endif
                return 0;
            }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data);
#if defined(TARGET_LINUX_GLES)
            if (!std3D_GlesCheckUploadError(&image_texture, "bitmap rgba upload")) {
                free(image_data);
                return 0;
            }
#endif
            texture->paDataDepthConverted[mipIdx] = image_data;
        }
#else
        uint32_t tex_width, tex_height, tex_row_stride;
        uint32_t row_stride = vbuf->format.width_in_bytes / 2;
        tex_width = width;
        tex_height = height;
        tex_row_stride = width;

        void* image_data = malloc(tex_width*tex_height*4);
        memset(image_data, 0, tex_width*tex_height*4);

        for (int j = 0; j < height; j++)
        {
            for (int i = 0; i < width; i++)
            {
                uint32_t index = (j*row_stride) + i;
                uint32_t tex_index = (j*tex_row_stride) + i;
                uint32_t val_rgba = 0x00000000;

                uint16_t val = image_16bpp[index];
                if (vbuf->format.format.r_bits == 5 && vbuf->format.format.g_bits == 6 && vbuf->format.format.b_bits == 5)
                {
                    uint8_t val_a8 = 0xFF;
                    uint8_t val_r5 = (val >> 11) & 0x1F;
                    uint8_t val_g6 = (val >> 5) & 0x3F;
                    uint8_t val_b5 = (val >> 0) & 0x1F;
                    uint8_t val_r8 = ( val_r5 * 527 + 23 ) >> 6;
                    uint8_t val_g8 = ( val_g6 * 259 + 33 ) >> 6;
                    uint8_t val_b8 = ( val_b5 * 527 + 23 ) >> 6;

                    if (vbuf->transparent_color) {
                        uint8_t transparent_r5 = (vbuf->transparent_color >> 11) & 0x1F;
                        uint8_t transparent_g6 = (vbuf->transparent_color >> 5) & 0x3F;
                        uint8_t transparent_b5 = (vbuf->transparent_color >> 0) & 0x1F;
                        if (val_r5 == transparent_r5 && val_g6 == transparent_g6 && val_b5 == transparent_b5) {
                            val_a8 = 0;
                        }
                    }

                    val_rgba |= (val_a8 << 24);
                    val_rgba |= (val_b8 << 16);
                    val_rgba |= (val_g8 << 8);
                    val_rgba |= (val_r8 << 0);
                }
                else if (vbuf->format.format.r_bits == 5 && vbuf->format.format.g_bits == 5 && vbuf->format.format.b_bits == 5)
                {
                    uint8_t val_a1 = (val >> 15);
                    uint8_t val_r5 = (val >> 10) & 0x1F;
                    uint8_t val_g5 = (val >> 5) & 0x1F;
                    uint8_t val_b5 = (val >> 0) & 0x1F;
                    uint8_t val_a8 = val_a1 ? 0xFF : 0x0;
                    uint8_t val_r8 = ( val_r5 * 527 + 23 ) >> 6;
                    uint8_t val_g8 = ( val_g5 * 527 + 23 ) >> 6;
                    uint8_t val_b8 = ( val_b5 * 527 + 23 ) >> 6;

                    val_rgba |= (val_a8 << 24);
                    val_rgba |= (val_b8 << 16);
                    val_rgba |= (val_g8 << 8);
                    val_rgba |= (val_r8 << 0);
                }

                ((uint32_t*)image_data)[tex_index] = val_rgba;
            }
        }

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tex_width, tex_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data);
        texture->paDataDepthConverted[mipIdx] = image_data;
#endif
    }
    else {
        texture->is_16bit = 0;
#if 1
        uint32_t tex_width, tex_height, tex_row_stride;
        uint32_t row_stride = vbuf->format.width_in_bytes;
        tex_width = width;//vbuf->format.width_in_bytes / 2;
        tex_height = height;
        tex_row_stride = width;

        void* image_data = malloc(tex_width*tex_height*4);
        memset(image_data, 0, tex_width*tex_height*4);

        void* palette_data = texture->palette;//displaypal_data;

        if (!palette_data) 
        {
            palette_data = std3D_ui_colormap.colors;//jkGui_stdBitmaps[2]->palette;
            pal = NULL;//palette_data;
        }
        else {
            pal = NULL;//texture->palette;
        }
    
        for (int j = 0; j < height; j++)
        {
            for (int i = 0; i < width; i++)
            {
                uint32_t index = (j*row_stride) + i;
                uint32_t tex_index = (j*tex_row_stride) + i;
                uint32_t val_rgba = 0x00000000;
                
                if (pal)
                {
                    uint8_t val = image_8bpp[index];
                    val_rgba |= (pal[(val * 3) + 2] << 16);
                    val_rgba |= (pal[(val * 3) + 1] << 8);
                    val_rgba |= (pal[(val * 3) + 0] << 0);
                    val_rgba |= (0xFF << 24);

                    if (!val) {
                        val_rgba = 0;
                    }
                }
                else
                {
                    uint8_t val = image_8bpp[index];
#if 0
                    if (sithWorld_pCurrentWorld && sithWorld_pCurrentWorld->colormaps && sithWorld_pCurrentWorld->colormaps->colors)
                    {
                        rdColor24* pal_master = (rdColor24*)sithWorld_pCurrentWorld->colormaps->colors;//stdDisplay_gammaPalette;
                        rdColor24* color = &pal_master[val];
                        val_rgba |= (color->r << 16);
                        val_rgba |= (color->g << 8);
                        val_rgba |= (color->b << 0);
                        val_rgba |= (0xFF << 24);
                        stdPlatform_Printf("%x %x\n", val_rgba, val);
                    }
                    else {
                        val_rgba = 0xFFFFFFFF; // HACK
                    }
#endif

                    if (palette_data)
                    {
                        uint8_t color_r = ((uint8_t*)palette_data)[(val*3) + 0];
                        uint8_t color_g = ((uint8_t*)palette_data)[(val*3) + 1];
                        uint8_t color_b = ((uint8_t*)palette_data)[(val*3) + 2];

                        val_rgba |= (0xFF << 24);
                        val_rgba |= (color_b << 16);
                        val_rgba |= (color_g << 8);
                        val_rgba |= (color_r << 0);
                    }
                    else {
                        val_rgba = 0xFFFFFF00; // HACK
                    }
                    

                    if (!val) {
                        val_rgba = 0;
                    }
                }
                
                ((uint32_t*)image_data)[tex_index] = val_rgba;
            }
        }
        
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data);

        texture->paDataDepthConverted[mipIdx] = image_data;
#endif
        //glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, image_8bpp);
    }
    
    std3D_aUIBitmaps[cacheIdx] = texture;
    std3D_aUITextures[cacheIdx] = image_texture;
    std3D_loadedUITexturesAmt++;
    /*ext->surfacebuf = image_data;
    ext->surfacetex = image_texture;
    ext->surfacepaltex = pal_texture;*/
    
    texture->aTextureIds[mipIdx] = image_texture;
    texture->abLoadedToGPU[mipIdx] = 1;

    glBindTexture(GL_TEXTURE_2D, blank_tex);
    
    return 1;
}

void std3D_UpdateFrameCount(rdDDrawSurface *pTexture) {
    //pTexture->frameNum = std3D_frameCount; // lol LEC bug
    std3D_RemoveTextureFromCacheList(pTexture);
    std3D_AddTextureToCacheList(pTexture);
    pTexture->frameNum = std3D_frameCount;
}
void std3D_RemoveTextureFromCacheList(rdDDrawSurface *surface) {
}
void std3D_AddTextureToCacheList(rdDDrawSurface *surface) {
}

// Added helpers
void std3D_UpdateSettings()
{
#if defined(TARGET_LINUX_GLES)
    if (!std3D_IsReady() || !std3D_EnsureGLContext())
        return;
#endif
    jk_printf("Updating texture cache...\n");
    for (int i = 0; i < STD3D_MAX_TEXTURES; i++)
    {
        rdDDrawSurface* tex = std3D_aLoadedSurfaces[i];
        if (!tex) continue;

        if (!std3D_aLoadedTextures[i]) continue;
        glBindTexture(GL_TEXTURE_2D, std3D_aLoadedTextures[i]);

        if (jkPlayer_enableTextureFilter && tex->is_16bit)
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        }
        else
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        }

        if (tex->emissive_texture_id != 0) {
            glBindTexture(GL_TEXTURE_2D, tex->emissive_texture_id);
            
            if (jkPlayer_enableTextureFilter)
            {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            }
            else
            {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            }
        }

        if (tex->displacement_texture_id != 0) {
            glBindTexture(GL_TEXTURE_2D, tex->displacement_texture_id);
            
            if (jkPlayer_enableTextureFilter)
            {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            }
            else
            {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            }
        }
    }

#if 0
    for (int i = 0; i < STD3D_MAX_TEXTURES; i++)
    {
        if (!std3D_aUITextures[i]) continue;
        glBindTexture(GL_TEXTURE_2D, std3D_aUITextures[i]);

        if (jkPlayer_enableTextureFilter)
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        }
        else
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        }
        
    }
#endif

    glBindTexture(GL_TEXTURE_2D, blank_tex);
}

// Added
void std3D_Screenshot(const char* pFpath)
{
#ifdef TARGET_CAN_JKGM
    if (!std3D_pFb) return;

    uint8_t* data = (uint8_t*)malloc(std3D_pFb->w * std3D_pFb->h * 3 * sizeof(uint8_t));
    glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->fbo);
    glReadPixels(0, 0, std3D_pFb->w, std3D_pFb->h, GL_RGB, GL_UNSIGNED_BYTE, data);
    jkgm_write_png(pFpath, std3D_pFb->w, std3D_pFb->h, data);
    free(data);
#endif
}

int std3D_HasAlpha()
{
    return 1;
}

int std3D_HasModulateAlpha()
{
    return 1;
}

int std3D_HasAlphaFlatStippled()
{
    return 1;
}

void std3D_PurgeBitmapRefs(stdBitmap *pBitmap)
{
    for (int i = 0; i < STD3D_MAX_TEXTURES; i++)
    {
        int texId = std3D_aUITextures[i];
        stdBitmap* tex = std3D_aUIBitmaps[i];
        if (!tex) continue;
        if (tex != pBitmap) continue;

        for (int j = 0; j < tex->numMips; j++)
        {
            if (tex->aTextureIds[j] == texId) {
                std3D_PurgeUIEntry(i, j);
                break;
            }
        }
    }
}

void std3D_PurgeSurfaceRefs(rdDDrawSurface *texture)
{
    for (int i = 0; i < STD3D_MAX_TEXTURES; i++)
    {
        rdDDrawSurface* tex = std3D_aLoadedSurfaces[i];
        if (!tex) continue;
        if (tex != texture) continue;

        std3D_PurgeTextureEntry(i);
    }
}

void std3D_PurgeTextureEntry(int i) {
    if (std3D_aLoadedTextures[i]) {
        glDeleteTextures(1, &std3D_aLoadedTextures[i]);
        std3D_aLoadedTextures[i] = 0;
    }

    rdDDrawSurface* tex = std3D_aLoadedSurfaces[i];
    if (!tex) return;

    if (tex->pDataDepthConverted != NULL) {
        free(tex->pDataDepthConverted);
        tex->pDataDepthConverted = NULL;
    }

    if (tex->albedo_data != NULL) {
        //jkgm_aligned_free(tex->albedo_data);
        tex->albedo_data = NULL;
    }

    if (tex->emissive_data != NULL) {
        //jkgm_aligned_free(tex->emissive_data);
        tex->emissive_data = NULL;
    }

    if (tex->displacement_data != NULL) {
        //jkgm_aligned_free(tex->displacement_data);
        tex->displacement_data = NULL;
    }

    if (tex->emissive_texture_id != 0) {
        glDeleteTextures(1, &tex->emissive_texture_id);
        tex->emissive_texture_id = 0;
    }

    if (tex->displacement_texture_id != 0) {
        glDeleteTextures(1, &tex->displacement_texture_id);
        tex->displacement_texture_id = 0;
    }

    tex->emissive_factor[0] = 0.0;
    tex->emissive_factor[1] = 0.0;
    tex->emissive_factor[2] = 0.0;
    tex->albedo_factor[0] = 1.0;
    tex->albedo_factor[1] = 1.0;
    tex->albedo_factor[2] = 1.0;
    tex->albedo_factor[3] = 1.0;
    tex->displacement_factor = 0.0;

    tex->texture_loaded = 0;
    tex->texture_id = 0;

    std3D_aLoadedSurfaces[i] = NULL;
}

void std3D_PurgeUIEntry(int i, int idx) {
    if (std3D_aUITextures[i]) {
        glDeleteTextures(1, &std3D_aUITextures[i]);
        std3D_aUITextures[i] = 0;
    }

    stdBitmap* tex = std3D_aUIBitmaps[i];
    if (!tex) return;

    tex->abLoadedToGPU[idx] = 0;
    tex->aTextureIds[idx] = 0;
    free(tex->paDataDepthConverted[idx]);
    tex->paDataDepthConverted[idx] = NULL;

    std3D_aUIBitmaps[i] = NULL;
    std3D_loadedUITexturesAmt--;
}

// From https://github.com/smlu/OpenJones3D/blob/main/Libs/std/Win95/std3D.c
int std3D_PurgeTextureCache(size_t size)
{
    size_t purgedBytes = 0;
    for ( rdDDrawSurface* pCacheTexture = std3D_pFirstTexCache; pCacheTexture && pCacheTexture->frameNum != std3D_frameCount; pCacheTexture = pCacheTexture->pNextCachedTexture )
    {
        if ( pCacheTexture->textureSize == size )
        {
            //IDirect3DTexture2_Release(pCacheTexture->pD3DCachedTex);
            std3D_PurgeSurfaceRefs(pCacheTexture);
            //pCacheTexture->pD3DCachedTex = NULL;
            std3D_RemoveTextureFromCacheList(pCacheTexture);
            return 1;
        }
    }

    rdDDrawSurface* pNextCachedTexture = NULL;
    for ( rdDDrawSurface* pCacheTexture = std3D_pFirstTexCache; pCacheTexture && purgedBytes < size; pCacheTexture = pNextCachedTexture )
    {
        pNextCachedTexture = pCacheTexture->pNextCachedTexture;
        if ( pCacheTexture->frameNum != std3D_frameCount )
        {
            //if ( pCacheTexture->pD3DCachedTex ) { // Added: Added check for null pointer
                //IDirect3DTexture2_Release(pCacheTexture->pD3DCachedTex);
                std3D_PurgeSurfaceRefs(pCacheTexture);
            //}
            //pCacheTexture->pD3DCachedTex = NULL;
            purgedBytes += pCacheTexture->textureSize;
            std3D_RemoveTextureFromCacheList(pCacheTexture);
        }
    }

    return purgedBytes != 0;
}

void std3D_PurgeEntireTextureCache()
{
    if (Main_bHeadless) {
        std3D_loadedTexturesAmt = 0;
        return;
    }

#if defined(TARGET_LINUX_GLES)
    if (!has_initted || !std3D_EnsureGLContext()) {
        std3D_loadedTexturesAmt = 0;
        return;
    }
#endif

    if (!std3D_loadedTexturesAmt) {
        jk_printf("Skipping texture cache purge, nothing loaded.\n");
        return;
    }

    jk_printf("Purging texture cache... %x\n", std3D_loadedTexturesAmt);
    for (int i = 0; i < std3D_loadedTexturesAmt; i++)
    {
        std3D_PurgeTextureEntry(i);
    }
    std3D_loadedTexturesAmt = 0;
}

void std3D_InitializeViewport(rdRect *viewRect)
{
    signed int v1; // ebx
    signed int height; // ebp

    float viewXMax_2; // [esp+14h] [ebp+4h]
    float viewRectYMax; // [esp+14h] [ebp+4h]

    std3D_rectViewIdk.x = viewRect->x;
    v1 = viewRect->width;
    std3D_rectViewIdk.y = viewRect->y;
    std3D_rectViewIdk.width = v1;
    height = viewRect->height;
    memset(std3D_aViewIdk, 0, sizeof(std3D_aViewIdk));
    std3D_aViewIdk[0] = (float)std3D_rectViewIdk.x;
    std3D_aViewIdk[1] = (float)std3D_rectViewIdk.y;
    std3D_rectViewIdk.height = height;
    std3D_aViewTris[0].v1 = 0;
    std3D_aViewTris[0].v2 = 1;
    viewXMax_2 = (float)(v1 + std3D_rectViewIdk.x);
    std3D_aViewIdk[8] = viewXMax_2;
    std3D_aViewIdk[9] = std3D_aViewIdk[1];
    std3D_aViewIdk[16] = viewXMax_2;
    viewRectYMax = (float)(height + std3D_rectViewIdk.y);
    std3D_aViewTris[0].texture = 0;
    std3D_aViewIdk[17] = viewRectYMax;
    std3D_aViewIdk[25] = viewRectYMax;
    std3D_aViewIdk[24] = std3D_aViewIdk[0];
    std3D_aViewTris[0].v3 = 2;
    std3D_aViewTris[0].flags = 0x8200;
    std3D_aViewTris[1].v1 = 0;
    std3D_aViewTris[1].v2 = 2;
    std3D_aViewTris[1].v3 = 3;
    std3D_aViewTris[1].texture = 0;
    std3D_aViewTris[1].flags = 0x8200;
}

int std3D_GetValidDimensions(int a1, int a2, int a3, int a4)
{
    int result; // eax

    std3D_gpuMaxTexSizeMaybe = a1;
    result = a4;
    std3D_dword_53D66C = a2;
    std3D_dword_53D670 = a3;
    std3D_dword_53D674 = a4;
    return result;
}

int std3D_FindClosestDevice(uint32_t index, int a2)
{
    return 0;
}

int std3D_SetRenderList(intptr_t a1)
{
    std3D_renderList = a1;
    return std3D_CreateExecuteBuffer();
}

intptr_t std3D_GetRenderList()
{
    return std3D_renderList;
}

int std3D_CreateExecuteBuffer()
{
    return 1;
}

int std3D_IsReady()
{
    return has_initted;
}

void std3D_WarmupPipeline(int frames)
{
#if defined(SDL2_RENDER) || defined(TARGET_LINUX_GLES)
    int i;
    int saved_ddraw;

    if (Main_bHeadless || frames <= 0)
        return;
    if (openjkdf2_IsWorldLoading())
        return;
    if (!std3D_EnsureGLContext())
        return;

    saved_ddraw = jkGame_isDDraw;
    jkGame_isDDraw = 1;

    for (i = 0; i < frames; i++) {
        if (!std3D_StartScene())
            break;
        std3D_DrawSceneFbo();
        std3D_EndScene();
#if defined(TARGET_LINUX_GLES)
        if (displayWindow)
            SDL_GL_SwapWindow(displayWindow);
#endif
        glFinish();
    }

    jkGame_isDDraw = saved_ddraw;
    fprintf(stderr, "OpenJKDF2: GLES warmup (%d frame%s)\n", i, i == 1 ? "" : "s");
#endif
}