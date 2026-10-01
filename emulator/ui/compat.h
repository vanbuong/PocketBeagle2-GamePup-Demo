/* SPDX-License-Identifier: GPL-2.0-only */
/* Windows (MinGW) shims so the desktop simulator builds there. The device
 * build is Linux-only and does not use this. */
#ifndef GAMEPUP_COMPAT_H
#define GAMEPUP_COMPAT_H

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#define mkdir(path, mode) _mkdir(path)
#define localtime_r(timer, result) (localtime_s((result), (timer)) ? NULL : (result))
#define strtok_r(str, delim, save) strtok_s((str), (delim), (save))
#ifndef SIGKILL
#define SIGKILL 9
#endif
#ifndef SIGSTOP
#define SIGSTOP 19
#endif
#ifndef SIGCONT
#define SIGCONT 18
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif

/* Linux input event codes used by the cape buttons. */
#define KEY_ESC 1
#define KEY_1 2
#define KEY_5 6
#define KEY_TAB 15
#define KEY_P 25
#define KEY_ENTER 28
#define KEY_UP 103
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_DOWN 108
#endif

#endif
