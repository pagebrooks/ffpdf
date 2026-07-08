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

#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <zlib.h>

#define MAX_LINE 1024
#define MAX_DICT 4096

// Absolute sanity ceiling on object count (guards against a corrupt file with an
// absurd /Size); real documents are far below this.
#define XREF_MAX_OBJECTS 20000000

// Cross-reference table: a dynamically grown, insertion-ordered list of entries
// plus a direct obj_num -> position index for O(1) dedup and lookup. `offset` is
// a file byte offset, or -1 for an object stored in a compressed object stream.
typedef struct {
    int obj_num;
    long offset;      // >=0: byte offset (uncompressed); -1: compressed (in an object stream)
    int stream_obj;   // compressed: containing /ObjStm object number; else 0
    int stream_idx;   // compressed: index within that object stream
} XRefEntry;

typedef struct {
    XRefEntry *entries;   // insertion order (for iteration); grows as needed
    int count;
    int cap;
    int *index;           // obj_num -> entries index, or -1 if absent
    int index_cap;
} XRefTable;

#define XREF_ABSENT LONG_MIN   // xref_lookup() result for an unknown object

void xref_init(XRefTable *t);
void xref_free(XRefTable *t);
int  xref_add(XRefTable *t, int obj_num, long offset);  // uncompressed; O(1) dedup; 1 if added
int  xref_add_compressed(XRefTable *t, int obj_num, int stream_obj, int stream_idx);
long xref_lookup(const XRefTable *t, int obj_num);      // offset, or XREF_ABSENT
const XRefEntry *xref_get(const XRefTable *t, int obj_num);  // entry, or NULL

// PDF object structure
typedef struct {
    int obj_num;
    int gen_num;
    long file_offset;
    char dictionary[MAX_DICT];
    char *stream;
    size_t stream_len;
} PdfObject;

// Utility functions
void skip_whitespace(FILE *f);
int read_token(FILE *f, char *token, int max_len);
int parse_object_ref(const char *ref);
char* find_dict_value(const char *dictionary, const char *key, char *value, size_t value_size);
int is_pdf_command(const char *str);
char* decompress_flate(const char *data, size_t data_len, size_t *out_len);
char* decompress_lzw(const char *data, size_t data_len, size_t *out_len);
// Max decompressed size per stream (bomb guard); PDF_MAX_DECOMPRESSED overrides.
size_t decompress_limit(void);
// Reset the per-run cumulative decompression budget (PDF_MAX_TOTAL_DECOMPRESSED).
void decompress_reset(void);
// Dispatch on the stream dict's filter (FlateDecode or LZWDecode).
char* decompress_stream(const char *dict, const char *data, size_t data_len, size_t *out_len);
void print_json_escaped_string(const char *str, size_t len);

#endif // UTILS_H
