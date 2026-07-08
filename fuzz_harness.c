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

/* Fuzz harness for the PDF read AND fill paths.
 *
 * Reads a candidate PDF from argv[1] (or stdin) and exercises:
 *   - the read path: xref parsing, object/object-stream decode, Flate/LZW
 *     decompression, predictors, encryption setup, the field walk, extraction;
 *   - the fill path: builds an FDF from the PDF's own field names and runs
 *     fill_pdf_with_fdf, so appearance generation, checkbox/choice/list-box
 *     handling and XFA sync (all in pdf_filler.c / xfa.c) are fuzzed too.
 * Seed the corpus with encrypted PDFs (see fuzz.sh) to cover crypto.c. Built
 * with -fsanitize=address,undefined so any OOB/UAF/leak/UB aborts the process;
 * a driver mutates inputs and watches for non-zero exits / hangs.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>       // getpid, unlink
#include "utils.h"
#include "pdf_parser.h"
#include "field_map.h"
#include "form_extractor.h"
#include "pdf_filler.h"

// Build an FDF from up to 16 of the PDF's real field names, then run the fill
// path against the same file -- exercising pdf_filler.c / xfa.c with a mutated
// PDF whose field dictionaries are attacker-shaped.
static void fuzz_fill(const char *pdf_path, const FieldMap *map) {
    char fdfpath[64];
    snprintf(fdfpath, sizeof fdfpath, "/tmp/fuzzfdf_%d.fdf", (int)getpid());
    FILE *fd = fopen(fdfpath, "wb");
    if (!fd) return;
    fputs("%FDF-1.2\n1 0 obj\n<< /FDF << /Fields [\n", fd);
    int emitted = 0;
    for (int i = 0; i < map->count && emitted < 16; i++) {
        if (!map->items[i].terminal) continue;
        fprintf(fd, "<< /T (%s) /V (1) >>\n", map->items[i].qname);
        emitted++;
    }
    fputs("] >> >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%EOF\n", fd);
    fclose(fd);
    if (emitted) {
        FILE *devnull = fopen("/dev/null", "wb");
        if (devnull) {
            fill_pdf_with_fdf(pdf_path, fdfpath, devnull);
            fill_pdf_with_fdf_ex(pdf_path, fdfpath, devnull, 1);   // --flatten path
            fclose(devnull);
        }
    }
    unlink(fdfpath);
}

static void run_one(unsigned char *data, size_t size, const char *path) {
    FILE *f = fmemopen(data, size, "rb");
    if (!f) return;

    long sx = find_startxref(f);
    XRefTable xref;
    xref_init(&xref);
    if (sx >= 0 && parse_xref_table(f, sx, &xref) > 0) {
        FieldMap map = {0};
        build_field_map(f, &xref, 0, &map);           // parse + decrypt + walk
        extract_form_fields_fdf(f, &xref);            // name/value formatting paths
        print_xref_table(&xref, f);                   // the -xref debug path
        if (path) fuzz_fill(path, &map);              // fill path (re-opens `path`)
        field_map_free(&map);
    }
    objstm_cache_reset();
    xref_free(&xref);
    fclose(f);
}

int main(int argc, char **argv) {
    // Keep the parser from spending the fuzz budget on decompression bombs.
    setenv("PDF_MAX_DECOMPRESSED", "8388608", 0);
    setenv("PDF_MAX_TOTAL_DECOMPRESSED", "33554432", 0);
    if (!freopen("/dev/null", "w", stdout))           // silence extraction output
        return 0;                                     // exit 0: not a parser crash

    FILE *in = (argc > 1) ? fopen(argv[1], "rb") : stdin;
    if (!in) return 0;
    size_t cap = 1 << 16, n = 0;
    unsigned char *buf = malloc(cap);
    for (;;) {
        if (n == cap) { cap *= 2; buf = realloc(buf, cap); }
        size_t r = fread(buf + n, 1, cap - n, in);
        n += r;
        if (r == 0) break;
    }
    if (in != stdin) fclose(in);

    run_one(buf, n, (argc > 1) ? argv[1] : NULL);
    free(buf);
    return 0;
}
