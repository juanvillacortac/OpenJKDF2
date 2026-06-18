#include "Platform/linux_display.h"

#include "SDL2_helper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(TARGET_LINUX)
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/kd.h>
#include <linux/vt.h>

static int openjkdf2_linux_kms_display;

static int openjkdf2_linux_is_bare_vt(void)
{
    const char *display = getenv("DISPLAY");
    const char *wayland = getenv("WAYLAND_DISPLAY");

    if ((display && display[0]) || (wayland && wayland[0]))
        return 0;

    {
        const char *tty = ttyname(STDERR_FILENO);
        if (!tty)
            tty = ttyname(STDOUT_FILENO);
        if (!tty || strncmp(tty, "/dev/tty", 8) != 0)
            return 0;
        for (const char *n = tty + 8; *n; ++n) {
            if (*n < '0' || *n > '9')
                return 0;
        }
    }

    return 1;
}

static int openjkdf2_linux_drm_usable(void)
{
    return access("/dev/dri/card0", R_OK | W_OK) == 0
        || access("/dev/dri/card1", R_OK | W_OK) == 0;
}

void openjkdf2_InitLinuxDisplayEnv(void)
{
#if defined(TARGET_LINUX)
    const char *video_env = getenv("SDL_VIDEODRIVER");

    openjkdf2_linux_kms_display = 0;

    if (video_env && video_env[0])
        return;

    if (!openjkdf2_linux_is_bare_vt())
        return;

    openjkdf2_linux_kms_display = 1;
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "kmsdrm");

    if (!openjkdf2_linux_drm_usable()) {
        fprintf(stderr,
            "OpenJKDF2: Linux VT without X11/Wayland; /dev/dri/card* is not accessible.\n"
            "Add your user to the video group, then log in again.\n");
        return;
    }

    fprintf(stderr, "OpenJKDF2: Linux VT detected — using kmsdrm + GLES\n");
#else
    openjkdf2_linux_kms_display = 0;
#endif
}

int openjkdf2_IsLinuxKmsDisplay(void)
{
    return openjkdf2_linux_kms_display;
}

void openjkdf2_RestoreLinuxConsole(void)
{
    struct vt_mode vt;
    struct vt_stat state;
    int fd;

    if (!openjkdf2_linux_kms_display)
        return;

    fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return;

    ioctl(fd, KDSETMODE, KD_TEXT);

    memset(&vt, 0, sizeof(vt));
    vt.mode = VT_AUTO;
    ioctl(fd, VT_SETMODE, &vt);

    if (ioctl(fd, VT_GETSTATE, &state) == 0)
        ioctl(fd, VT_ACTIVATE, state.v_active);

    close(fd);

    /* Redraw login prompt on the active VT. */
    fputs("\033c", stderr);
    fflush(stderr);
}

#else

void openjkdf2_InitLinuxDisplayEnv(void)
{
}

int openjkdf2_IsLinuxKmsDisplay(void)
{
    return 0;
}

void openjkdf2_RestoreLinuxConsole(void)
{
}

#endif
