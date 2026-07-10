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

#ifndef FIELD_MAP_H
#define FIELD_MAP_H

#include <stdio.h>
#include "utils.h"    // XRefTable, MAX_DICT
#include "crypto.h"   // PdfCrypt

// A named node in the AcroForm field tree. The same walk backs both form-field
// extraction (fdf-extract/xfdf-extract) and the fill command, so the two can never disagree
// about which fields exist or what they are named.
typedef struct {
    char qname[256];   // fully-qualified partial name, e.g. "form1[0].f1_1[0]"
    int  obj_num;
    int  gen_num;
    char ftype;        // 'T' text, 'B' button, 'C' choice, 'S' signature, 0 if unknown
    int  terminal;     // 1 if a leaf field (no named descendant) -> fillable
} FieldLoc;

typedef struct {
    FieldLoc *items;
    int count;
    int cap;
} FieldMap;

// Walk the AcroForm field tree, recording every named node. EXTENDS support must
// already be initialised by the caller (init_form_extractor_extends_support).
// `root_obj` is the /Root (catalog) object number, or <=0 to have it resolved
// from the trailer. Returns the AcroForm object number (>0) on success, or 0 if
// no AcroForm/Fields were found. Populates `map` (free with field_map_free).
int build_field_map(FILE *f, XRefTable *xref, int root_obj, FieldMap *map);
void field_map_free(FieldMap *map);

// Free the lazy object-stream cache. Call once after a fill/extract run (object
// streams are decompressed on demand and cached across get_object_raw calls).
void objstm_cache_reset(void);

// After build_field_map(): if the document is encrypted, the /Encrypt object
// number (>0) and, via pdf_doc_id0(), the first /ID element. Both are needed to
// carry the encryption forward into an incremental-update trailer. Return 0 /
// length 0 when the document is not encrypted.
int pdf_doc_encrypt_obj(void);
int pdf_doc_id0(unsigned char *out, int cap);

// The active document crypt handler after build_field_map(), or NULL if the
// document is not encrypted. Set up while reading; used to decrypt/encrypt.
const PdfCrypt *pdf_doc_crypt(void);

// Locate the AcroForm object number via the document catalog (Root).
int find_acroform_obj(FILE *f, XRefTable *xref, int root_obj);

// Resolve the /Root (catalog) object number from the trailer. 0 if not found.
int find_root_obj(FILE *f);

// ---- Shared raw-object / dictionary helpers (reused by the fill writer) ----

// Raw dictionary text ("<<...>>") of an object, verbatim. Reads uncompressed
// objects directly and compressed ones via EXTENDS. malloc'd; caller frees.
char *get_object_raw(FILE *f, XRefTable *xref, int obj_num, int *gen_out);

// The "[...]" array text for `key` in `dict`, resolving an inline array or an
// indirect reference. malloc'd; caller frees. NULL if absent.
char *field_array_text(FILE *f, XRefTable *xref, const char *dict, const char *key);

// (find_key / remove_entry / extract_dict_inner and friends now live in pdf_lex.h.)

#endif // FIELD_MAP_H
