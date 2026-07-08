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
#ifndef OS_COMPAT_H
#define OS_COMPAT_H

#include <stdio.h>

// Open a file whose path is UTF-8. On POSIX this is plain fopen(); on Windows it
// converts to UTF-16 and uses _wfopen, so paths with non-ASCII characters
// (accented user names, OneDrive folders, CJK, etc.) open correctly instead of
// failing under the ANSI code page.
FILE *portable_fopen(const char *path_utf8, const char *mode);

// Ensure argv holds UTF-8 strings. On POSIX this is a no-op; on Windows it
// rebuilds argc/argv from the wide command line so file-path arguments survive
// as UTF-8 (plain argv is code-page encoded and mangles non-ASCII paths).
// Call once at the top of main(), before touching argv.
void os_init_utf8_args(int *argc, char ***argv);

// Non-zero when stdout is an interactive terminal (isatty / _isatty).
int portable_isatty_stdout(void);

// rename() that replaces an existing destination on every platform (POSIX
// rename is already atomic-replace; Windows needs MoveFileEx). UTF-8 paths.
// Returns 0 on success.
int portable_rename(const char *from_utf8, const char *to_utf8);

// remove() with UTF-8 paths (plain remove on POSIX, _wremove on Windows).
int portable_remove(const char *path_utf8);

// Create a new temporary file open for writing and store its path (UTF-8) in
// path_out. Returns NULL on failure. The caller removes it when done.
FILE *os_temp_file(char *path_out, size_t cap);

#endif // OS_COMPAT_H
