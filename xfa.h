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

#ifndef XFA_H
#define XFA_H

// Set the value of the field identified by its fully-qualified SOM name
// (e.g. "form1[0].Table[0].Row[1].amount[0]") in an XFA datasets XML document.
//
// The datasets tree is navigated by path + occurrence index: each SOM component
// selects the idx-th direct child element of that name. Container subforms that
// are absent from the data (transparent subforms, e.g. page subforms) are
// skipped, so two fields that share a leaf name in different subforms are
// disambiguated by their path and occurrence -- unlike a plain leaf-name match.
//
// Returns a new malloc'd XML string (caller frees); the input is unchanged. If
// the field's element cannot be located, returns an unchanged copy.
char *xfa_datasets_set(const char *xml, const char *field_name, const char *value);

#endif // XFA_H
