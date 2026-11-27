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

#ifndef FONT_METRICS_H
#define FONT_METRICS_H

// Advance width, in 1/1000 em, of WinAnsiEncoding code `code` in the standard-14
// font `base_font` ("Helvetica", "Times-Bold", "Courier", ...). Oblique and
// italic Helvetica/Courier share the upright metrics; unknown names fall back to
// Helvetica. Control codes (< 0x20) have width 0.
int std14_width(const char *base_font, unsigned char code);

#endif
