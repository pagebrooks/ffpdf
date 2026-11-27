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

#ifndef PDF_LEX_H
#define PDF_LEX_H

#include <stddef.h>

/* ==========================================================================
 * PDF syntax helpers: locating keys, reading strings/refs, and walking values
 * inside in-memory dictionary text. Pure string operations (no FILE/xref), so
 * every module that inspects raw object text shares one implementation.
 *
 * Two key finders, intentionally different:
 *   find_key   -- top-level, structure-aware: skips string/array/nested-dict
 *                 contents so a same-named key inside a value (or a nested
 *                 subdictionary) is not matched. Use for object dictionaries.
 *   dict_find  -- shallow: the first "/key" anywhere via strstr. Use for simple
 *                 dictionaries or when the wanted key lives in a subdictionary
 *                 (e.g. /CFM in /CF, or /Predictor in /DecodeParms).
 * ========================================================================== */

// Pointer just past the matching ">>" of a dict starting at `start` ("<<..."),
// string-aware; NULL if unbalanced.
const char *match_dict_end(const char *start);

// Pointer to the value following a top-level `key` (whitespace skipped), or NULL.
const char *find_key(const char *dict, const char *key);

// Pointer just past a PDF value at `v` (string/dict/hex/array/name/number, and
// a trailing "N G R" indirect reference).
const char *skip_value(const char *v);

// Remove every top-level "/key <value>" from a mutable dict buffer, in place.
void remove_entry(char *dict, const char *key);

// Copy the inner text between the outer << and >> of an object dict into `out`.
// Returns 0 on success, -1 if malformed/truncated or too large for `out`.
int extract_dict_inner(const char *obj_dict, char *out, size_t out_size);

// Like extract_dict_inner but returns a malloc'd, exactly-sized copy (no fixed
// limit). Caller frees. NULL if malformed or out of memory.
char *extract_dict_inner_alloc(const char *obj_dict);

// Shallow key lookup (first "/key" via strstr): pointer to the value (whitespace
// skipped), or NULL.
const char *dict_find(const char *dict, const char *key);

// Integer value of a shallow `key`, or `dflt` if absent/non-numeric.
int dict_int(const char *dict, const char *key, int dflt);

// Read a PDF literal string starting at '(' into `out` (unescaped). Returns a
// pointer past the closing ')'.
const char *read_literal(const char *p, char *out, size_t out_size);

// Like read_literal but allocates an exactly-sized, unescaped copy into *out
// (caller frees; never truncates). Returns a pointer past the closing ')'.
const char *read_literal_alloc(const char *p, char **out);

// Decode the literal string starting at '(' into raw bytes per ISO 32000-1
// 7.3.4.2: balanced parentheses; the escapes \n \r \t \b \f \( \) \\; octal
// \d, \dd and \ddd; a backslash before an end-of-line continues the line (no
// byte); an unescaped CR, LF or CRLF reads as one LF; any other escaped
// character stands for itself. At most `cap` bytes are stored (the rest of the
// string is still consumed) and *len is set to the count. Binary-safe for
// escaped bytes, so use this wherever exact bytes matter (encryption keys,
// ciphertext). Returns a pointer past the closing ')'.
const char *pdf_literal_bytes(const char *p, unsigned char *out, size_t cap, size_t *len);

/* ==========================================================================
 * PDF text strings (ISO 32000-1 7.9.2.2) -> UTF-8.
 *
 * Field names (/T), labels (/TU), text values (/V) and choice options (/Opt)
 * are text strings: UTF-16BE when the bytes start with the FE FF byte order
 * mark, UTF-8 when they start with EF BB BF (PDF 2.0), otherwise
 * PDFDocEncoding. Everything ffpdf reports or matches is UTF-8, so these
 * strings are decoded once, here.
 * ========================================================================== */

// Decode one UTF-8 code point from *s (NUL-terminated) and advance *s past it.
// On a malformed lead/continuation byte, consume a single byte and return it
// unchanged (U+0000..U+00FF), so no input is lost on non-UTF-8 or truncated data.
unsigned utf8_next(const unsigned char **s);

// Decode text-string bytes `b` (length `n`) to NUL-terminated UTF-8 in `out`.
// Unpaired surrogates and bytes PDFDocEncoding leaves undefined become U+FFFD;
// U+0000 is dropped. Truncates on a character boundary to fit `cap` (>= 1).
// Returns the number of bytes written, excluding the NUL. 3*n+1 bytes always
// suffice.
size_t pdf_text_bytes_utf8(const unsigned char *b, size_t n, char *out, size_t cap);

// Decode the text string at `v` (a literal "(...)" or hex "<...>" string) to
// UTF-8 in `out` (see pdf_text_bytes_utf8). Returns a pointer past the string,
// or NULL (with `out` set to "") if `v` is not a string.
const char *pdf_text(const char *v, char *out, size_t cap);

// Like pdf_text but returns an exactly-sized malloc'd UTF-8 copy (caller frees),
// or NULL if `v` is not a string or memory runs out.
char *pdf_text_alloc(const char *v);

// Variants for hand-written input files (FDF): a string without a byte order
// mark whose bytes are valid UTF-8 and include a non-ASCII character is taken
// as UTF-8 instead of PDFDocEncoding, since that is how people type them.
const char *pdf_text_input(const char *v, char *out, size_t cap);
char *pdf_text_input_alloc(const char *v);

// Object number of an indirect reference "N G R" at `p` (leading ws skipped).
int parse_ref_num(const char *p);

// Parse an indirect reference "N G R" at `p` into *num and *gen.
void parse_indirect_ref(const char *p, int *num, int *gen);

#endif // PDF_LEX_H
