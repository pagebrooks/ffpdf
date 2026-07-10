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

#ifndef FORM_EXTRACTOR_H
#define FORM_EXTRACTOR_H

#include "utils.h"

// Extract the form's fillable fields to stdout, as FDF or XFDF. Both are thin
// presentation layers over build_field_map() (see field_map.h).
void extract_form_fields_fdf(FILE *f, const XRefTable *xref_table);
void extract_form_fields_xfdf(FILE *f, const XRefTable *xref_table);

// The `fields` command: list every fillable field as JSON (name, type, current
// value, and per-type extras: a choice field's options and combo/multi-select
// flags, a checkbox's on-state). Designed for programmatic callers.
void extract_form_fields_json(FILE *f, const XRefTable *xref_table);

#endif // FORM_EXTRACTOR_H
