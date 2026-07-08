/*
 * ffpdf — read and fill PDF form fields.
 * Copyright (C) 2026 Page Brooks
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy
 * of the License at <http://www.apache.org/licenses/LICENSE-2.0>. Distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND; see the
 * License for the specific language governing permissions and limitations.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "os_compat.h"

#ifdef _WIN32

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600   // GetFileInformationByHandleEx (Vista+)
#endif
#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW
#include <io.h>         // _fileno, _get_osfhandle
#include <stdlib.h>
#include <wchar.h>

// Convert a UTF-8 string to a freshly allocated UTF-16 string (caller frees).
static wchar_t *utf8_to_utf16(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

FILE *portable_fopen(const char *path_utf8, const char *mode) {
    wchar_t *wpath = utf8_to_utf16(path_utf8);
    if (!wpath) return NULL;
    wchar_t wmode[16];
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode,
                        (int)(sizeof(wmode) / sizeof(wmode[0])));
    FILE *f = _wfopen(wpath, wmode);
    free(wpath);
    return f;
}

// _isatty() is wrong here: it reports ANY character device as a tty, including
// NUL — so `fill > /dev/null` (or `> NUL`) would be refused. A real console
// answers GetConsoleMode; an MSYS2/Cygwin (mintty) terminal is a named pipe
// whose name looks like "\msys-<hex>-pty0-to-master".
int portable_isatty_stdout(void) {
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(stdout));
    DWORD mode;
    if (h == INVALID_HANDLE_VALUE || h == NULL) return 0;
    if (GetConsoleMode(h, &mode)) return 1;          // real Win32 console
    if (GetFileType(h) == FILE_TYPE_PIPE) {          // maybe an msys/cygwin pty
        struct { FILE_NAME_INFO info; wchar_t pad[MAX_PATH]; } u;
        if (GetFileInformationByHandleEx(h, FileNameInfo, &u, sizeof u)) {
            wchar_t name[MAX_PATH + 1];
            DWORD n = u.info.FileNameLength / sizeof(wchar_t);
            if (n > MAX_PATH) n = MAX_PATH;
            wmemcpy(name, u.info.FileName, n);
            name[n] = L'\0';
            if ((wcsstr(name, L"msys-") || wcsstr(name, L"cygwin-")) &&
                wcsstr(name, L"-pty"))
                return 1;
        }
    }
    return 0;
}

int portable_rename(const char *from_utf8, const char *to_utf8) {
    wchar_t *wfrom = utf8_to_utf16(from_utf8);
    wchar_t *wto   = utf8_to_utf16(to_utf8);
    int ok = wfrom && wto &&
             MoveFileExW(wfrom, wto, MOVEFILE_REPLACE_EXISTING);
    free(wfrom);
    free(wto);
    return ok ? 0 : -1;
}

int portable_remove(const char *path_utf8) {
    wchar_t *wpath = utf8_to_utf16(path_utf8);
    if (!wpath) return -1;
    int rc = _wremove(wpath);
    free(wpath);
    return rc;
}

FILE *os_temp_file(char *path_out, size_t cap) {
    wchar_t wdir[MAX_PATH], wpath[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, wdir);
    if (n == 0 || n >= MAX_PATH) return NULL;
    if (GetTempFileNameW(wdir, L"ffp", 0, wpath) == 0) return NULL;
    int u = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path_out, (int)cap,
                                NULL, NULL);
    if (u <= 0) { _wremove(wpath); return NULL; }
    FILE *f = _wfopen(wpath, L"wb");
    if (!f) _wremove(wpath);
    return f;
}

void os_init_utf8_args(int *argc, char ***argv) {
    int wargc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv) return;                         // keep the ANSI argv on failure

    char **uargv = calloc((size_t)wargc + 1, sizeof(char *));
    if (!uargv) { LocalFree(wargv); return; }

    for (int i = 0; i < wargc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        uargv[i] = malloc(n > 0 ? (size_t)n : 1);
        if (uargv[i] && n > 0)
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, uargv[i], n, NULL, NULL);
        else if (uargv[i])
            uargv[i][0] = '\0';
    }
    uargv[wargc] = NULL;
    *argc = wargc;
    *argv = uargv;                              // intentionally leaked; freed at exit
    LocalFree(wargv);
}

#else  // POSIX: argv is already UTF-8 (or whatever the locale is) and fopen takes it verbatim.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

FILE *portable_fopen(const char *path_utf8, const char *mode) {
    return fopen(path_utf8, mode);
}

void os_init_utf8_args(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
}

int portable_isatty_stdout(void) {
    return isatty(fileno(stdout));
}

int portable_rename(const char *from_utf8, const char *to_utf8) {
    return rename(from_utf8, to_utf8);   // atomic replace on POSIX
}

int portable_remove(const char *path_utf8) {
    return remove(path_utf8);
}

FILE *os_temp_file(char *path_out, size_t cap) {
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    int n = snprintf(path_out, cap, "%s/ffpdf.XXXXXX", dir);
    if (n < 0 || (size_t)n >= cap) return NULL;
    int fd = mkstemp(path_out);
    if (fd < 0) return NULL;
    return fdopen(fd, "wb");
}

#endif
