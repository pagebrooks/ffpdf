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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <io.h>       // _setmode
#include <fcntl.h>    // _O_BINARY
#endif

#include "utils.h"
#include "pdf_parser.h"
#include "form_extractor.h"
#include "pdf_filler.h"
#include "os_compat.h"

extern long find_startxref(FILE *f);

#define PROG_NAME    "ffpdf"
#define PROG_VERSION "0.1.0"

// Program name as invoked (without any directory), for diagnostics.
static const char *prog_basename(const char *p) {
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

// One-line usage, for argument errors. Goes to stderr; pairs with exit 1.
static void print_usage(FILE *out, const char *prog) {
    fprintf(out, "Usage: %s <command> [arguments]\n", prog);
    fprintf(out, "Try '%s help' for the full command list.\n", prog);
}

// GNU-style version banner.
static void print_version(void) {
    printf("%s %s\n", PROG_NAME, PROG_VERSION);
    printf("Fast PDF form-field extractor and filler.\n");
    printf("Copyright (C) 2026 Page Brooks.\n");
    printf("License Apache-2.0: Apache License, Version 2.0 "
           "<https://www.apache.org/licenses/LICENSE-2.0>.\n");
    printf("This is free software: you are free to change and redistribute it.\n");
    printf("There is NO WARRANTY, to the extent permitted by law.\n");
}

// Full help. Goes to stdout; pairs with exit 0.
static void print_help(void) {
    fputs(
PROG_NAME " " PROG_VERSION " \xe2\x80\x94 read and fill PDF form fields.\n"
"\n"
"USAGE\n"
"  " PROG_NAME " <command> [arguments]     (all commands write to stdout)\n"
"\n"
"COMMANDS\n"
"  fdf-extract  <pdf>              Extract form fields as an FDF\n"
"  xfdf-extract <pdf>              Extract form fields as an XFDF\n"
"  fields       <pdf>              List form fields as JSON: names, types,\n"
"                                  current values, choice options, checkbox\n"
"                                  on-states. Made for scripts and AI agents\n"
"  fill [options] <fdf> <pdf>      Fill <pdf> with values from <fdf>\n"
"                                  -f, --flatten bake the values in and remove\n"
"                                                the form -> a non-editable PDF\n"
"                                  -o, --output FILE   write to FILE instead of\n"
"                                                stdout (atomically: FILE.tmp is\n"
"                                                renamed over FILE on success)\n"
"                                  <fdf> may be '-' to read the FDF from stdin\n"
"  xref         <pdf>              Dump the parsed cross-reference table (debug)\n"
"  help                        Show this help and exit  (also -h, --help)\n"
"  version                     Show version information  (also -v, --version)\n"
"\n"
"WORKFLOW\n"
"  1. Discover field names:  " PROG_NAME " fdf-extract form.pdf\n"
"  2. Edit the FDF, setting each field's /V (value) beside its /T (name).\n"
"  3. Fill it:               " PROG_NAME " fill answers.fdf form.pdf > filled.pdf\n"
"\n"
"  Mail merge / bulk fills:  an FDF is plain text, so template it and loop --\n"
"  substitute per-record values (e.g. ${NAME} + envsubst) and run fill once\n"
"  per record. No startup cost: thousands of filled PDFs per minute per core.\n"
"\n"
"NOTES\n"
"  * Every command writes to stdout; redirect fill to a file (> out.pdf) or\n"
"    use -o. As a safety net, fill refuses to write a PDF to a terminal.\n"
"    Progress and warnings go to stderr, so they never corrupt the output.\n"
"  * '--' ends option parsing (for file names that begin with '-').\n"
"  * fill takes the FDF first, then the PDF:  fill <fdf> <pdf>.\n"
"  * Fills are incremental updates: the original bytes are preserved and the\n"
"    changes appended. --flatten instead bakes the values in and removes the form.\n"
"  * Encrypted PDFs are supported for the empty user password (RC4/AES).\n"
"  * Dynamic-XFA forms (e.g. some USCIS forms) keep fields outside the\n"
"    AcroForm tree; extraction finds nothing for those.\n"
"\n"
"ENVIRONMENT\n"
"  FFPDF_VERBOSE              Set to 1 to log filled values (off by default;\n"
"                            values may be sensitive, so they are redacted).\n"
"  PDF_MAX_DECOMPRESSED       Cap on one decompressed stream   (default 256M).\n"
"  PDF_MAX_TOTAL_DECOMPRESSED Cap on total decompression/run   (default 1G).\n"
"\n"
"EXIT STATUS\n"
"  0  success\n"
"  1  error: bad usage, or an unreadable / unparseable PDF\n"
"\n"
"See ffpdf(1) or README.md for details.\n",
    stdout);
}

static int cmd_xref(const char *path) {
    FILE *f = portable_fopen(path, "rb");
    if (!f) { perror("fopen"); return 1; }

    long xref_offset = find_startxref(f);
    if (xref_offset < 0) {
        fprintf(stderr, "Failed to find startxref\n");
        fclose(f);
        return 1;
    }

    XRefTable xref_table;
    if (parse_xref_table(f, xref_offset, &xref_table) <= 0) {
        fprintf(stderr, "Failed to parse xref table\n");
        xref_free(&xref_table);
        fclose(f);
        return 1;
    }

    print_xref_table(&xref_table, f);
    xref_free(&xref_table);
    fclose(f);
    return 0;
}

// mode: 0 = FDF, 1 = XFDF, 2 = JSON field listing.
static int cmd_extract(int mode, const char *path) {
    FILE *f = portable_fopen(path, "rb");
    if (!f) { perror("fopen"); return 1; }

    long xref_offset = find_startxref(f);
    if (xref_offset < 0) {
        fprintf(stderr, "Failed to find startxref\n");
        fclose(f);
        return 1;
    }

    XRefTable xref_table;
    if (parse_xref_table(f, xref_offset, &xref_table) <= 0) {
        fprintf(stderr, "Failed to parse xref table\n");
        xref_free(&xref_table);
        fclose(f);
        return 1;
    }

    if (mode == 2)
        extract_form_fields_json(f, &xref_table);
    else if (mode == 1)
        extract_form_fields_xfdf(f, &xref_table);
    else
        extract_form_fields_fdf(f, &xref_table);

    xref_free(&xref_table);
    fclose(f);
    return 0;
}

// Run `fill`: resolve the stdin-FDF and -o conventions, then delegate to
// fill_pdf_with_fdf_ex. With -o the PDF is written to <FILE>.tmp and renamed
// into place only on success, so a failed fill never leaves a partial output.
static int cmd_fill(const char *prog, const char *fdf_arg, const char *pdf_arg,
                    const char *outpath, int flatten) {
    int explicit_stdout = 0;
    if (outpath && strcmp(outpath, "-") == 0) {   // -o - : stdout, explicitly
        outpath = NULL;
        explicit_stdout = 1;
    }

    if (!outpath && !explicit_stdout && portable_isatty_stdout()) {
        fprintf(stderr,
                "%s: refusing to write a PDF to a terminal.\n"
                "Redirect it (%s fill ... > out.pdf) or use -o out.pdf.\n",
                prog, prog);
        return 1;
    }

    // FDF from stdin ("-"): spool to a temp file so the parser has a path.
    char fdf_tmp[4096];
    int fdf_is_tmp = 0;
    if (strcmp(fdf_arg, "-") == 0) {
        FILE *tf = os_temp_file(fdf_tmp, sizeof fdf_tmp);
        if (!tf) {
            fprintf(stderr, "%s: cannot create a temp file for the stdin FDF\n", prog);
            return 1;
        }
        char buf[65536];
        size_t r;
        while ((r = fread(buf, 1, sizeof buf, stdin)) > 0) {
            if (fwrite(buf, 1, r, tf) != r) {
                fprintf(stderr, "%s: cannot spool stdin FDF: write failed\n", prog);
                fclose(tf);
                portable_remove(fdf_tmp);
                return 1;
            }
        }
        fclose(tf);
        fdf_arg = fdf_tmp;
        fdf_is_tmp = 1;
    }

    FILE *out = stdout;
    char out_tmp[4096];
    if (outpath) {
        int n = snprintf(out_tmp, sizeof out_tmp, "%s.tmp", outpath);
        if (n < 0 || (size_t)n >= sizeof out_tmp) {
            fprintf(stderr, "%s: output path too long\n", prog);
            if (fdf_is_tmp) portable_remove(fdf_tmp);
            return 1;
        }
        out = portable_fopen(out_tmp, "wb");
        if (!out) {
            fprintf(stderr, "%s: cannot open %s for writing\n", prog, out_tmp);
            if (fdf_is_tmp) portable_remove(fdf_tmp);
            return 1;
        }
    }

    int rc = fill_pdf_with_fdf_ex(pdf_arg, fdf_arg, out, flatten);

    if (outpath) {
        if (fclose(out) != 0 && rc == 0) {
            fprintf(stderr, "%s: write to %s failed\n", prog, out_tmp);
            rc = 1;
        }
        if (rc == 0) {
            if (portable_rename(out_tmp, outpath) != 0) {
                fprintf(stderr, "%s: cannot rename %s to %s\n", prog, out_tmp, outpath);
                rc = 1;
            }
        }
        if (rc != 0) portable_remove(out_tmp);   // never leave a partial output
    }
    if (fdf_is_tmp) portable_remove(fdf_tmp);
    return rc;
}

int main(int argc, char *argv[]) {
#ifdef _WIN32
    // On Windows stdout defaults to text mode, which rewrites '\n' as "\r\n" and
    // would corrupt the binary PDF that fill/flatten write. Force raw bytes both
    // ways (stdin can carry a piped FDF for `fill -`).
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    // Rebuild argv as UTF-8 on Windows so non-ASCII file-path arguments survive.
    os_init_utf8_args(&argc, &argv);

    const char *prog = prog_basename(argv[0]);

    if (argc < 2) {
        print_usage(stderr, prog);
        return 1;
    }
    const char *cmd = argv[1];

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        print_help();
        return 0;
    }
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "-v") == 0 || strcmp(cmd, "--version") == 0) {
        print_version();
        return 0;
    }

    // fill [--flatten] [-o FILE] [--] <fdf> <pdf>  -> PDF on stdout (or FILE)
    if (strcmp(cmd, "fill") == 0) {
        int flatten = 0, a = 2;
        const char *outpath = NULL;
        for (; a < argc; a++) {
            if (strcmp(argv[a], "--") == 0) { a++; break; }   // end of options
            if (strcmp(argv[a], "--flatten") == 0 || strcmp(argv[a], "-f") == 0) {
                flatten = 1;
                continue;
            }
            if (strcmp(argv[a], "-o") == 0 || strcmp(argv[a], "--output") == 0) {
                if (a + 1 >= argc) {
                    fprintf(stderr, "%s: %s requires a file name\n", prog, argv[a]);
                    print_usage(stderr, prog);
                    return 1;
                }
                outpath = argv[++a];
                continue;
            }
            if (argv[a][0] == '-' && argv[a][1] != '\0') {   // "-" alone is stdin
                fprintf(stderr, "%s: unknown option '%s'\n", prog, argv[a]);
                print_usage(stderr, prog);
                return 1;
            }
            break;                                           // first positional
        }
        if (argc - a != 2) { print_usage(stderr, prog); return 1; }
        return cmd_fill(prog, argv[a], argv[a + 1], outpath, flatten);
    }

    // single-PDF commands
    if (strcmp(cmd, "fdf-extract") == 0 || strcmp(cmd, "xfdf-extract") == 0 ||
        strcmp(cmd, "fields") == 0 || strcmp(cmd, "xref") == 0) {
        int a = 2;
        if (a < argc && strcmp(argv[a], "--") == 0) a++;     // end of options
        if (argc - a != 1) { print_usage(stderr, prog); return 1; }
        if (strcmp(cmd, "xref") == 0) return cmd_xref(argv[a]);
        if (strcmp(cmd, "fields") == 0) return cmd_extract(2, argv[a]);
        return cmd_extract(strcmp(cmd, "xfdf-extract") == 0, argv[a]);
    }

    fprintf(stderr, "%s: unknown command '%s'\n", prog, cmd);
    print_usage(stderr, prog);
    return 1;
}
