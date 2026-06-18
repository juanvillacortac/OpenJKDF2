#include "stdDisplay.h"
#include "Platform/gl_backend.h"

#include "stdPlatform.h"
#include "jk.h"
#include "Win95/Video.h"
#include "Win95/Window.h"
#include "General/stdColor.h"

void stdDisplay_SetGammaTable(int len, flex_d_t *table)
{
    stdDisplay_gammaTableLen = len;
    stdDisplay_paGammaTable = table;
}

uint8_t* stdDisplay_GetPalette()
{
    return (uint8_t*)stdDisplay_gammaPalette;
}

#ifndef SDL2_RENDER

#else
#include "SDL2_helper.h"
#include "Platform/trace_gles.h"
#include "Platform/std3D.h"
#include <assert.h>

uint8_t* stdDisplay_VBufferPixels(stdVBuffer *vbuf)
{
    if (!vbuf) {
        return NULL;
    }
    if (vbuf->sdlSurface) {
        return (uint8_t*)vbuf->sdlSurface->pixels;
    }
    return (uint8_t*)vbuf->surface_lock_alloc;
}

#if defined(TARGET_LINUX_GLES) || defined(OPENJKDF2_RUNTIME_GL)
static void stdDisplay_Free8bppBuffer(stdVBuffer *vbuf)
{
    if (!vbuf) {
        return;
    }
    if (vbuf->sdlSurface) {
        SDL_FreeSurface(vbuf->sdlSurface);
        vbuf->sdlSurface = NULL;
    }
    if (vbuf->surface_lock_alloc) {
        std_pHS->free(vbuf->surface_lock_alloc);
        vbuf->surface_lock_alloc = NULL;
    }
}

static int stdDisplay_Alloc8bppBuffer(stdVBuffer *vbuf, uint32_t w, uint32_t h)
{
    uint32_t size;

    if (!vbuf) {
        return 0;
    }

    stdDisplay_Free8bppBuffer(vbuf);

    size = w * h;
    vbuf->surface_lock_alloc = (char*)std_pHS->alloc(size);
    if (!vbuf->surface_lock_alloc) {
        openjkdf2_trace("stdDisplay_Alloc8bppBuffer: alloc failed");
        return 0;
    }
    memset(vbuf->surface_lock_alloc, 0, size);
    vbuf->sdlSurface = NULL;
    vbuf->format.width_in_bytes = w;
    vbuf->format.width_in_pixels = w;
    vbuf->format.width = w;
    vbuf->format.height = h;
    vbuf->format.format.bpp = 8;
    vbuf->format.texture_size_in_bytes = size;
    return 1;
}
#endif

uint32_t Video_menuTexId = 0;
uint32_t Video_overlayTexId = 0;
rdColor24 stdDisplay_masterPalette[256];
int Video_bModeSet = 0;

#if defined(TARGET_LINUX_GLES) || defined(OPENJKDF2_RUNTIME_GL)
extern SDL_Window *displayWindow;
extern SDL_GLContext glWindowContext;
static int stdDisplay_menuTexturesValid = 0;

static void stdDisplay_InvalidateMenuGLTextures(void)
{
    if (Video_menuTexId) {
        glDeleteTextures(1, &Video_menuTexId);
        Video_menuTexId = 0;
    }
    if (Video_overlayTexId) {
        glDeleteTextures(1, &Video_overlayTexId);
        Video_overlayTexId = 0;
    }
    stdDisplay_menuTexturesValid = 0;
}

int stdDisplay_EnsureMenuGLTextures(void)
{
    uint32_t newW;
    uint32_t newH;
    uint8_t *menuPixels;
    uint8_t *overlayPixels;

    if (stdDisplay_menuTexturesValid) {
        return 1;
    }
    if (!displayWindow || !glWindowContext) {
        openjkdf2_trace("stdDisplay_EnsureMenuGLTextures: no GL context");
        return 0;
    }

    SDL_GL_MakeCurrent(displayWindow, glWindowContext);

    newW = Video_menuBuffer.format.width;
    newH = Video_menuBuffer.format.height;
    menuPixels = stdDisplay_VBufferPixels(&Video_menuBuffer);
    overlayPixels = stdDisplay_VBufferPixels(&Video_overlayMapBuffer);
    if (!newW || !newH || !menuPixels || !overlayPixels) {
        openjkdf2_trace("stdDisplay_EnsureMenuGLTextures: missing menu buffers");
        return 0;
    }

    {
        uint32_t overlayW = Video_overlayMapBuffer.format.width;
        uint32_t overlayH = Video_overlayMapBuffer.format.height;
        if (!overlayW || !overlayH) {
            overlayW = newW;
            overlayH = newH;
        }

    stdDisplay_InvalidateMenuGLTextures();

    openjkdf2_trace("stdDisplay_EnsureMenuGLTextures: create textures");

    glGenTextures(1, &Video_menuTexId);
    glBindTexture(GL_TEXTURE_2D, Video_menuTexId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, newW);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, newW, newH, 0, GL_RED, GL_UNSIGNED_BYTE, menuPixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    glGenTextures(1, &Video_overlayTexId);
    glBindTexture(GL_TEXTURE_2D, Video_overlayTexId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, overlayW);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, overlayW, overlayH, 0, GL_RED, GL_UNSIGNED_BYTE, overlayPixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }

    stdDisplay_menuTexturesValid = 1;
    openjkdf2_trace("stdDisplay_EnsureMenuGLTextures: done");
    return 1;
}

int stdDisplay_ResizeOverlayMapBuffer(uint32_t w, uint32_t h)
{
    if (!w || !h) {
        return 0;
    }
    if (Video_overlayMapBuffer.format.width == w && Video_overlayMapBuffer.format.height == h) {
        return 1;
    }

    stdDisplay_Free8bppBuffer(&Video_overlayMapBuffer);
    if (!stdDisplay_Alloc8bppBuffer(&Video_overlayMapBuffer, w, h)) {
        openjkdf2_trace("stdDisplay_ResizeOverlayMapBuffer: alloc failed");
        return 0;
    }

    if (Video_overlayTexId) {
        glDeleteTextures(1, &Video_overlayTexId);
        Video_overlayTexId = 0;
    }
    stdDisplay_menuTexturesValid = 0;
    return 1;
}
#endif

#if !defined(TARGET_LINUX_GLES) && !defined(OPENJKDF2_RUNTIME_GL)
int stdDisplay_EnsureMenuGLTextures(void)
{
    return 1;
}
#endif

int stdDisplay_Startup()
{
    stdDisplay_bStartup = 1;
    return 1;
}

int stdDisplay_FindClosestDevice(void* a)
{
    Video_dword_866D78 = 0;
    return 0;
}

int stdDisplay_Open(int a)
{
    stdDisplay_pCurDevice = &stdDisplay_aDevices[0];
    stdDisplay_bOpen = 1;
    return 1;
}

void stdDisplay_Close()
{
    stdDisplay_bOpen = 0;
}

int stdDisplay_FindClosestMode(render_pair *a1, struct stdVideoMode *render_surface, unsigned int max_modes)
{
    Video_curMode = 0;
    stdDisplay_bPaged = 1;
    stdDisplay_bModeSet = 1;
    return 0;
}

int stdDisplay_SetMode(unsigned int modeIdx, const void *palette, int paged)
{
    // GUI layout is fixed at 640x480; scaling to window size happens in std3D_DrawMenu.
    uint32_t newW = 640;
    uint32_t newH = 480;

    openjkdf2_trace("stdDisplay_SetMode: enter");

    stdDisplay_pCurVideoMode = &Video_renderSurface[modeIdx];
    
    stdDisplay_pCurVideoMode->format.format.bpp = 8;
    stdDisplay_pCurVideoMode->format.width_in_pixels = newW;
    stdDisplay_pCurVideoMode->format.width = newW;
    stdDisplay_pCurVideoMode->format.height = newH;
    
    _memcpy(&Video_otherBuf.format, &stdDisplay_pCurVideoMode->format, sizeof(Video_otherBuf.format));
    _memcpy(&Video_menuBuffer.format, &stdDisplay_pCurVideoMode->format, sizeof(Video_menuBuffer.format));
    
    _memcpy(&Video_overlayMapBuffer.format, &stdDisplay_pCurVideoMode->format, sizeof(Video_overlayMapBuffer.format));
    

    if (Video_bModeSet)
    {
#if defined(TARGET_LINUX_GLES) || defined(OPENJKDF2_RUNTIME_GL)
        stdDisplay_InvalidateMenuGLTextures();
        stdDisplay_Free8bppBuffer(&Video_otherBuf);
        stdDisplay_Free8bppBuffer(&Video_menuBuffer);
        stdDisplay_Free8bppBuffer(&Video_overlayMapBuffer);
#else
        glDeleteTextures(1, &Video_menuTexId);
        glDeleteTextures(1, &Video_overlayTexId);
        if (Video_otherBuf.sdlSurface)
            SDL_FreeSurface(Video_otherBuf.sdlSurface);
        if (Video_menuBuffer.sdlSurface)
            SDL_FreeSurface(Video_menuBuffer.sdlSurface);
        if (Video_overlayMapBuffer.sdlSurface)
            SDL_FreeSurface(Video_overlayMapBuffer.sdlSurface);
        
        Video_otherBuf.sdlSurface = 0;
        Video_menuBuffer.sdlSurface = 0;
        Video_overlayMapBuffer.sdlSurface = 0;
#endif
    }

#if defined(TARGET_LINUX_GLES) || defined(OPENJKDF2_RUNTIME_GL)
    openjkdf2_trace("stdDisplay_SetMode: alloc 8bpp buffers");
    if (!stdDisplay_Alloc8bppBuffer(&Video_otherBuf, newW, newH)
        || !stdDisplay_Alloc8bppBuffer(&Video_menuBuffer, newW, newH)
        || !stdDisplay_Alloc8bppBuffer(&Video_overlayMapBuffer, newW, newH))
    {
        openjkdf2_trace("stdDisplay_SetMode: 8bpp alloc failed");
        return 0;
    }
    openjkdf2_trace("stdDisplay_SetMode: alloc ok");
#else
    SDL_Surface* otherSurface = SDL_CreateRGBSurface(0, newW, newH, 8,
                                        0,
                                        0,
                                        0,
                                        0);
    SDL_Surface* menuSurface = SDL_CreateRGBSurface(0, newW, newH, 8,
                                        0,
                                        0,
                                        0,
                                        0);
    SDL_Surface* overlaySurface = SDL_CreateRGBSurface(0, newW, newH, 8, 0, 0, 0, 0);
    
    if (palette)
    {
        memcpy(stdDisplay_gammaPalette, palette, 0x300);
        const rdColor24* pal24 = (const rdColor24*)palette;
        SDL_Color* tmp = (SDL_Color*)malloc(sizeof(SDL_Color) * 256);
        for (int i = 0; i < 256; i++)
        {
            tmp[i].r = pal24[i].r;
            tmp[i].g = pal24[i].g;
            tmp[i].b = pal24[i].b;
            tmp[i].a = 0xFF;
        }
        
        SDL_SetPaletteColors(otherSurface->format->palette, tmp, 0, 256);
        SDL_SetPaletteColors(menuSurface->format->palette, tmp, 0, 256);
        SDL_SetPaletteColors(overlaySurface->format->palette, tmp, 0, 256);
        free(tmp);
    }
    
    Video_otherBuf.sdlSurface = otherSurface;
    Video_menuBuffer.sdlSurface = menuSurface;
    Video_overlayMapBuffer.sdlSurface = overlaySurface;
    
    Video_menuBuffer.format.width_in_bytes = menuSurface->pitch;
    Video_otherBuf.format.width_in_bytes = otherSurface->pitch;
    Video_overlayMapBuffer.format.width_in_bytes = overlaySurface->pitch;
#endif

    if (palette)
    {
        memcpy(stdDisplay_gammaPalette, palette, 0x300);
        stdDisplay_SetMasterPalette((uint8_t*)palette);
    }
    
    Video_menuBuffer.format.width_in_pixels = newW;
    Video_otherBuf.format.width_in_pixels = newW;
    Video_overlayMapBuffer.format.width_in_pixels = newW;
    Video_menuBuffer.format.width = newW;
    Video_otherBuf.format.width = newW;
    Video_overlayMapBuffer.format.width = newW;
    Video_menuBuffer.format.height = newH;
    Video_otherBuf.format.height = newH;
    Video_overlayMapBuffer.format.height = newH;
    
    Video_menuBuffer.format.format.bpp = 8;
    Video_otherBuf.format.format.bpp = 8;
    Video_overlayMapBuffer.format.format.bpp = 8;

#if !defined(TARGET_LINUX_GLES) && !defined(OPENJKDF2_RUNTIME_GL)
    glGenTextures(1, &Video_menuTexId);
    glBindTexture(GL_TEXTURE_2D, Video_menuTexId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, newW);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, newW, newH, 0, GL_RED, GL_UNSIGNED_BYTE, stdDisplay_VBufferPixels(&Video_menuBuffer));
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    
    glGenTextures(1, &Video_overlayTexId);
    glBindTexture(GL_TEXTURE_2D, Video_overlayTexId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, newW);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, newW, newH, 0, GL_RED, GL_UNSIGNED_BYTE, stdDisplay_VBufferPixels(&Video_overlayMapBuffer));
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif

    Video_bModeSet = 1;
    openjkdf2_trace("stdDisplay_SetMode: done");
    
    return 1;
}

void stdDisplay_SyncMenuBufferFormat(void)
{
#if defined(TARGET_LINUX_GLES) || defined(OPENJKDF2_RUNTIME_GL)
    /* Buffers 8bpp sin SDL_Surface; format ya viene de SetMode/Alloc8bppBuffer */
    (void)0;
#else
    if (Video_menuBuffer.sdlSurface) {
        Video_menuBuffer.format.width = Video_menuBuffer.sdlSurface->w;
        Video_menuBuffer.format.height = Video_menuBuffer.sdlSurface->h;
        Video_menuBuffer.format.width_in_pixels = Video_menuBuffer.sdlSurface->w;
        Video_menuBuffer.format.width_in_bytes = Video_menuBuffer.sdlSurface->pitch;
    }
#endif
}

int stdDisplay_ClearRect(stdVBuffer *buf, int fillColor, rdRect *rect)
{
    return stdDisplay_VBufferFill(buf, fillColor, rect);
}



int stdDisplay_DDrawGdiSurfaceFlip()
{
    Window_SdlUpdate();
    return 1;
}

int stdDisplay_ddraw_waitforvblank()
{
    Window_SdlVblank();
    return 1;
}

int stdDisplay_SetMasterPalette(uint8_t* pal)
{
    rdColor24* pal24 = (rdColor24*)pal;
    
    memcpy(stdDisplay_masterPalette, pal24, sizeof(stdDisplay_masterPalette));
    
    return 1;
}

stdVBuffer* stdDisplay_VBufferNew(stdVBufferTexFmt *fmt, int create_ddraw_surface, int gpu_mem, const void* palette)
{
    stdVBuffer* out = (stdVBuffer*)std_pHS->alloc(sizeof(stdVBuffer));
    
    _memset(out, 0, sizeof(*out));
    
    _memcpy(&out->format, fmt, sizeof(out->format));

#if defined(TARGET_LINUX_GLES) || defined(OPENJKDF2_RUNTIME_GL)
    if (fmt->format.bpp == 8) {
        if (!stdDisplay_Alloc8bppBuffer(out, fmt->width, fmt->height)) {
            std_pHS->free(out);
            openjkdf2_trace("stdDisplay_VBufferNew: 8bpp alloc failed");
            return NULL;
        }
        return out;
    }
#endif
    
    // force 0 reads
    //out->format.width = 0;
    //out->format.width_in_bytes = 0;
    //out->surface_lock_alloc = std_pHS->alloc(texture_size_in_bytes);
    
    //if (fmt->format.g_bits == 6) // RGB565
    {
        fmt->format.r_bits = 0;
        fmt->format.g_bits = 0;
        fmt->format.b_bits = 0;
        fmt->format.r_shift = 0;
        fmt->format.g_shift = 0;
        fmt->format.b_shift = 0;
    }

    uint32_t rbitmask = ((1 << fmt->format.r_bits) - 1) << fmt->format.r_shift;
    uint32_t gbitmask = ((1 << fmt->format.g_bits) - 1) << fmt->format.g_shift;
    uint32_t bbitmask = ((1 << fmt->format.b_bits) - 1) << fmt->format.b_shift;
    uint32_t abitmask = 0;//((1 << fmt->format.a_bits) - 1) << fmt->format.a_shift;
    if (fmt->format.bpp == 8)
    {
        rbitmask = 0;
        gbitmask = 0;
        bbitmask = 0;
        abitmask = 0;
    }

    SDL_Surface* surface = SDL_CreateRGBSurface(0, fmt->width, fmt->height, fmt->format.bpp, rbitmask, gbitmask, bbitmask, abitmask);
    
    if (surface)
    {
        static int num = 0;
        //printf("Allocated VBuffer %u, w %u h %u bpp %u %x %x %x\n", num++, fmt->width, fmt->height, fmt->format.bpp, rbitmask, gbitmask, bbitmask);
        out->format.width_in_bytes = surface->pitch;
        out->format.width_in_pixels = fmt->width;
        out->format.texture_size_in_bytes = surface->pitch * fmt->height;
    }
    else
    {
        stdPlatform_Printf("Failed to allocate VBuffer! %s, w %u h %u bpp %u, rmask %x gmask %x bmask %x amask %x, %x %x %x, %x %x %x\n", SDL_GetError(), fmt->width, fmt->height, fmt->format.bpp, rbitmask, gbitmask, bbitmask, abitmask, fmt->format.r_bits, fmt->format.g_bits, fmt->format.b_bits, fmt->format.r_shift, fmt->format.g_shift, fmt->format.b_shift);
        std_pHS->free(out);
        return NULL;
    }
    //printf("Failed to allocate VBuffer! %s, w %u h %u bpp %u, rmask %x gmask %x bmask %x amask %x, %x %x %x, %x %x %x\n", SDL_GetError(), fmt->width, fmt->height, fmt->format.bpp, rbitmask, gbitmask, bbitmask, abitmask, fmt->format.r_bits, fmt->format.g_bits, fmt->format.b_bits, fmt->format.r_shift, fmt->format.g_shift, fmt->format.b_shift);
    
    out->sdlSurface = surface;
    
    return out;
}

int stdDisplay_VBufferLock(stdVBuffer *buf)
{
    if (!buf) return 0;

    if (!buf->sdlSurface) {
        return buf->surface_lock_alloc != NULL;
    }

    SDL_LockSurface(buf->sdlSurface);
    buf->surface_lock_alloc = (char*)buf->sdlSurface->pixels;
    return 1;
}

void stdDisplay_VBufferUnlock(stdVBuffer *buf)
{
    if (!buf) return;

    if (!buf->sdlSurface) {
        return;
    }
    
    buf->surface_lock_alloc = NULL;
    SDL_UnlockSurface(buf->sdlSurface);
}

int stdDisplay_VBufferCopy(stdVBuffer *vbuf, stdVBuffer *vbuf2, unsigned int blit_x, int blit_y, rdRect *rect, int alpha_maybe)
{
    if (!vbuf || !vbuf2) return 1;
    
    rdRect fallback = {0,0,vbuf2->format.width, vbuf2->format.height};
    if (!rect)
    {
        rect = &fallback;
        //memcpy(vbuf->sdlSurface->pixels, vbuf2->sdlSurface->pixels, 640*480);
        //return;
    }
    
    //if (vbuf == &Video_menuBuffer)
    //    stdPlatform_Printf("Vbuffer copy to menu %u,%u %ux%u %u,%u\n", rect->x, rect->y, rect->width, rect->height, blit_x, blit_y);
    
    if (vbuf->palette && vbuf->sdlSurface && vbuf->sdlSurface->format && vbuf->sdlSurface->format->palette)
    {
        rdColor24* pal24 = (rdColor24*)vbuf->palette;
        SDL_Color* tmp = (SDL_Color*)malloc(sizeof(SDL_Color) * 256);
        for (int i = 0; i < 256; i++)
        {
            tmp[i].r = pal24[i].r;
            tmp[i].g = pal24[i].g;
            tmp[i].b = pal24[i].b;
            tmp[i].a = 0xFF;
        }
    
        SDL_SetPaletteColors(vbuf->sdlSurface->format->palette, tmp, 0, 256);
        free(tmp);
    }
    
    if (vbuf2->palette && vbuf2->sdlSurface && vbuf2->sdlSurface->format && vbuf2->sdlSurface->format->palette)
    {
        rdColor24* pal24 = (rdColor24*)vbuf2->palette;
        SDL_Color* tmp = (SDL_Color*)malloc(sizeof(SDL_Color) * 256);
        for (int i = 0; i < 256; i++)
        {
            tmp[i].r = pal24[i].r;
            tmp[i].g = pal24[i].g;
            tmp[i].b = pal24[i].b;
            tmp[i].a = 0xFF;
        }
        
        SDL_SetPaletteColors(vbuf2->sdlSurface->format->palette, tmp, 0, 256);
        free(tmp);
    }

    SDL_Rect dstRect = {(int)blit_x, (int)blit_y, (int)rect->width, (int)rect->height};
    SDL_Rect srcRect = {(int)rect->x, (int)rect->y, (int)rect->width, (int)rect->height};
    
    uint8_t* srcPixels = stdDisplay_VBufferPixels(vbuf2);
    uint8_t* dstPixels = stdDisplay_VBufferPixels(vbuf);
    if (!srcPixels || !dstPixels) {
        return 0;
    }
    uint32_t srcStride = vbuf2->format.width_in_bytes;
    uint32_t dstStride = vbuf->format.width_in_bytes;

    int self_copy = 0;

    if (dstPixels == srcPixels)
    {
        size_t buf_len = srcStride * dstRect.w * dstRect.h;
        uint8_t* dstPixels = (uint8_t*)malloc(buf_len);
        int has_alpha = 0;//!(rect->width == 640);

        SDL_Rect dstRect_inter = {0, 0, rect->width, rect->height};

        for (int i = 0; i < rect->width; i++)
        {
            for (int j = 0; j < rect->height; j++)
            {
                if ((uint32_t)(i + srcRect.x) > (uint32_t)vbuf2->format.width) continue;
                if ((uint32_t)(j + srcRect.y) > (uint32_t)vbuf2->format.height) continue;
                
                uint8_t pixel = srcPixels[(i + srcRect.x) + ((j + srcRect.y)*srcStride)];

                if (!pixel && has_alpha) continue;
                if ((uint32_t)(i + dstRect_inter.x) > (uint32_t)vbuf->format.width) continue;
                if ((uint32_t)(j + dstRect_inter.y) > (uint32_t)vbuf->format.height) continue;

                dstPixels[(i + dstRect_inter.x) + ((j + dstRect_inter.y)*srcStride)] = pixel;
            }
        }
        
        

        srcPixels = dstPixels;
        srcRect.x = 0;
        srcRect.y = 0;

        self_copy = 1;
    }
    
    int once = 0;
    int has_alpha = !(rect->width == 640) && (alpha_maybe & 1);
    
    for (int i = 0; i < rect->width; i++)
    {
        for (int j = 0; j < rect->height; j++)
        {
            if ((uint32_t)(i + srcRect.x) >= (uint32_t)vbuf2->format.width) continue;
            if ((uint32_t)(j + srcRect.y) >= (uint32_t)vbuf2->format.height) continue;
            
            uint8_t pixel = srcPixels[(i + srcRect.x) + ((j + srcRect.y)*srcStride)];

            if (!pixel && has_alpha) continue;
            if ((uint32_t)(i + dstRect.x) >= (uint32_t)vbuf->format.width) continue;
            if ((uint32_t)(j + dstRect.y) >= (uint32_t)vbuf->format.height) continue;

            dstPixels[(i + dstRect.x) + ((j + dstRect.y)*dstStride)] = pixel;
        }
    }

    if (self_copy)
    {
        free(srcPixels);
    }
    
    //SDL_BlitSurface(vbuf2->sdlSurface, &srcRect, vbuf->sdlSurface, &dstRect); //TODO error check
    if (vbuf == &Video_menuBuffer)
        std3D_MarkMenuBufferDirty();
    return 1;
}

int stdDisplay_VBufferFill(stdVBuffer *vbuf, int fillColor, rdRect *rect)
{    
    rdRect fallback = {0,0,vbuf->format.width, vbuf->format.height};
    if (!rect)
    {
        rect = &fallback;
    }
    
    //if (vbuf == &Video_menuBuffer)
    //    stdPlatform_Printf("Vbuffer fill to menu %u,%u %ux%u\n", rect->x, rect->y, rect->width, rect->height);

    SDL_Rect dstRect = {rect->x, rect->y, rect->width, rect->height};
    
    //printf("%x; %u %u %u %u\n", fillColor, rect->x, rect->y, rect->width, rect->height);
    
    uint8_t* dstPixels = stdDisplay_VBufferPixels(vbuf);
    uint32_t dstStride = vbuf->format.width_in_bytes;
    uint32_t max_idx = dstStride * vbuf->format.height;
    if (!dstPixels) {
        return 0;
    }
    for (int i = 0; i < rect->width; i++)
    {
        for (int j = 0; j < rect->height; j++)
        {
            uint32_t idx = (i + dstRect.x) + ((j + dstRect.y)*dstStride);
            if (idx > max_idx)
                continue;
            
            dstPixels[idx] = fillColor;
        }
    }
    
    //SDL_FillRect(vbuf, &dstRect, fillColor); //TODO error check

    if (vbuf == &Video_menuBuffer)
        std3D_MarkMenuBufferDirty();

    return 1;
}

int stdDisplay_VBufferSetColorKey(stdVBuffer *vbuf, int color)
{
    //DDCOLORKEY v3; // [esp+0h] [ebp-8h] BYREF

    if ( vbuf->bSurfaceLocked )
    {
        /*if ( vbuf->bSurfaceLocked == 1 )
        {
            v3.dwColorSpaceLowValue = color;
            v3.dwColorSpaceHighValue = color;
            vbuf->ddraw_surface->lpVtbl->SetColorKey(vbuf->ddraw_surface, 8, &v3);
            return 1;
        }*/
        vbuf->transparent_color = color;
    }
    else
    {
        vbuf->transparent_color = color;
    }
    return 1;
}

void stdDisplay_VBufferFree(stdVBuffer *vbuf)
{
    // Added: Safety fallbacks
    if (!vbuf) {
        return;
    }
    stdDisplay_VBufferUnlock(vbuf);
#if defined(TARGET_LINUX_GLES) || defined(OPENJKDF2_RUNTIME_GL)
    stdDisplay_Free8bppBuffer(vbuf);
#else
    if (vbuf->sdlSurface) {
        SDL_FreeSurface(vbuf->sdlSurface);
        vbuf->sdlSurface = NULL;
    }
#endif
    std_pHS->free(vbuf);
}

void stdDisplay_ddraw_surface_flip2()
{
}

void stdDisplay_RestoreDisplayMode()
{

}

stdVBuffer* stdDisplay_VBufferConvertColorFormat(void* a, stdVBuffer* b)
{
    return b;
}

int stdDisplay_GammaCorrect3(int a1)
{
    jk_printf("STUB: stdDisplay_GammaCorrect3\n");
    return 1;
}

int stdDisplay_SetCooperativeLevel(uint32_t a){return 0;}
int stdDisplay_DrawAndFlipGdi(uint32_t a){return 0;}
void stdDisplay_422A50(){}
#endif

void stdDisplay_GammaCorrect(const void *pPal)
{
    _memcpy(stdDisplay_tmpGammaPal, pPal, sizeof(stdDisplay_tmpGammaPal));
    if ( stdDisplay_paGammaTable && stdDisplay_gammaIdx )
        stdColor_GammaCorrect((uint8_t *)stdDisplay_gammaPalette, (uint8_t *)stdDisplay_tmpGammaPal, 256, stdDisplay_paGammaTable[stdDisplay_gammaIdx - 1]);
    else
        _memcpy(stdDisplay_gammaPalette, pPal, sizeof(stdDisplay_gammaPalette));
}