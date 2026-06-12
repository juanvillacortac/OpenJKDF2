#include "Window.h"

#include "Win95/stdGdi.h"
#include "Platform/std3D.h"
#include "Main/Main.h"
#include "Main/jkMain.h"
#include "Main/jkGame.h"
#include "Gui/jkGUI.h"
#include "Gui/jkGUIRend.h"
#include "Win95/stdDisplay.h"
#include "World/jkPlayer.h"
#include "Platform/stdControl.h"
#include "stdPlatform.h"
#include "Devices/sithConsole.h"
#include "Platform/wuRegistry.h"
#include "Main/jkQuakeConsole.h"

#include "jk.h"

#ifdef ARCH_WASM
#include <emscripten.h>
#endif

#ifdef SDL2_RENDER

#include <fcntl.h> 
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#endif //!_WIN32

#if !defined(WIN64_MINGW) && !defined(_WIN32)
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#else
#include <conio.h>
#endif
//#include <stropts.h>

#include "SDL2_helper.h"
#include "Platform/trace_gles.h"
#include "Platform/handheld.h"
#if defined(TARGET_LINUX_GLES)
#include "Platform/Posix/gles_loader.h"
#endif

#include <string.h>

//#include <GL/glew.h>
#ifdef MACOS
#include "Platform/macOS/SDL_fix.h"
#else
//#include <GL/gl.h>
#endif
#include "Win95/Video.h"

#if defined(MACOS)
#include <stdbool.h>
#import <Carbon/Carbon.h>
#endif

extern int Window_xPos, Window_yPos;
#endif // SDL2_RENDER

int Window_xSize = WINDOW_DEFAULT_WIDTH;
int Window_ySize = WINDOW_DEFAULT_HEIGHT;
int Window_screenXSize = WINDOW_DEFAULT_WIDTH;
int Window_screenYSize = WINDOW_DEFAULT_HEIGHT;
int Window_isHiDpi = 0;
int Window_isFullscreen = 0;
int Window_needsRecreate = 0;
int Window_bShouldPopSteamKeyboard = 0;

#if defined(SDL2_RENDER)
static int Window_physXSize;
static int Window_physYSize;
static int Window_presentX;
static int Window_presentY;
static int Window_presentW;
static int Window_presentH;
static int Window_bForcedResolution;

static int Window_ParseForceRes(const char *env, int *outW, int *outH)
{
    int w = 0;
    int h = 0;

    if (!env || !env[0] || !outW || !outH)
        return 0;

    if (_sscanf(env, "%dx%d", &w, &h) == 2
        || _sscanf(env, "%d,%d", &w, &h) == 2
        || _sscanf(env, "%d %d", &w, &h) == 2)
    {
        if (w >= 160 && h >= 120 && w <= 3840 && h <= 2160) {
            *outW = w;
            *outH = h;
            return 1;
        }
    }
    return 0;
}

static void Window_SetLogicalPresent(int logicalW, int logicalH, int physW, int physH)
{
    double logicalAspect = (double)logicalW / (double)logicalH;
    double physAspect = (double)physW / (double)physH;

    Window_bForcedResolution = 1;
    Window_xSize = logicalW;
    Window_ySize = logicalH;
    Window_screenXSize = logicalW;
    Window_screenYSize = logicalH;

    if (logicalAspect > physAspect) {
        Window_presentW = physW;
        Window_presentH = (int)((double)physW / logicalAspect + 0.5);
        Window_presentX = 0;
        Window_presentY = (physH - Window_presentH) / 2;
    } else {
        Window_presentH = physH;
        Window_presentW = (int)((double)physH * logicalAspect + 0.5);
        Window_presentY = 0;
        Window_presentX = (physW - Window_presentW) / 2;
    }
}

static int Window_ShouldAutoDownscale(void)
{
    const char *env = getenv("OPENJKDF2_AUTO_DOWNSCALE");
    if (env && (!strcmp(env, "0") || !__strcmpi(env, "false") || !__strcmpi(env, "off")))
        return 0;
    return 1;
}

static void Window_ApplyForcedResolution(void)
{
    int physW = Window_xSize;
    int physH = Window_ySize;
    int forceW = 0;
    int forceH = 0;

    if (!openjkdf2_IsHandheld()) {
        Window_bForcedResolution = 0;
        Window_presentX = 0;
        Window_presentY = 0;
        Window_presentW = physW;
        Window_presentH = physH;
        return;
    }

    Window_physXSize = physW;
    Window_physYSize = physH;

    if (Window_ParseForceRes(getenv("OPENJKDF2_FORCE_RES"), &forceW, &forceH)) {
        Window_SetLogicalPresent(forceW, forceH, physW, physH);
        stdPlatform_Printf(
            "OpenJKDF2: OPENJKDF2_FORCE_RES %dx%d (present %dx%d at %d,%d on panel %dx%d)\n",
            forceW, forceH,
            Window_presentW, Window_presentH, Window_presentX, Window_presentY,
            physW, physH);
        return;
    }

    if (Window_ShouldAutoDownscale() && (physW < 640 || physH < 480)) {
        double physAspect = (double)physW / (double)physH;
        const double ar43 = 4.0 / 3.0;

        /* Wide handheld panels (3:2, 16:9): fill the screen instead of 4:3 pillarbox. */
        if (physAspect > ar43 + 0.001) {
            Window_SetLogicalPresent(physW, physH, physW, physH);
            stdPlatform_Printf(
                "OpenJKDF2: auto downscale %dx%d fill panel (present %dx%d at %d,%d on panel %dx%d)\n",
                physW, physH,
                Window_presentW, Window_presentH, Window_presentX, Window_presentY,
                physW, physH);
            return;
        }

        double scale = (double)physW / 640.0;
        if ((double)physH / 480.0 < scale)
            scale = (double)physH / 480.0;

        forceW = (int)(640.0 * scale + 0.5);
        forceH = (int)(480.0 * scale + 0.5);
        if (forceW < 160)
            forceW = 160;
        if (forceH < 120)
            forceH = 120;

        Window_SetLogicalPresent(forceW, forceH, physW, physH);
        stdPlatform_Printf(
            "OpenJKDF2: auto downscale %dx%d (present %dx%d at %d,%d on panel %dx%d)\n",
            forceW, forceH,
            Window_presentW, Window_presentH, Window_presentX, Window_presentY,
            physW, physH);
        return;
    }

    Window_bForcedResolution = 0;
    Window_presentX = 0;
    Window_presentY = 0;
    Window_presentW = physW;
    Window_presentH = physH;
}

static void Window_LogicalFromPanelCoords(int panelX, int panelY, int *outX, int *outY)
{
    if (Window_bForcedResolution && Window_presentW > 0 && Window_presentH > 0) {
        panelX = (int)((panelX - Window_presentX) * (double)Window_xSize / (double)Window_presentW);
        panelY = (int)((panelY - Window_presentY) * (double)Window_ySize / (double)Window_presentH);
    }

    *outX = panelX;
    *outY = panelY;
}

void Window_BeginScreenDraw(void)
{
    if (!openjkdf2_IsHandheld())
        return;
    if (!Window_bForcedResolution || Window_presentW <= 0 || Window_presentH <= 0)
        return;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, Window_physXSize, Window_physYSize);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(Window_presentX, Window_presentY, Window_presentW, Window_presentH);
}
#endif /* SDL2_RENDER */

void Window_SetHiDpi(int val)
{
    if (Window_isHiDpi != val)
    {
        Window_isHiDpi = val;

        Window_needsRecreate = 1;
    }

    wuRegistry_SaveBool("Window_isHiDpi", Window_isHiDpi);
}

void Window_SetFullscreen(int val)
{
    if (Window_isFullscreen != val)
    {
        // Reset window when exiting fullscreen
        // TODO: Add settings for these sizes maybe?
        if (Window_isFullscreen && !val) {
            Window_xSize = WINDOW_DEFAULT_WIDTH;
            Window_ySize = WINDOW_DEFAULT_HEIGHT;
            Window_screenXSize = WINDOW_DEFAULT_WIDTH;
            Window_screenYSize = WINDOW_DEFAULT_HEIGHT;
#ifdef SDL2_RENDER
            Window_xPos = SDL_WINDOWPOS_CENTERED;
            Window_yPos = SDL_WINDOWPOS_CENTERED;
#endif
        }

        Window_isFullscreen = val;
        Window_needsRecreate = 1;
    }

    wuRegistry_SaveBool("Window_isFullscreen", Window_isFullscreen);
    
}

//static wm_handler Window_ext_handlers[16] = {0};

int Window_AddMsgHandler(WindowHandler_t a1)
{
    int i = 0;

    // Added: no duplicates
    for (i = 0; i < 16; i++)
    {
        if (Window_ext_handlers[i].exists && Window_ext_handlers[i].handler == a1)
            return 1;
    }

    for (i = 0; i < 16; i++)
    {
        if ( !Window_ext_handlers[i].exists )
            break;
    }
    
    // Added: no OOB
    if (i >= 16) return 1;

    Window_ext_handlers[i].handler = a1;
    Window_ext_handlers[i].exists = 1;
    ++g_handler_count;
    return 1;
}

int Window_RemoveMsgHandler(WindowHandler_t a1)
{
    int i = 0;

    // Added: the original would still decrement on missing handlers
    for (i = 0; i < 16; i++)
    {
        if ( Window_ext_handlers[i].handler == a1 )
        {
            Window_ext_handlers[i].handler = 0;
            Window_ext_handlers[i].exists = 0;
            g_handler_count -= 1; // doing g_handler_count-- changes behavior???
            return 1;
        }
    }

    return 1;
}

int Window_AddDialogHwnd(HWND a1)
{
    int v1; // eax

    v1 = g_thing_two_some_dialog_count;
    if ( (unsigned int)g_thing_two_some_dialog_count >= 0x10 )
        return 0;
    Window_aDialogHwnds[g_thing_two_some_dialog_count] = a1;
    g_thing_two_some_dialog_count = v1 + 1;
    return 1;
}

#if !defined(SDL2_RENDER) && defined(WIN32)
#define dword_855E98 (*(int*)0x855E98)
#define dword_855DE4 (*(int*)0x855DE4)
#else
static int dword_855E98 = 0;
static int dword_855DE4 = 0;
#endif // SDL2_RENDER

int Window_msg_main_handler(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    int handler_count; // ebx
    struct wm_handler *ext_handler; // esi
    DWORD dwProcessId; // [esp+10h] [ebp-8h] BYREF
    LRESULT v10; // [esp+14h] [ebp-4h] BYREF

    switch ( Msg )
    {
        case WM_CREATE:
            g_app_active = 0;
            g_window_active = 0;
            break;
        case WM_DESTROY:
            g_window_not_destroyed = 0;
            Main_Shutdown();
            break;
        case WM_ACTIVATE:
            if ( (uint16_t)wParam == 2 || (uint16_t)wParam == 1 )// WA_ACTIVE or WA_CLICKACTIVE
            {
                g_window_active = 1;
                if ( dword_855E98 )
                {
                    dword_855E98 = 0;
                    if ( Window_setCooperativeLevel )
                        Window_setCooperativeLevel(0);
                }
#ifdef WIN32_BLOBS
                jk_SetFocus(g_hWnd);
#endif
            }
            else
            {
                if ( dword_855DE4 == 1 && g_window_not_destroyed && g_app_active && !dword_855E98 )
                {
                    dwProcessId = 0;
                    lParam = 1;
                    if ( lParam )
                    {
#ifdef WIN32_BLOBS
                        jk_GetWindowThreadProcessId((HWND)lParam, (LPDWORD)&lParam);
                        jk_GetWindowThreadProcessId(hWnd, &dwProcessId);
#endif
                    }
                    if ( dwProcessId == lParam )
                    {
                        dword_855E98 = 1;
                        if ( Window_drawAndFlip )
                            Window_drawAndFlip(0);
                    }
                }
                g_window_active = 0;
            }
            break;
        case WM_ACTIVATEAPP:
            g_app_active = wParam != 0;
            break;
        default:
            break;
    }

    if ( !g_app_active || (g_app_suspended = 1, !g_window_active) )
        g_app_suspended = 0;
    handler_count = 0;

    if ( g_handler_count <= 0 )
        return Window_DefaultHandler(hWnd, Msg, wParam, lParam, NULL);

    for ( ext_handler = Window_ext_handlers; !ext_handler->exists || !ext_handler->handler(hWnd, Msg, wParam, lParam, &v10); ++ext_handler )
    {
        if ( ++handler_count >= g_handler_count )
            return Window_DefaultHandler(hWnd, Msg, wParam, lParam, NULL);
    }
    return v10;
}

#if !defined(SDL2_RENDER) && defined(WIN32)

int Window_Main(HINSTANCE hInstance, int a2, char *lpCmdLine, int nShowCmd, LPCSTR lpWindowName)
{
    int result;
    WNDCLASSEXA wndClass;
    MSG msg;

    g_handler_count = 0;
    g_thing_two_some_dialog_count = 0;
    g_should_exit = 0;
    g_window_not_destroyed = 0;
    g_hInstance = hInstance;
    g_nShowCmd = nShowCmd;

    wndClass.cbSize = 48;
    wndClass.hInstance = hInstance;
    wndClass.lpszClassName = "wKernel";
    wndClass.lpszMenuName = 0;
    wndClass.lpfnWndProc = Window_msg_main_handler;
    wndClass.style = 3;
    wndClass.hIcon = jk_LoadIconA(hInstance, "APPICON");
    if ( !wndClass.hIcon )
        wndClass.hIcon = jk_LoadIconA(0, (void*)32512);
    wndClass.hIconSm = jk_LoadIconA(hInstance, "APPICON");
    if ( !wndClass.hIconSm )
        wndClass.hIconSm = jk_LoadIconA(0, (void*)32512);
    wndClass.hCursor = jk_LoadCursorA(0, (void*)0x7F00);
    wndClass.cbClsExtra = 0;
    wndClass.cbWndExtra = 0;
    wndClass.hbrBackground = jk_GetStockObject(4);

    if (jk_RegisterClassExA(&wndClass))
    {
        if ( jk_FindWindowA("wKernel", lpWindowName) )
            jk_exit(-1);

        uint32_t hres = jk_GetSystemMetrics(1);
        uint32_t vres = jk_GetSystemMetrics(0);
        g_hWnd = jk_CreateWindowExA(0x40000u, "wKernel", lpWindowName, 0x90000000, 0, 0, vres, hres, 0, 0, hInstance, 0);

        if (g_hWnd)
        {
            g_hInstance = hInstance;
            jk_ShowWindow(g_hWnd, 1);
            jk_UpdateWindow(g_hWnd);
        }
    }

    stdGdi_SetHwnd(g_hWnd);
    stdGdi_SetHInstance(g_hInstance);
    jk_InitCommonControls();

    g_855E8C = 2 * jk_GetSystemMetrics(32);
    uint32_t metrics_32 = jk_GetSystemMetrics(32);
    g_855E90 = jk_GetSystemMetrics(15) + 2 * metrics_32;
    result = Main_Startup(lpCmdLine);

    if (!result) return result;

    
    g_window_not_destroyed = 1;

    while (1)
    {
        if (jk_PeekMessageA(&msg, 0, 0, 0, 0))
        {
            if (!jk_GetMessageA(&msg, 0, 0, 0))
            {
                result = msg.wParam;
                g_should_exit = 1;
                break;
            }

            uint32_t some_cnt = 0;
            if (g_thing_two_some_dialog_count > 0)
            {
#if 0
                v16 = &thing_three;
                do
                {
                    //TODO if ( jk_IsDialogMessageA(*v16, &msg) )
                    //  break;
                    ++some_cnt;
                    ++v16;
                }
                while ( some_cnt < g_thing_two_some_dialog_count );
#endif
            }

            if (some_cnt == g_thing_two_some_dialog_count)
            {
                jk_TranslateMessage(&msg);
                jk_DispatchMessageA(&msg);
            }

            if (!jk_PeekMessageA(&msg, 0, 0, 0, 0))
            {
                result = 0;
                if ( g_should_exit )
                    return result;
            }
        }

        //if (user32->stopping) break;

        jkMain_GuiAdvance();
    }

    return result;
}

int Window_DefaultHandler(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam, void* unused)
{
    return DefWindowProcA(hWnd, Msg, wParam, lParam);
}

#endif

#ifdef SDL2_RENDER

SDL_Window* displayWindow = NULL;
SDL_Event event;
SDL_GLContext glWindowContext;

int Window_lastXRel = 0;
int Window_lastYRel = 0;
int Window_lastSampleTime = 0;
int Window_lastSampleMs = 0;
int Window_bMouseLeft = 0;
int Window_bMouseRight = 0;
int Window_resized = 0;
int Window_mouseX = 0;
int Window_mouseY = 0;
int Window_mouseWheelX = 0;
int Window_mouseWheelY = 0;
int Window_lastMouseX = 0;
int Window_lastMouseY = 0;
int Window_xPos = SDL_WINDOWPOS_CENTERED;
int Window_yPos = SDL_WINDOWPOS_CENTERED;
int last_jkGame_isDDraw = 0;
#ifdef QUAKE_CONSOLE
int last_jkQuakeConsole_bOpen = 0;
#endif
int Window_menu_mouseX = 0;
int Window_menu_mouseY = 0;

extern int jkGuiBuildMulti_bRendering;

void Window_HandleMouseMove(SDL_MouseMotionEvent *event)
{
    int x = event->x;
    int y = event->y;

    if (openjkdf2_IsHandheld())
        Window_LogicalFromPanelCoords(x, y, &x, &y);

    Window_lastMouseX = Window_mouseX;
    Window_lastMouseY = Window_mouseY;

    if (!jkGame_isDDraw)
    {
        // FLEXTODO
        flex_t fX = (flex_t)x;
        flex_t fY = (flex_t)y;

        double menu_x, menu_y, menu_w, menu_h;

        std3D_ComputeMenuRect((double)Window_screenXSize, (double)Window_screenYSize, &menu_x, &menu_y, &menu_w, &menu_h);

        Window_mouseX = (int)(((fX - menu_x) / menu_w) * 640.0);
        Window_mouseY = (int)(((fY - menu_y) / menu_h) * 480.0);
        //printf("%d %d\n", Window_mouseX, Window_mouseY);
    }
    else
    {
        Window_mouseX = x;
        Window_mouseY = y;// - (Window_ySize - 480);
    }

    if (Window_mouseX < 0)
        Window_mouseX = 0;

    if (jkQuakeConsole_bOpen) return; // Hijack all input to console

    uint32_t pos = ((Window_mouseX) & 0xFFFF) | (((Window_mouseY) << 16) & 0xFFFF0000);
    
    Window_lastSampleMs = event->timestamp - Window_lastSampleTime;
    //Window_lastSampleTime = event->timestamp;
    Window_lastXRel += event->xrel;
    Window_lastYRel += event->yrel;

    Window_msg_main_handler(g_hWnd, WM_MOUSEMOVE, 0, pos);
}

int jkCutscene_wasPaused = 0;
int jkGame_wasDDraw = 0;
int Window_bNeedsKeyboardFixed = 0;
void Window_HandleWindowEvent(SDL_Event* event)
{
    switch (event->window.event) 
    {
        case SDL_WINDOWEVENT_SHOWN:
#ifdef MACOS
            {
                static int bMacosOnlyOncePerProcessLifetimeTriggerTheStupidDylibLoad = 0;
                if (!bMacosOnlyOncePerProcessLifetimeTriggerTheStupidDylibLoad)
                {
                    CGEventRef ref = CGEventCreateKeyboardEvent(NULL, 0x72 /* help */, 1);
                    CGEventSetFlags( ref, kCGEventFlagMaskNumericPad );
                    CGEventSetFlags( ref, kCGEventFlagMaskSecondaryFn );
                    CGEventPost(kCGHIDEventTap, ref);
                    CFRelease(ref);
                    bMacosOnlyOncePerProcessLifetimeTriggerTheStupidDylibLoad = 1;
                }
            }
#endif
            //printf("Window %d shown", event->window.windowID);
            break;
        case SDL_WINDOWEVENT_HIDDEN:
            //printf("Window %d hidden", event->window.windowID);
            break;
        case SDL_WINDOWEVENT_EXPOSED:
            //printf("Window %d exposed", event->window.windowID);
            break;
        case SDL_WINDOWEVENT_MOVED:
            /*printf("Window %d moved to %d,%d",
                    event->window.windowID, event->window.data1,
                    event->window.data2);*/
            Window_xPos = event->window.data1;
            Window_yPos = event->window.data2;
            break;
        case SDL_WINDOWEVENT_RESIZED:
        case SDL_WINDOWEVENT_SIZE_CHANGED:
            if (Window_xSize != event->window.data1 || Window_ySize != event->window.data2)
                Window_resized = 1;

            //Window_xSize = event->window.data1;
            //Window_ySize = event->window.data2;
            SDL_GL_GetDrawableSize(displayWindow, &Window_xSize, &Window_ySize);
            SDL_GetWindowSize(displayWindow, &Window_screenXSize, &Window_screenYSize);
            if (openjkdf2_IsHandheld())
                Window_ApplyForcedResolution();
            else {
                if (Window_xSize < 640) Window_xSize = 640;
                if (Window_ySize < 480) Window_ySize = 480;
            }
            //printf("%u %u\n", Window_xSize, Window_ySize);
            break;
        case SDL_WINDOWEVENT_MINIMIZED:
            stdPlatform_Printf("Window %d minimized", event->window.windowID);

            // HACK: Cutscene audio gets messed up when multitasking on Android :/
#ifdef TARGET_ANDROID
            stdPlatform_Printf("SDL_WINDOWEVENT_MINIMIZED");
            jkCutscene_wasPaused = jkCutscene_55AA54 && jkCutscene_isRendering && std3D_IsReady();
            jkGame_wasDDraw = jkGame_isDDraw;
            if (std3D_IsReady() && jkCutscene_isRendering && !jkCutscene_wasPaused) {
                stdPlatform_Printf("Pause cutscene...\n");
                Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_SPACE, 0);
                Window_msg_main_handler(g_hWnd, WM_CHAR, VK_SPACE, 0);
            }
            else if (std3D_IsReady() && jkGame_isDDraw) {
                stdPlatform_Printf("Pause game...\n");
                Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_ESCAPE, 0);
                Window_msg_main_handler(g_hWnd, WM_CHAR, VK_ESCAPE, 0);
            }
#endif
            break;
        case SDL_WINDOWEVENT_MAXIMIZED:
            stdPlatform_Printf("Window %d maximized", event->window.windowID);
            break;
        case SDL_WINDOWEVENT_RESTORED:
            stdPlatform_Printf("Window %d restored", event->window.windowID);
            
            // HACK: Cutscene audio gets messed up when multitasking on Android :/
#ifdef TARGET_ANDROID
            stdPlatform_Printf("SDL_WINDOWEVENT_RESTORED");
            if (std3D_IsReady() && jkCutscene_isRendering && !jkCutscene_wasPaused) {
                stdPlatform_Printf("Play cutscene...\n");
                Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_SPACE, 0);
                Window_msg_main_handler(g_hWnd, WM_CHAR, VK_SPACE, 0);
            }
            else if (std3D_IsReady() && !jkGame_isDDraw && jkGame_wasDDraw) {
                stdPlatform_Printf("Resume game...\n");
                Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_ESCAPE, 0);
                Window_msg_main_handler(g_hWnd, WM_CHAR, VK_ESCAPE, 0);
            }
#endif
            break;
        case SDL_WINDOWEVENT_ENTER:
            stdPlatform_Printf("Mouse entered window %d\n", event->window.windowID);
            break;
        case SDL_WINDOWEVENT_LEAVE:
            stdPlatform_Printf("Mouse left window %d\n", event->window.windowID);
            break;
        case SDL_WINDOWEVENT_FOCUS_GAINED:
            stdPlatform_Printf("Window %d gained keyboard focus\n", event->window.windowID);
            Window_bNeedsKeyboardFixed = 0;
            break;
        case SDL_WINDOWEVENT_FOCUS_LOST:
            stdPlatform_Printf("Window %d lost keyboard focus\n", event->window.windowID);
            if (stdControl_IsSystemKeyboardShowing() && Window_bNeedsKeyboardFixed) {
                stdPlatform_Printf("Fixing keyboard...\n");
                
                SDL_Window* gimmeKeyboard = SDL_CreateWindow("Gimme Keyboard", 20, 20, 20, 20, SDL_WINDOW_KEYBOARD_GRABBED | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS);
                SDL_RaiseWindow(gimmeKeyboard);
                SDL_DestroyWindow(gimmeKeyboard);
                SDL_RaiseWindow(displayWindow);

                SDL_MinimizeWindow(displayWindow);
                SDL_RestoreWindow(displayWindow);
                SDL_RaiseWindow(displayWindow);
            }
            break;
        case SDL_WINDOWEVENT_CLOSE:
            //printf("Window %d closed", event->window.windowID);
            break;
        case SDL_WINDOWEVENT_TAKE_FOCUS:
            //printf("Window %d is offered a focus", event->window.windowID);
            break;
        case SDL_WINDOWEVENT_HIT_TEST:
            //printf("Window %d has a special hit test", event->window.windowID);
            break;
    }
}

#if defined(WIN64_MINGW) || defined(_WIN32)
CHAR my_getch() {
    DWORD mode, cc;
    DWORD num;
    INPUT_RECORD irInBuf[1];
    HANDLE h = GetStdHandle( STD_INPUT_HANDLE );

    if (h == NULL) {
        return 0; // console not found
    }

    GetConsoleMode( h, &mode );
    SetConsoleMode( h, mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT) );
    TCHAR c = 0;
    GetNumberOfConsoleInputEvents(h, &num);
    if (num)
    {
        if (!ReadConsoleInput(
            h,      // input buffer handle 
            irInBuf,     // buffer to read into 
            1,         // size of read buffer 
            &num))
        {

        }
        else
        {
            if (irInBuf[0].EventType == KEY_EVENT && irInBuf[0].Event.KeyEvent.bKeyDown) {
                c = irInBuf[0].Event.KeyEvent.uChar.AsciiChar;
            }
        }
    }
    SetConsoleMode( h, mode );
    return c;
}

int my_kbhit() {
    DWORD num;
    DWORD mode, cc;
    HANDLE h = GetStdHandle( STD_INPUT_HANDLE );
    if (h == NULL) {
        return 0; // console not found
    }

    GetConsoleMode( h, &mode );
    SetConsoleMode( h, mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT) );
    
    GetNumberOfConsoleInputEvents(h, &num);
    SetConsoleMode( h, mode );

    return num;
}
#else
int my_kbhit() {
    static const int STDIN = 0;
    static int initialized = 0;

    if (! initialized) {
        // Use termios to turn off line buffering
        struct termios term;
        tcgetattr(STDIN, &term);
        term.c_lflag &= ~ICANON;
        term.c_lflag &= ~ECHO;
        tcsetattr(STDIN, TCSANOW, &term);
        setbuf(stdin, NULL);
        initialized = 1;
    }

    int bytesWaiting;
    ioctl(STDIN, FIONREAD, &bytesWaiting);
    return bytesWaiting;
}
#endif

static char Window_headlessBuffer[256];

void Window_UpdateHeadless()
{
    char buffer[32];
    size_t bytes_read = 0;

    if (my_kbhit() > 0) {
#if defined(WIN64_MINGW) || (_WIN32)
        buffer[0] = my_getch();
        buffer[1] = 0;
        bytes_read = 1;
#else
        int fd = STDIN_FILENO;
        bytes_read = read(fd, buffer, sizeof(buffer)-1);
        buffer[bytes_read] = 0;
#endif

        for (int i = 0; i < bytes_read; i++)
        {
            if (buffer[i] == '\n' || buffer[i] == '\r') {
                printf("\r> %s\n", Window_headlessBuffer);
                sithConsole_TryCommand(Window_headlessBuffer);
                memset(Window_headlessBuffer, 0, sizeof(Window_headlessBuffer));
                continue;
            }
            else if (buffer[i] == 0x7F && strlen(Window_headlessBuffer)) {
                Window_headlessBuffer[strlen(Window_headlessBuffer)-1] = 0;
                printf("\r> %s ", Window_headlessBuffer);
                continue;
            }
            else if (buffer[i] < ' ' || buffer[i] > '~')
            {
                continue;
            }

            char tmp[2] = {buffer[i], 0};
            strncat(Window_headlessBuffer, tmp, 255);
        }
    }
    
    printf("\r> %s", Window_headlessBuffer);
    //printf("> %x %x %s\n", buffer[0], my_kbhit(), Window_headlessBuffer);
    fflush(stdout);

    if (Window_resized)
    {
        jkMain_FixRes();
        if (!jkGui_SetModeMenu(0))
        {
            stdDisplay_SetMode(0, 0, 0);
            //jkMain_FixRes();
        }

        jkGui_SetModeGame();
        
        Window_resized = 0;
    }
    
    int sampleTime_roundtrip = SDL_GetTicks() - Window_lastSampleTime;
    //printf("%u\n", sampleTime_roundtrip);
    Window_lastSampleTime = SDL_GetTicks();

    static int sampleTime_delay = 0;
    int menu_framelimit_amt_ms = 6;

    if (!jkGame_isDDraw)
    {

        if (!jkGuiBuildMulti_bRendering) {
            std3D_StartScene();
#ifdef QUAKE_CONSOLE
            jkQuakeConsole_Render();
#endif
            std3D_DrawMenu();
            std3D_EndScene();
            //SDL_GL_SwapWindow(displayWindow);
        }
        else {
#ifdef QUAKE_CONSOLE
            jkQuakeConsole_Render();
#endif
            std3D_DrawMenu();
            //SDL_GL_SwapWindow(displayWindow);
            //menu_framelimit_amt_ms = 64;
        }
    }
    else
    {
        // Save mouse position for menu
        if (jkGame_isDDraw != last_jkGame_isDDraw) {
            Window_menu_mouseX = Window_mouseX;
            Window_menu_mouseY = Window_mouseY;
            Window_lastXRel = 0;
            Window_lastYRel = 0;
        }
    }

    // Keep entire loop at 6ms (150FPS)
    if (sampleTime_roundtrip < menu_framelimit_amt_ms) {
        sampleTime_delay++;
    }
    else {
        sampleTime_delay--;
    }
    if (sampleTime_delay <= 0) {
        sampleTime_delay = 1;
    }
    if (sampleTime_delay >= menu_framelimit_amt_ms) {
        sampleTime_delay = menu_framelimit_amt_ms;
    }
    SDL_Delay(sampleTime_delay);

    last_jkGame_isDDraw = jkGame_isDDraw;
    last_jkQuakeConsole_bOpen = jkQuakeConsole_bOpen;
}

void Window_SdlUpdate()
{
    if (Main_bHeadless)
    {
        Window_UpdateHeadless();
        return;
    }

    uint16_t left, right;
    uint32_t pos, msgl, msgr;
    int hasLeft, hasRight;
    SDL_Event event;
    SDL_MouseButtonEvent* mevent;

    // HACK: Escape key for controllers
    extern int stdControl_bControllerEscapeKey;
    extern int stdControl_bControllerEscapeKey_last;

    while (SDL_PollEvent(&event))
    {
        int bIsOdin = 0;
        int bIsGamepad = 0;

        if (event.type == SDL_JOYBUTTONDOWN || event.type == SDL_JOYBUTTONUP) {
            const char* name = SDL_JoystickNameForIndex(event.jbutton.which);
            bIsOdin = name && strcmp(name, "Odin Controller") == 0;
            bIsGamepad = SDL_IsGameController(event.jbutton.which);
        }
        if (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP) {
            bIsGamepad = 1;
        }

        switch (event.type)
        {
            case SDL_JOYDEVICEADDED: {
                stdControl_bHasJoysticks = 1;
                stdControl_InitSdlJoysticks();
                break;
            }
            case SDL_JOYDEVICEREMOVED: {
                stdControl_InitSdlJoysticks();
                break;
            }

            case SDL_TEXTINPUT:
                for (int i = 0; i < _strlen(event.text.text); i++)
                {
                    Window_msg_main_handler(g_hWnd, WM_CHAR, event.text.text[i], 0);
                }
                break;
            case SDL_WINDOWEVENT:
                Window_HandleWindowEvent(&event);
                break;
            case SDL_KEYDOWN:
                //stdPlatform_Printf("scancode %d\n", event.key.keysym.scancode);
                //handleKey(&event.key.keysym, WM_KEYDOWN, 0x1);
                if (event.key.keysym.sym == SDLK_ESCAPE)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_ESCAPE, event.key.repeat & 0xFFFF);
                    Window_msg_main_handler(g_hWnd, WM_CHAR, VK_ESCAPE, event.key.repeat & 0xFFFF);
                }
                else if (event.key.keysym.sym == SDLK_PAGEUP)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_PRIOR, event.key.repeat & 0xFFFF);
                }
                else if (event.key.keysym.sym == SDLK_PAGEDOWN)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_NEXT, event.key.repeat & 0xFFFF);
                }
                else if (event.key.keysym.sym == SDLK_LEFT)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_LEFT, event.key.repeat & 0xFFFF);
                }
                else if (event.key.keysym.sym == SDLK_RIGHT)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_RIGHT, event.key.repeat & 0xFFFF);
                }
                else if (event.key.keysym.sym == SDLK_UP)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_UP, event.key.repeat & 0xFFFF);
                }
                else if (event.key.keysym.sym == SDLK_DOWN)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_DOWN, event.key.repeat & 0xFFFF);
                }
                else if (event.key.keysym.sym == SDLK_BACKSPACE)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_BACK, event.key.repeat & 0xFFFF);
                    Window_msg_main_handler(g_hWnd, WM_CHAR, VK_BACK, event.key.repeat & 0xFFFF);
                }
                else if (event.key.keysym.sym == SDLK_DELETE)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_DELETE, event.key.repeat & 0xFFFF);
                    //Window_msg_main_handler(g_hWnd, WM_CHAR, VK_DELETE, 0);
                }
                else if (event.key.keysym.sym == SDLK_INSERT)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_INSERT, event.key.repeat & 0xFFFF);
                    Window_msg_main_handler(g_hWnd, WM_CHAR, VK_INSERT, 0);
                }
                else if (event.key.keysym.sym == SDLK_RETURN)
                {
                    // HACK apparently Windows buffers these events in some way, but to replicate the behavior in jkGUI we just spam KEYFIRST
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_RETURN, event.key.repeat & 0xFFFF);
                    Window_msg_main_handler(g_hWnd, WM_CHAR, VK_RETURN, event.key.repeat & 0xFFFF);
                }
                else if (event.key.keysym.sym == SDLK_LSHIFT)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_LSHIFT, event.key.repeat & 0xFFFF);
                }
                else if (event.key.keysym.sym == SDLK_RSHIFT)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_RSHIFT, event.key.repeat & 0xFFFF);
                }
                else if (event.key.keysym.sym == SDLK_TAB)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_TAB, event.key.repeat & 0xFFFF);
                    Window_msg_main_handler(g_hWnd, WM_CHAR, VK_TAB, event.key.repeat & 0xFFFF);
                }
                else if (event.key.keysym.sym == SDLK_END)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_END, event.key.repeat & 0xFFFF);
                    //Window_msg_main_handler(g_hWnd, WM_CHAR, 0x23, 0);
                }
                else if (event.key.keysym.sym == SDLK_HOME)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_HOME, event.key.repeat & 0xFFFF);
                    //Window_msg_main_handler(g_hWnd, WM_CHAR, 0x24, 0);
                }
                else if (event.key.keysym.sym == SDLK_BACKQUOTE)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_OEM_3, event.key.repeat & 0xFFFF);
                }
                else if (event.key.keysym.scancode == SDL_SCANCODE_AC_BACK) {
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_ESCAPE, 0);
                    Window_msg_main_handler(g_hWnd, WM_CHAR, VK_ESCAPE, event.key.repeat & 0xFFFF);
                }

                //if (!event.key.repeat)
                //    stdControl_SetSDLKeydown(event.key.keysym.scancode, 1, event.key.timestamp);
                break;
            case SDL_KEYUP:
                if (event.key.keysym.sym == SDLK_ESCAPE)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_ESCAPE, 0);
                }
                else if (event.key.keysym.sym == SDLK_PAGEUP)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_PRIOR, 0);
                }
                else if (event.key.keysym.sym == SDLK_PAGEDOWN)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_NEXT, 0);
                }
                else if (event.key.keysym.sym == SDLK_LEFT)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_LEFT, 0);
                }
                else if (event.key.keysym.sym == SDLK_RIGHT)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_RIGHT, 0);
                }
                else if (event.key.keysym.sym == SDLK_UP)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_UP, 0);
                }
                else if (event.key.keysym.sym == SDLK_DOWN)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_DOWN, 0);
                }
                else if (event.key.keysym.sym == SDLK_BACKSPACE)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_BACK, 0);
                }
                else if (event.key.keysym.sym == SDLK_DELETE)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_DELETE, 0);
                }
                else if (event.key.keysym.sym == SDLK_INSERT)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_INSERT, 0);
                }
                else if (event.key.keysym.sym == SDLK_RETURN)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_RETURN, 0); // 0xB?
                }
                else if (event.key.keysym.sym == SDLK_LSHIFT)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_LSHIFT, 0);
                }
                else if (event.key.keysym.sym == SDLK_RSHIFT)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_RSHIFT, 0);
                }
                else if (event.key.keysym.sym == SDLK_TAB)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_TAB, 0);
                }
                else if (event.key.keysym.sym == SDLK_END)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_END, 0);
                }
                else if (event.key.keysym.sym == SDLK_HOME)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_HOME, 0);
                }
                else if (event.key.keysym.sym == SDLK_BACKQUOTE)
                {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_OEM_3, 0);
                }
                else if (event.key.keysym.scancode == SDL_SCANCODE_AC_BACK) {
                    Window_msg_main_handler(g_hWnd, WM_KEYUP, VK_ESCAPE, 0);
                }
                //handleKey(&event.key.keysym, WM_KEYUP, 0xc0000001);

                if (jkQuakeConsole_bOpen) break; // Hijack all input to console

                stdControl_SetSDLKeydown(event.key.keysym.scancode, 0, event.key.timestamp);
                break;
            case SDL_MOUSEMOTION:
                Window_HandleMouseMove(&event.motion);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:

                mevent = (SDL_MouseButtonEvent*)&event;
                left = 0;
                right = 0;
                hasLeft = 0;
                hasRight = 0;
                if (event.type == SDL_MOUSEBUTTONDOWN)
                {
                    left = (mevent->button == SDL_BUTTON_LEFT ? 1 : 0);
                    right = (mevent->button == SDL_BUTTON_RIGHT ? 2 : 0);
                    
                    if (left)
                        hasLeft = 1;
                    if (right)
                        hasRight = 1;
                }
                else if (event.type == SDL_MOUSEBUTTONUP)
                {
                    left = (mevent->button == SDL_BUTTON_LEFT ? 0 : 1);
                    right = (mevent->button == SDL_BUTTON_RIGHT ? 0 : 2);
                    
                    if (!left)
                        hasLeft = 1;
                    if (!right)
                        hasRight = 1;
                }
                
                if (hasLeft)
                    Window_bMouseLeft = left;
                if (hasRight)
                    Window_bMouseRight = right;

                Window_mouseX = mevent->x;
                Window_mouseY = mevent->y;// - (Window_ySize - 480);

                pos = ((Window_mouseX) & 0xFFFF) | (((Window_mouseY) << 16) & 0xFFFF0000);
                msgl = (event.type == SDL_MOUSEBUTTONDOWN ? WM_LBUTTONDOWN : WM_LBUTTONUP);
                msgr = (event.type == SDL_MOUSEBUTTONDOWN ? WM_RBUTTONDOWN : WM_RBUTTONUP);

                if (jkQuakeConsole_bOpen) break; // Hijack all input to console
                
                if (hasLeft)
                    Window_msg_main_handler(g_hWnd, msgl, left | right, pos);
                if (hasRight)
                    Window_msg_main_handler(g_hWnd, msgr, left | right, pos);

                //stdControl_SetKeydown(KEY_MOUSE_B1, Window_bMouseLeft, mevent->timestamp);
                //stdControl_SetKeydown(KEY_MOUSE_B2, Window_bMouseRight, mevent->timestamp);

                break;
            case SDL_MOUSEWHEEL:
                Window_mouseWheelY = event.wheel.y;
                Window_mouseWheelX = event.wheel.x;

                if (jkQuakeConsole_bOpen) break; // Hijack all input to console
                break;

            // HACK: Escape key for controllers
            case SDL_JOYBUTTONDOWN:
            case SDL_JOYBUTTONUP:
                if (!bIsGamepad) {
                    //stdPlatform_Printf("button %d, %d\n", event.jbutton.button, event.jbutton.state);
                }
                if (bIsOdin && !bIsGamepad && (event.jbutton.button == 6 || event.jbutton.button == 4)) {
                    stdControl_bControllerEscapeKey = (event.jbutton.state == SDL_PRESSED);
                }
                else if (!bIsGamepad && jkCutscene_isRendering && event.type == SDL_JOYBUTTONDOWN && event.jbutton.button == 3) { // y
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_SPACE, 0);
                    Window_msg_main_handler(g_hWnd, WM_CHAR, VK_SPACE, 0);
                }
                else if (!bIsGamepad && jkCutscene_isRendering  && event.type == SDL_JOYBUTTONDOWN&& event.jbutton.button == 2) { // x
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_SPACE, 0);
                    Window_msg_main_handler(g_hWnd, WM_CHAR, VK_SPACE, 0);
                }
                else if (!bIsGamepad && jkCutscene_isRendering && event.type == SDL_JOYBUTTONDOWN && event.jbutton.button == 1) { // b
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_ESCAPE, 0);
                    Window_msg_main_handler(g_hWnd, WM_CHAR, VK_ESCAPE, 0);
                }
                else if (!bIsGamepad && jkCutscene_isRendering && event.type == SDL_JOYBUTTONDOWN && event.jbutton.button == 0) { // a
                    Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_ESCAPE, 0);
                    Window_msg_main_handler(g_hWnd, WM_CHAR, VK_ESCAPE, 0);
                }
                break;

            case SDL_JOYAXISMOTION:
                if (event.jaxis.which == 0) {
                    //stdPlatform_Printf("axis %d, %d\n", event.jaxis.axis, event.jaxis.value);
                }
                break;

            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP:
                if (bIsGamepad) {
                    //stdPlatform_Printf("gpad button %d, %d\n", event.cbutton.button, event.cbutton.state);
                    if (event.cbutton.button == SDL_CONTROLLER_BUTTON_START || event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
                        stdControl_bControllerEscapeKey = (event.cbutton.state == SDL_PRESSED);
                    }
                    else if (jkCutscene_isRendering && event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == SDL_CONTROLLER_BUTTON_Y) { // y
                        Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_SPACE, 0);
                        Window_msg_main_handler(g_hWnd, WM_CHAR, VK_SPACE, 0);
                    }
                    else if (jkCutscene_isRendering  && event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == SDL_CONTROLLER_BUTTON_X) { // x
                        Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_SPACE, 0);
                        Window_msg_main_handler(g_hWnd, WM_CHAR, VK_SPACE, 0);
                    }
                    else if (jkCutscene_isRendering && event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == SDL_CONTROLLER_BUTTON_B) { // b
                        Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_ESCAPE, 0);
                        Window_msg_main_handler(g_hWnd, WM_CHAR, VK_ESCAPE, 0);
                    }
                    else if (jkCutscene_isRendering && event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == SDL_CONTROLLER_BUTTON_A) { // a
                        Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_ESCAPE, 0);
                        Window_msg_main_handler(g_hWnd, WM_CHAR, VK_ESCAPE, 0);
                    }
                }
                break;
            case SDL_CONTROLLERAXISMOTION:
                //stdPlatform_Printf("Controller %d Axis %d moved to %d\n", 
                //       event.caxis.which, event.caxis.axis, event.caxis.value);
                break;

            case SDL_QUIT:
                stdPlatform_Printf("Quit!\n");

                // Added
                if (jkPlayer_bHasLoadedSettingsOnce) {
                    jkPlayer_WriteConf(jkPlayer_playerShortName);
                }
                
                exit(-1);
                break;
            default:
                break;
        }
    }

    // HACK: Escape key for controllers
    if (stdControl_bControllerEscapeKey && !stdControl_bControllerEscapeKey_last) {
        Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_ESCAPE, 0);
        Window_msg_main_handler(g_hWnd, WM_CHAR, VK_ESCAPE, 0);
    }
    stdControl_bControllerEscapeKey_last = stdControl_bControllerEscapeKey;
    
    if (Window_resized)
    {
        jkMain_FixRes();
        if (!jkGui_SetModeMenu(0))
        {
            stdDisplay_SetMode(0, 0, 0);
            //jkMain_FixRes();
        }
        
        Window_resized = 0;
    }
    
    static int sampleTime_delay = 0;
    int sampleTime_roundtrip = SDL_GetTicks() - Window_lastSampleTime;
    //printf("%u\n", sampleTime_roundtrip);
    Window_lastSampleTime = SDL_GetTicks();

    static int jkPlayer_enableVsync_last = 0;
    int menu_framelimit_amt_ms = 16;

    if (jkPlayer_enableVsync_last != jkPlayer_enableVsync)
    {
        SDL_GL_SetSwapInterval(jkPlayer_enableVsync);
    }

    if (!jkGame_isDDraw)
    {
        // Restore menu mouse position
        if (jkGame_isDDraw != last_jkGame_isDDraw) {
            SDL_WarpMouseInWindow(displayWindow, Window_menu_mouseX, Window_menu_mouseY);
        }

        SDL_SetRelativeMouseMode(SDL_FALSE);

        if (!jkGuiBuildMulti_bRendering) {
            std3D_StartScene();
#ifdef QUAKE_CONSOLE
            jkQuakeConsole_Render();
#endif
            std3D_DrawMenu();
            std3D_EndScene();
            SDL_GL_SwapWindow(displayWindow);
        }
        else {
#ifdef QUAKE_CONSOLE
            jkQuakeConsole_Render();
#endif
            std3D_DrawMenu();
            SDL_GL_SwapWindow(displayWindow);
            //menu_framelimit_amt_ms = 64;
        }

        if (Window_needsRecreate) {
            std3D_PurgeEntireTextureCache();
            Window_RecreateSDL2Window();
        }
        
        // Keep menu FPS at 60FPS, to avoid cranking the GPU unnecessarily.
        if (sampleTime_roundtrip < menu_framelimit_amt_ms) {
            sampleTime_delay++;
        }
        else {
            sampleTime_delay--;
        }
        if (sampleTime_delay <= 0) {
            sampleTime_delay = 1;
        }
        if (sampleTime_delay >= menu_framelimit_amt_ms) {
            sampleTime_delay = menu_framelimit_amt_ms;
        }
        SDL_Delay(sampleTime_delay);
    }
    else
    {
        // Save mouse position for menu
        if (jkGame_isDDraw != last_jkGame_isDDraw) {
            Window_menu_mouseX = Window_mouseX;
            Window_menu_mouseY = Window_mouseY;
            Window_lastXRel = 0;
            Window_lastYRel = 0;
        }

#ifdef QUAKE_CONSOLE

        if (jkQuakeConsole_bOpen && jkQuakeConsole_bOpen != last_jkQuakeConsole_bOpen) {
            SDL_WarpMouseInWindow(displayWindow, Window_menu_mouseX, Window_menu_mouseY);
        }
        else if (!jkQuakeConsole_bOpen && jkQuakeConsole_bOpen != last_jkQuakeConsole_bOpen) {
            Window_menu_mouseX = Window_mouseX;
            Window_menu_mouseY = Window_mouseY;
            Window_lastXRel = 0;
            Window_lastYRel = 0;
        }

        if (jkQuakeConsole_bOpen)
        {
            SDL_SetRelativeMouseMode(SDL_FALSE);
        }

        if (!jkQuakeConsole_bOpen && SDL_GetWindowFlags(displayWindow) & SDL_WINDOW_MOUSE_FOCUS) {
            SDL_SetRelativeMouseMode(SDL_TRUE);
            //SDL_WarpMouseInWindow(displayWindow, 100, 100);
        }
        else
        {
            SDL_SetRelativeMouseMode(SDL_FALSE);
        }
#endif
    }

    jkPlayer_enableVsync_last = jkPlayer_enableVsync;

    last_jkGame_isDDraw = jkGame_isDDraw;
#ifdef QUAKE_CONSOLE
    last_jkQuakeConsole_bOpen = jkQuakeConsole_bOpen;
#endif
}

void Window_SdlVblank()
{
    if (Main_bHeadless) return;

    //static uint32_t roundtrip = 0;
    //uint32_t before = stdPlatform_GetTimeMsec();
#ifdef ARCH_WASM
    if (!jkGuiBuildMulti_bRendering)
#endif
    SDL_GL_SwapWindow(displayWindow);
    //uint32_t after = stdPlatform_GetTimeMsec();
    //printf("%u %u\n", after-before, before-roundtrip);

    //roundtrip = before;

    if (Window_needsRecreate)
        Window_RecreateSDL2Window();

#ifdef ARCH_WASM
    //emscripten_sleep(1);
#endif
}

#ifdef ARCH_WASM
EM_JS(int, canvas_get_width, (), {
  return canvas.width;
});

EM_JS(int, canvas_get_height, (), {
  return canvas.height;
});
#endif

void Window_RecreateSDL2Window()
{
    openjkdf2_trace("Window_RecreateSDL2Window: enter");
#ifdef ARCH_WASM
    static int onlyOnce = 0;
    if (onlyOnce) {
        return;
    }
    onlyOnce = 1;
#endif

    if (Main_bHeadless) return;

    stdPlatform_Printf("Recreating SDL2 Window!\n");
    Window_needsRecreate = 0;

    if (displayWindow) {
        openjkdf2_trace("Window_RecreateSDL2Window: destroy old");
        std3D_FreeResources();
        SDL_GL_DeleteContext(glWindowContext);
        SDL_DestroyWindow(displayWindow);
        displayWindow = NULL;
        glWindowContext = NULL;
    }

    // HACK: side-step the json stuff
    if (Window_bShouldPopSteamKeyboard) {
        Window_isFullscreen = 1;
        Window_isHiDpi = 1;
    }

    int flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;

    if (displayWindow) {
        flags = SDL_GetWindowFlags(displayWindow);
        //std3D_FreeResources();
        //SDL_GL_DeleteContext(glWindowContext);
        //SDL_DestroyWindow(displayWindow);

        flags |= SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
    }

#ifdef WIN64_STANDALONE
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif

    if (Window_isHiDpi)
        flags |= SDL_WINDOW_ALLOW_HIGHDPI;
    else
        flags &= ~SDL_WINDOW_ALLOW_HIGHDPI;

    if (Window_isFullscreen) {
        //flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    else {
        //flags &= ~SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

#if defined(ARCH_WASM)
    //flags &= ~SDL_WINDOW_RESIZABLE;
#endif

#if defined(TARGET_ANDROID) || defined(TARGET_LINUX_GLES)
    /* OPENGL required for SDL_GL_CreateContext (Wayland/ROCKNIX rejects GL on non-GL windows). */
    flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif

#ifdef ARCH_WASM
    displayWindow = SDL_CreateWindow(Window_isHiDpi ? "OpenJKDF2 HiDPI" : "OpenJKDF2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, canvas_get_width(), canvas_get_height(), flags);
#elif defined(TARGET_ANDROID) || defined(TARGET_LINUX_GLES)
    displayWindow = SDL_CreateWindow(Window_isHiDpi ? "OpenJKDF2 HiDPI" : "OpenJKDF2", 0, 0, Window_screenXSize, Window_screenYSize, flags);
#else
    displayWindow = SDL_CreateWindow(Window_isHiDpi ? "OpenJKDF2 HiDPI" : "OpenJKDF2", Window_xPos, Window_yPos, Window_screenXSize, Window_screenYSize, flags);
#endif
    if (!displayWindow) {
        char errtmp[256];
        snprintf(errtmp, 256, "!! Failed to create SDL2 window !!\n%s", SDL_GetError());
        openjkdf2_trace("Window_RecreateSDL2Window: SDL_CreateWindow failed");
        fprintf(stderr, "%s\n", errtmp);
        fflush(stderr);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", errtmp, NULL);
        exit (-1);
    }
    openjkdf2_trace("Window_RecreateSDL2Window: SDL_CreateWindow ok");
    //SDL_SetRenderDrawBlendMode(displayRenderer, SDL_BLENDMODE_BLEND);

#if defined(MACOS) && defined(__aarch64__)
    //SDL_FixWindowMacOS(displayWindow);
#endif

    if (Window_isFullscreen) {
        SDL_SetWindowFullscreen(displayWindow, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
    else {
        SDL_SetWindowFullscreen(displayWindow, 0);
    }
    SDL_RaiseWindow(displayWindow);

    glWindowContext = SDL_GL_CreateContext(displayWindow);

#if defined(TARGET_ANDROID) || defined(TARGET_LINUX_GLES)
    // GLES fallback: 3.0 ES -> 2.0 ES (no desktop GL CORE on Mali/KMSDRM)
    if (glWindowContext == NULL)
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
        glWindowContext = SDL_GL_CreateContext(displayWindow);
        if (glWindowContext) {
            openjkdf2_trace("Window_RecreateSDL2Window: GLES 2.0 context");
        }
    }
#else
    // Retry with 3.30 instead
    if (glWindowContext == NULL)
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
        glWindowContext = SDL_GL_CreateContext(displayWindow);
    }

    // Retry with 3.20 and this thing instead
    if (glWindowContext == NULL)
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
        glWindowContext = SDL_GL_CreateContext(displayWindow);
    }
#endif
    
    if (glWindowContext == NULL)
    {
        char errtmp[256];
        snprintf(errtmp, 256, "!! Failed to initialize SDL OpenGL context !!\n%s", SDL_GetError());
        openjkdf2_trace("Window_RecreateSDL2Window: SDL_GL_CreateContext failed");
        fprintf(stderr, "%s\n", errtmp);
        fflush(stderr);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", errtmp, NULL);
        exit(-1);
    }

    openjkdf2_trace("Window_RecreateSDL2Window: GL context ok");
    if (SDL_GL_MakeCurrent(displayWindow, glWindowContext) != 0) {
        openjkdf2_trace_fmt("Window_RecreateSDL2Window: MakeCurrent failed: %s", SDL_GetError());
    }
#if defined(TARGET_LINUX_GLES)
    {
        int gl_major = 0, gl_minor = 0, gl_profile = 0;
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &gl_major);
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &gl_minor);
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &gl_profile);
        openjkdf2_trace_fmt("Window_RecreateSDL2Window: context %d.%d profile=0x%x",
            gl_major, gl_minor, gl_profile);
        if (!gles_loader_init()) {
            openjkdf2_trace("Window_RecreateSDL2Window: gles_loader_init failed");
            fprintf(stderr, "OpenJKDF2: GLES loader init failed (missing GL symbols)\n");
            fflush(stderr);
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error",
                "GLES loader init failed. See log.txt / startup.log.", NULL);
            exit(-1);
        }
        {
            const GLubyte *ver = glGetString(GL_VERSION);
            const GLubyte *renderer = glGetString(GL_RENDERER);
            openjkdf2_trace_fmt("Window_RecreateSDL2Window: GL_VERSION=%s",
                ver ? (const char*)ver : "null");
            openjkdf2_trace_fmt("Window_RecreateSDL2Window: GL_RENDERER=%s",
                renderer ? (const char*)renderer : "null");
            stdPlatform_Printf("OpenJKDF2: GL %s (%s)\n",
                ver ? (const char*)ver : "unknown",
                renderer ? (const char*)renderer : "unknown");
        }
    }
#endif
    SDL_GL_SetSwapInterval(jkPlayer_enableVsync); // Disable vsync
#if !defined(TARGET_ANDROID)
    SDL_StartTextInput();
#endif

    SDL_GL_GetDrawableSize(displayWindow, &Window_xSize, &Window_ySize);
    SDL_GetWindowSize(displayWindow, &Window_screenXSize, &Window_screenYSize);
    if (openjkdf2_IsHandheld())
        Window_ApplyForcedResolution();
    else {
        if (Window_xSize < 640) Window_xSize = 640;
        if (Window_ySize < 480) Window_ySize = 480;
    }

    Window_resized = 1;
    openjkdf2_trace("Window_RecreateSDL2Window: done");
}

void Window_Main_Loop()
{
    openjkdf2_trace("Window_Main_Loop: enter");
    jkMain_GuiAdvance(); // TODO needed?
    openjkdf2_trace("Window_Main_Loop: after GuiAdvance");
    Window_msg_main_handler(g_hWnd, WM_PAINT, 0, 0);
    openjkdf2_trace("Window_Main_Loop: after WM_PAINT");
    
    //Window_SdlUpdate();
}

int Window_Main_Linux(int argc, char** argv)
{
    char cmdLine[1024];
    int result;

    // Init SDL
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    openjkdf2_InitHandheldMode();
    {
        const char *app_name = getenv("SDL_HINT_APP_NAME");
        if (!app_name || !app_name[0])
            app_name = "OpenJKDF2";
        SDL_SetHint(SDL_HINT_APP_NAME, app_name);
    }

#if defined(TARGET_LINUX_GLES)
    /* GLES-only binary: force ES driver when desktop GL is also available (ROCKNIX). */
    if (!getenv("SDL_OPENGL_ES_DRIVER"))
        SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
#endif

#if defined(TARGET_ANDROID)
    //SDL_SetHint(SDL_HINT_JOYSTICK_DEBUG, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "1");
    //SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "1");
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint("SDL_MIXER_DEBUG_MUSIC_INTERFACES", "1");
    SDL_SetHint(SDL_HINT_AUDIODRIVER, "aaudio"); // This is fine for music tbh
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif

    openjkdf2_trace("Window_Main_Linux: SDL_Init");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_NOPARACHUTE | SDL_INIT_GAMECONTROLLER) < 0) {
        openjkdf2_trace("Window_Main_Linux: SDL_Init failed");
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    openjkdf2_trace("Window_Main_Linux: SDL_Init ok");

    if (openjkdf2_IsHandheld()) {
        const char* w_env = getenv("DISPLAY_WIDTH");
        const char* h_env = getenv("DISPLAY_HEIGHT");
        if (w_env && h_env) {
            int w = atoi(w_env);
            int h = atoi(h_env);
            if (w > 0 && h > 0) {
                Window_screenXSize = w;
                Window_screenYSize = h;
                Window_xSize = w;
                Window_ySize = h;
                Window_isFullscreen = 1;
            }
        }
    }

    if ((SDL_GetHintBoolean("SteamClientLaunch", 0) || SDL_GetHintBoolean("SteamOS", 0) || SDL_GetHintBoolean("SteamDeck", 0)) && SDL_GetHintBoolean("SteamGamepadUI", 0)) {
        Window_bShouldPopSteamKeyboard = 1;
        Window_isFullscreen = 1;
        Window_isHiDpi = 1;
    }

#if defined(MACOS)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
#else

#if defined(WIN64_STANDALONE)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);

    // apitrace
#if 0
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
#endif
#elif defined(TARGET_ANDROID) || defined(TARGET_LINUX_GLES)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
#elif defined(ARCH_WASM)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
#endif

#endif

    openjkdf2_trace("Window_Main_Linux: Window_RecreateSDL2Window");
    Window_RecreateSDL2Window();
    openjkdf2_trace("Window_Main_Linux: GL context ok");
#if !defined(TARGET_ANDROID) && !defined(TARGET_LINUX_GLES) && !defined(ARCH_WASM)
    glewInit();
#endif
    
    //SDL_RenderClear(displayRenderer);
    //SDL_RenderPresent(displayRenderer);
    
    
    strcpy(cmdLine, "");
    
    g_handler_count = 0;
    g_thing_two_some_dialog_count = 0;
    g_should_exit = 0;
    g_window_not_destroyed = 0;
    g_hInstance = 0;//hInstance;
    g_nShowCmd = 0;//nShowCmd;
    
    for (int i = 1; i < argc; i++)
    {
        strcat(cmdLine, argv[i]);
        strcat(cmdLine, " ");
    }
    
    openjkdf2_trace("Window_Main_Linux: Main_Startup");
    result = Main_Startup(cmdLine);
    openjkdf2_trace("Window_Main_Linux: Main_Startup done");

    openjkdf2_trace("Window_Main_Linux: registry read");
    int fullscreen = wuRegistry_GetBool("Window_isFullscreen", Window_isFullscreen);
    int hidpi = wuRegistry_GetBool("Window_isHiDpi", Window_isHiDpi);
    openjkdf2_trace("Window_Main_Linux: apply window settings");
    Window_SetFullscreen(fullscreen);
    Window_SetHiDpi(hidpi);
    if (openjkdf2_IsHandheld()) {
        /* First create already applied panel size; avoid tearing down GL before first frame. */
        if (Window_needsRecreate) {
            openjkdf2_trace("Window_Main_Linux: Window_RecreateSDL2Window (post-startup)");
            Window_RecreateSDL2Window();
        } else {
            openjkdf2_trace("Window_Main_Linux: skip post-startup recreate");
        }
    } else {
        openjkdf2_trace("Window_Main_Linux: Window_RecreateSDL2Window (post-startup)");
        Window_RecreateSDL2Window();
    }
    openjkdf2_trace("Window_Main_Linux: window ready");

    if (!result) return result;

    if (Main_bHeadless)
    {
        if (displayWindow) {
            std3D_FreeResources();
            SDL_GL_DeleteContext(glWindowContext);
            SDL_DestroyWindow(displayWindow);
        }
    }

    g_window_not_destroyed = 1;
    
    openjkdf2_trace("Window_Main_Linux: WM_CREATE");
    Window_msg_main_handler(g_hWnd, 0x1, 0, 0); // WM_CREATE
    openjkdf2_trace("Window_Main_Linux: WM_ACTIVATE");
    Window_msg_main_handler(g_hWnd, 0x6, 2, 0); // WM_ACTIVATE
    openjkdf2_trace("Window_Main_Linux: WM_ACTIVATEAPP");
    Window_msg_main_handler(g_hWnd, 0x1C, 1, 0); // WM_ACTIVATEAPP
    openjkdf2_trace("Window_Main_Linux: WM_SHOWWINDOW");
    Window_msg_main_handler(g_hWnd, 0x18, 0, 0); // WM_SHOWWINDOW
    openjkdf2_trace("Window_Main_Linux: WM_PAINT (bootstrap)");
    Window_msg_main_handler(g_hWnd, WM_PAINT, 0, 0);
    openjkdf2_trace("Window_Main_Linux: main loop");


#ifdef ARCH_WASM
    //int fps = 0; // Use browser's requestAnimationFrame
    //emscripten_set_main_loop_arg(Window_Main_Loop, NULL, fps, 1);
    while (1)
    {
        Window_Main_Loop();
        if (g_should_exit) break;
    }
#else
    while (1)
    {
        Window_Main_Loop();
        if (g_should_exit) break;
    }
#endif

    // Added
    if (jkPlayer_bHasLoadedSettingsOnce) {
        jkPlayer_WriteConf(jkPlayer_playerShortName);
    }

    Main_Shutdown();
    return 1;
}

int Window_Main(HINSTANCE hInstance, int a2, char *lpCmdLine, int nShowCmd, LPCSTR lpWindowName)
{
    int result;

    g_handler_count = 0;
    g_thing_two_some_dialog_count = 0;
    g_should_exit = 0;
    g_window_not_destroyed = 0;
    g_hInstance = hInstance;
    g_nShowCmd = nShowCmd;
#if 0
    if (jk_RegisterClassExA(&wndClass))
    {
        if ( jk_FindWindowA("wKernel", lpWindowName) )
            jk_exit(-1);

        uint32_t hres = jk_GetSystemMetrics(1);
        uint32_t vres = jk_GetSystemMetrics(0);
        g_hWnd = jk_CreateWindowExA(0x40000u, "wKernel", lpWindowName, 0x90000000, 0, 0, vres, hres, 0, 0, hInstance, 0);

        if (g_hWnd)
        {
            g_hInstance = hInstance;
            jk_ShowWindow(g_hWnd, 1);
            jk_UpdateWindow(g_hWnd);
        }
    }

    stdGdi_SetHwnd(g_hWnd);
    stdGdi_SetHInstance(g_hInstance);
    jk_InitCommonControls();

    g_855E8C = 2 * jk_GetSystemMetrics(32);
    uint32_t metrics_32 = jk_GetSystemMetrics(32);
    g_855E90 = jk_GetSystemMetrics(15) + 2 * metrics_32;
    result = Main_Startup(lpCmdLine);

    if (!result) return result;

    
    g_window_not_destroyed = 1;

    while (1)
    {
        if (jk_PeekMessageA(&msg, 0, 0, 0, 0))
        {
            if (!jk_GetMessageA(&msg, 0, 0, 0))
            {
                result = msg.wParam;
                g_should_exit = 1;
                break;
            }

            uint32_t some_cnt = 0;
            if (g_thing_two_some_dialog_count > 0)
            {
#if 0
                v16 = &thing_three;
                do
                {
                    //TODO if ( jk_IsDialogMessageA(*v16, &msg) )
                    //  break;
                    ++some_cnt;
                    ++v16;
                }
                while ( some_cnt < g_thing_two_some_dialog_count );
#endif
            }

            if (some_cnt == g_thing_two_some_dialog_count)
            {
                jk_TranslateMessage(&msg);
                jk_DispatchMessageA(&msg);
            }

            if (!jk_PeekMessageA(&msg, 0, 0, 0, 0))
            {
                result = 0;
                if ( g_should_exit )
                    return result;
            }
        }

        //if (user32->stopping) break;

        jkMain_GuiAdvance();
    }
#endif
    result = 1;
    return result;
}

int Window_ShowCursorUnwindowed(int a1)
{
    return stdControl_ShowCursor(a1);
}

int Window_DefaultHandler(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam, void* unused)
{
    return 0;
}

int Window_MessageLoop()
{
    // Added: controller menuing
    jkGuiRend_UpdateController();

    jkMain_GuiAdvance();
    Window_msg_main_handler(g_hWnd, WM_PAINT, 0, 0);
    
    //Window_SdlUpdate();
    return 0;
}

#endif // SDL2_RENDER

void Window_SetDrawHandlers(WindowDrawHandler_t a1, WindowDrawHandler_t a2)
{
    Window_drawAndFlip = a1;
    Window_setCooperativeLevel = a2;
}

void Window_GetDrawHandlers(WindowDrawHandler_t *a1, WindowDrawHandler_t *a2)
{
    *a1 = Window_drawAndFlip;
    *a2 = Window_setCooperativeLevel;
}
