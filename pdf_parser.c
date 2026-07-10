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

#include "pdf_parser.h"
#include "pdf_lex.h"          // dict_int, find_key, ...
#include "crypto.h"

// Helper function to extract form field name from /T(...) pattern
// Returns the length of the extracted name, or 0 if extraction failed
int extract_form_field_name(const char *data, size_t data_len, size_t start_pos, char *name_buffer, size_t buffer_size) {
    if (start_pos + 3 >= data_len) return 0;
    
    size_t pos = start_pos + 3; // Skip "/T("
    size_t name_pos = 0;
    int paren_count = 1;
    
    while (pos < data_len && paren_count > 0 && name_pos < buffer_size - 1) {
        if (data[pos] == '(') {
            paren_count++;
            name_buffer[name_pos++] = data[pos];
        } else if (data[pos] == ')') {
            paren_count--;
            if (paren_count > 0) {
                name_buffer[name_pos++] = data[pos];
            }
        } else {
            name_buffer[name_pos++] = data[pos];
        }
        pos++;
    }
    
    name_buffer[name_pos] = '\0';
    return name_pos;
}

// Helper function to extract form field value from /V(...) pattern after a given position
// Returns the length of the extracted value, or 0 if extraction failed
int extract_form_field_value(const char *data, size_t data_len, size_t search_start, char *value_buffer, size_t buffer_size) {
    // Search for /V( pattern after the current position, but within a reasonable range
    size_t max_search = search_start + 1000; // Search up to 1000 chars ahead
    if (max_search > data_len) max_search = data_len;
    
    for (size_t i = search_start; i <= max_search - 3; i++) {
        if (data[i] == '/' && data[i+1] == 'V' && data[i+2] == '(') {
            size_t pos = i + 3; // Skip "/V("
            size_t value_pos = 0;
            int paren_count = 1;
            
            while (pos < data_len && paren_count > 0 && value_pos < buffer_size - 1) {
                if (data[pos] == '(') {
                    paren_count++;
                    value_buffer[value_pos++] = data[pos];
                } else if (data[pos] == ')') {
                    paren_count--;
                    if (paren_count > 0) {
                        value_buffer[value_pos++] = data[pos];
                    }
                } else if (data[pos] == '\\' && pos + 1 < data_len) {
                    // Handle escaped characters
                    value_buffer[value_pos++] = data[pos];
                    pos++;
                    if (value_pos < buffer_size - 1) {
                        value_buffer[value_pos++] = data[pos];
                    }
                } else {
                    value_buffer[value_pos++] = data[pos];
                }
                pos++;
            }
            
            value_buffer[value_pos] = '\0';
            return value_pos;
        }
        // Stop searching if we hit another field definition (but allow for some distance)
        if (i > search_start + 10 && data[i] == '/' && i + 1 < data_len && data[i+1] == 'T' && i + 2 < data_len && data[i+2] == '(') {
            break;
        }
    }
    
    value_buffer[0] = '\0';
    return 0;
}

// Find the startxref offset: the last "startxref" in the file's tail followed
// by a number. Returns that byte offset, or -1.
long find_startxref(FILE *f) {
    char buffer[2048];
    fseek(f, 0, SEEK_END);
    long filesize = ftell(f);
    if (filesize < 0) return -1;

    long search_start = filesize > 1024 ? filesize - 1024 : 0;   // scan the tail
    long search_len = filesize - search_start;
    if (search_len > (long)sizeof(buffer) - 1) search_len = sizeof(buffer) - 1;
    fseek(f, search_start, SEEK_SET);
    size_t bytes_read = fread(buffer, 1, (size_t)search_len, f);
    buffer[bytes_read] = '\0';                                   // so atol() never runs off the end

    // Search backwards. Parse the offset with a separate index `j`; mutating the
    // loop index would let a keyword with no numeric offset re-match forever.
    for (long i = (long)bytes_read - 9; i >= 0; i--) {
        if (strncmp(buffer + i, "startxref", 9) != 0) continue;
        long j = i + 9;
        while (j < (long)bytes_read &&
               (buffer[j] == ' ' || buffer[j] == '\t' || buffer[j] == '\n' || buffer[j] == '\r')) j++;
        if (j < (long)bytes_read && buffer[j] >= '0' && buffer[j] <= '9')
            return atol(buffer + j);
        // "startxref" with no numeric offset here -> keep scanning downward.
    }
    return -1;
}

// Value of a direct-integer /Length, or -1 if /Length is absent or an indirect
// reference ("/Length N G R"). The scan-for-endstream stream length includes
// the EOL delimiter before "endstream" (harmless for stream ciphers, but fatal
// for block ciphers, which need the exact byte count), so a direct /Length wins.
static long pdf_direct_length(const char *dict) {
    const char *p = strstr(dict, "/Length");
    if (!p) return -1;
    p += 7;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!isdigit((unsigned char)*p)) return -1;
    char *e;
    long v = strtol(p, &e, 10);
    const char *q = e;
    while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
    if (isdigit((unsigned char)*q)) {                 // maybe "N G R" indirect ref
        while (isdigit((unsigned char)*q)) q++;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q == 'R') return -1;
    }
    return v;
}

// Parse PDF object at given offset
PdfObject parse_obj_at_offset(FILE *f, long offset, const PdfCrypt *crypt) {
    PdfObject obj = {0};
    // A negative offset is the sentinel for an object that lives inside a
    // compressed object stream (a type-2 xref entry; see parse_xref_stream_data). It has
    // no byte offset in the file and is resolved lazily via the object-stream cache.
    if (offset < 0) return obj;
    fseek(f, offset, SEEK_SET);
    
    char token[MAX_LINE];
    if (!read_token(f, token, sizeof(token)) || !isdigit(token[0])) {
        return obj;
    }
    
    obj.obj_num = atoi(token);
    obj.file_offset = offset;
    
    // Read generation number
    if (!read_token(f, token, sizeof(token))) return obj;
    obj.gen_num = atoi(token);
    
    // Read "obj"
    if (!read_token(f, token, sizeof(token)) || strcmp(token, "obj") != 0) {
        return obj;
    }

    // --- Capture the dictionary VERBATIM ---------------------------------
    // The dictionary is copied byte-for-byte between the outer "<<" and its
    // matching ">>", preserving exact PDF syntax (e.g. "/AcroForm", not
    // "/ AcroForm"). Objects decompressed from object streams are already
    // stored raw, so both access paths now yield identical text. Literal and
    // hex strings are copied opaquely so that "(" ")" "<" ">" inside a string
    // do not affect dictionary nesting.
    long body_start = ftell(f);
    fseek(f, 0, SEEK_END);
    long file_end = ftell(f);
    long window = file_end - body_start;
    long window_cap = (long)MAX_DICT * 4;          // dict text lives near the head
    if (window > window_cap) window = window_cap;
    fseek(f, body_start, SEEK_SET);

    char *win = malloc((size_t)window + 1);
    if (!win) return obj;
    size_t got = fread(win, 1, (size_t)window, f);
    win[got] = '\0';

    long dict_end_off = body_start;                // where the dict text ends
    // Scan for the object's opening "<<", but stop at the object's "endobj"
    // (or "stream") boundary so a bodyless object (e.g. "8 0 obj 1234 endobj")
    // does not capture the *next* object's dictionary.
    size_t i = 0;
    while (i + 1 < got) {
        if (win[i] == '<' && win[i + 1] == '<') break;
        if (win[i] == 'e' && i + 6 <= got && strncmp(win + i, "endobj", 6) == 0) break;
        if (win[i] == 's' && i + 6 <= got && strncmp(win + i, "stream", 6) == 0) break;
        i++;
    }

    if (i + 1 < got && win[i] == '<' && win[i + 1] == '<') {
        size_t j = i, dp = 0;
        int depth = 0;
        while (j < got) {
            char ch = win[j];
            if (ch == '(') {                       // literal string (balanced parens)
                int sdepth = 1;
                if (dp < MAX_DICT - 1) obj.dictionary[dp++] = ch;
                j++;
                while (j < got && sdepth > 0) {
                    char sc = win[j];
                    if (sc == '\\' && j + 1 < got) {
                        if (dp < MAX_DICT - 2) { obj.dictionary[dp++] = sc; obj.dictionary[dp++] = win[j + 1]; }
                        j += 2;
                        continue;
                    }
                    if (sc == '(') sdepth++;
                    else if (sc == ')') sdepth--;
                    if (dp < MAX_DICT - 1) obj.dictionary[dp++] = sc;
                    j++;
                }
                continue;
            }
            if (ch == '<' && j + 1 < got && win[j + 1] == '<') {
                depth++;
                if (dp < MAX_DICT - 2) { obj.dictionary[dp++] = '<'; obj.dictionary[dp++] = '<'; }
                j += 2;
                continue;
            }
            if (ch == '>' && j + 1 < got && win[j + 1] == '>') {
                depth--;
                if (dp < MAX_DICT - 2) { obj.dictionary[dp++] = '>'; obj.dictionary[dp++] = '>'; }
                j += 2;
                if (depth == 0) break;
                continue;
            }
            if (dp < MAX_DICT - 1) obj.dictionary[dp++] = ch;
            j++;
        }
        obj.dictionary[dp < MAX_DICT ? dp : MAX_DICT - 1] = '\0';
        dict_end_off = body_start + (long)j;
    }
    free(win);

    // --- Capture an optional stream --------------------------------------
    // Match "stream" by hand (not read_token, which would swallow one byte of
    // the EOL and leave us guessing). Per PDF 7.3.8.1 the keyword is followed by
    // exactly CRLF or LF and then the data begins -- so consume that one EOL and
    // NOTHING else. A leading space/tab/newline in the data is data (this matters
    // for binary and encrypted streams, whose first byte is arbitrary).
    fseek(f, dict_end_off, SEEK_SET);
    int c;
    do { c = fgetc(f); } while (c == ' ' || c == '\t' || c == '\r' || c == '\n');
    int is_stream = (c == 's');
    for (int k = 1; k < 6 && is_stream; k++)
        if (fgetc(f) != "stream"[k]) is_stream = 0;
    if (is_stream) {
        c = fgetc(f);                          // the single EOL after "stream"
        if (c == '\r') { int n = fgetc(f); if (n != '\n' && n != EOF) ungetc(n, f); }
        else if (c == '\n') { /* consumed */ }
        else if (c != EOF) ungetc(c, f);       // tolerate a missing EOL

        long stream_start = ftell(f);
        long len = -1;

        // Preferred: a direct-integer /Length, trusted only when "endstream"
        // actually follows it (encrypted/binary data can contain the keyword).
        // This path is independent of the file's line-ending style.
        // /Length is attacker-declared: cap it at the bytes that actually
        // remain in the file before using it for seeks or allocation (also
        // avoids 32-bit long overflow of stream_start + L on LLP64 Windows).
        long L = pdf_direct_length(obj.dictionary);
        if (L >= 0 && L <= file_end - stream_start) {
            fseek(f, stream_start + L, SEEK_SET);
            char probe[16];
            size_t pg = fread(probe, 1, sizeof(probe) - 1, f);
            probe[pg] = '\0';
            const char *p = probe;
            while (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') p++;
            if (strncmp(p, "endstream", 9) == 0) len = L;
        }

        // Fallback (no usable /Length): scan byte-wise for the "endstream"
        // keyword, so CR-only, LF, and CRLF line endings all work (an fgets scan
        // finds nothing in a CR-delimited file), then trim the one EOL delimiter
        // that precedes the keyword.
        if (len < 0) {
            fseek(f, stream_start, SEEK_SET);
            const char *kw = "endstream";
            int m = 0, ch;
            long endkw = -1;
            while ((ch = fgetc(f)) != EOF) {
                if (ch == (unsigned char)kw[m]) {
                    if (++m == 9) { endkw = ftell(f); break; }
                } else {
                    m = (ch == (unsigned char)kw[0]) ? 1 : 0;
                }
            }
            if (endkw >= 0) {
                long dlen = (endkw - 9) - stream_start;
                if (dlen >= 2) {                       // trim CRLF / LF / CR delimiter
                    fseek(f, stream_start + dlen - 2, SEEK_SET);
                    int b1 = fgetc(f), b2 = fgetc(f);
                    if (b1 == '\r' && b2 == '\n') dlen -= 2;
                    else if (b2 == '\n' || b2 == '\r') dlen -= 1;
                } else if (dlen == 1) {
                    fseek(f, stream_start, SEEK_SET);
                    int b = fgetc(f);
                    if (b == '\n' || b == '\r') dlen -= 1;
                }
                len = dlen;
            }
        }

        if (len > file_end - stream_start) len = file_end - stream_start;
        if (len >= 0) {
            obj.stream_len = len;
            fseek(f, stream_start, SEEK_SET);
            obj.stream = malloc(obj.stream_len + 1);
            if (obj.stream) {
                size_t got = fread(obj.stream, 1, obj.stream_len, f);
                obj.stream_len = got;              // a short read (truncated file) shrinks the stream
                obj.stream[got] = '\0';
            }
        }
    }

    // Decrypt the stream in place when a crypt handler is supplied. Callers pass
    // NULL for unencrypted documents and when reading cross-reference streams
    // (which are never encrypted).
    if (crypt && obj.stream && obj.stream_len > 0 && !strstr(obj.dictionary, "/XRef")) {
        if (getenv("CRYPT_DEBUG"))
            fprintf(stderr, "[crypt] decrypt stream obj %d gen %d len %zu cfm %d\n",
                    obj.obj_num, obj.gen_num, obj.stream_len, crypt->cfm);
        int r = pdf_decrypt(crypt, obj.obj_num, obj.gen_num,
                            (unsigned char *)obj.stream, obj.stream_len,
                            (unsigned char *)obj.stream);
        if (r >= 0) { obj.stream_len = r; obj.stream[r] = '\0'; }
    }

    return obj;
}

// (xref_add / xref_lookup live in utils.c: O(1) dedup + lookup, no fixed cap.
//  The chain is walked newest-first, so an existing entry always wins.)

// Parse one cross-reference section (a cross-reference stream or a classic
// `xref` table) at `offset`, adding its entries to the table. Sets *prev to the
// section's /Prev offset (-1 if none). Returns 0 if a section was parsed.
static int parse_xref_section(FILE *f, long offset, XRefTable *xref_table, long *prev) {
    *prev = -1;
    if (offset < 0) return -1;
    fseek(f, offset, SEEK_SET);
    char token[MAX_LINE];
    if (!read_token(f, token, sizeof(token))) return -1;

    if (isdigit((unsigned char)token[0])) {            // cross-reference stream
        PdfObject xo = parse_obj_at_offset(f, offset, NULL);
        int ok = -1;
        // decompress_stream handles Flate, LZW, and raw (no /Filter) streams;
        // ffpdf's own fill appends its xref stream unfiltered.
        if (xo.stream) {
            size_t dl;
            char *dec = decompress_stream(xo.dictionary, xo.stream, xo.stream_len, &dl);
            if (dec && dl > 0) {
                parse_xref_stream_data(dec, dl, xo.dictionary, xref_table);
                free(dec);
                ok = 0;
            }
        }
        *prev = dict_int(xo.dictionary, "/Prev", -1);
        if (xo.stream) free(xo.stream);
        return ok;
    }

    if (strcmp(token, "xref") == 0) {                  // classic table
        char a[MAX_LINE], b[MAX_LINE], c[MAX_LINE];
        while (read_token(f, token, sizeof(token))) {
            if (strcmp(token, "trailer") == 0) break;
            int start_obj = atoi(token);
            if (!read_token(f, token, sizeof(token))) break;
            int count = atoi(token);
            for (int i = 0; i < count; i++) {
                if (!read_token(f, a, sizeof(a))) break;   // offset
                if (!read_token(f, b, sizeof(b))) break;   // generation
                if (!read_token(f, c, sizeof(c))) break;   // n / f
                if (strcmp(c, "n") == 0) xref_add(xref_table, start_obj + i, atol(a));
            }
        }
        // Read the trailer dictionary for /Prev and (hybrid files) /XRefStm.
        char tbuf[MAX_DICT];
        size_t tn = fread(tbuf, 1, sizeof(tbuf) - 1, f);
        tbuf[tn] = '\0';
        *prev = dict_int(tbuf, "/Prev", -1);

        // Hybrid-reference file: the classic table lists uncompressed objects
        // for legacy readers, and a parallel cross-reference stream at /XRefStm
        // lists the compressed ones. Fold in its entries (its own /Prev is
        // ignored -- this classic table's /Prev drives the chain).
        long xrefstm = dict_int(tbuf, "/XRefStm", -1);
        if (xrefstm >= 0) {
            long ignored;
            parse_xref_section(f, xrefstm, xref_table, &ignored);
        }
        return 0;
    }
    return -1;
}

// Build the cross-reference table by walking the /Prev chain from the newest
// section (at `xref_offset`) to the oldest. Only the cross-reference sections
// themselves are read -- no whole-file scan -- so cost is independent of file
// size. Handles both cross-reference streams and classic tables, and mixed
// chains. The newest definition of each object wins.
int parse_xref_table(FILE *f, long xref_offset, XRefTable *xref_table) {
    xref_init(xref_table);   // caller frees with xref_free()

    long off = xref_offset;
    long visited[64];
    int nvisited = 0;
    while (off >= 0) {
        int seen = 0;
        for (int i = 0; i < nvisited; i++) if (visited[i] == off) { seen = 1; break; }
        if (seen || nvisited >= (int)(sizeof(visited) / sizeof(visited[0]))) break;  // cycle / too deep
        visited[nvisited++] = off;

        long prev = -1;
        if (parse_xref_section(f, off, xref_table, &prev) != 0 && nvisited == 1)
            return 0;   // could not parse the first (newest) section
        off = prev;
    }

    // Compressed objects arrive as type-2 xref entries (offset -1); their
    // containing object streams are decompressed lazily on first access (the
    // object-stream cache in field_map.c), so no whole-file work is needed here.
    return xref_table->count;
}

// Helper function to escape JSON strings
void print_json_escaped_string(const char *str, size_t len) {
    if (!str || len == 0) {
        return;
    }
    
    for (size_t i = 0; i < len; i++) {
        char c = str[i];
        switch (c) {
            case '"': printf("\\\""); break;
            case '\\': printf("\\\\"); break;
            case '\b': printf("\\b"); break;
            case '\f': printf("\\f"); break;
            case '\n': printf("\\n"); break;
            case '\r': printf("\\r"); break;
            case '\t': printf("\\t"); break;
            default:
                if (c >= 32 && c <= 126) {
                    putchar(c);
                } else {
                    printf("\\u%04x", (unsigned char)c);
                }
                break;
        }
    }
}

// Print XRef table for debugging in JSON format
void print_xref_table(const XRefTable *xref_table, FILE *f) {
    printf("{\n");
    printf("  \"xref_table\": {\n");
    printf("    \"total_entries\": %d,\n", xref_table->count);
    printf("    \"objects\": [\n");
    
    // Get file size for calculating last object size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    
    for (int i = 0; i < xref_table->count; ++i) {
        int obj_num = xref_table->entries[i].obj_num;
        long offset = xref_table->entries[i].offset;
        
        // Calculate object size (from current offset to next object or end of file)
        long obj_size;
        if (i + 1 < xref_table->count) {
            obj_size = xref_table->entries[i + 1].offset - offset;
        } else {
            obj_size = file_size - offset;
        }
        
        printf("      {\n");
        printf("        \"object_number\": %d,\n", obj_num);
        printf("        \"offset\": %ld,\n", offset);
        printf("        \"size_bytes\": %ld,\n", obj_size);
        
        // Try to parse the object at this offset
        PdfObject obj = parse_obj_at_offset(f, offset, NULL);
        
        // Compression detection
        const char *compression = "none";
        if (obj.dictionary[0]) {
            if (strstr(obj.dictionary, "FlateDecode")) compression = "FlateDecode";
            else if (strstr(obj.dictionary, "LZWDecode")) compression = "LZWDecode";
            else if (strstr(obj.dictionary, "DCTDecode")) compression = "DCTDecode";
            else if (strstr(obj.dictionary, "JPXDecode")) compression = "JPXDecode";
            else if (strstr(obj.dictionary, "RunLengthDecode")) compression = "RunLengthDecode";
        }
        
        printf("        \"compression\": \"%s\",\n", compression);
        
        int has_stream = (obj.stream && obj.stream_len > 0);
        printf("        \"has_stream\": %s,\n", has_stream ? "true" : "false");
        
        if (has_stream) {
            printf("        \"stream_size_bytes\": %zu,\n", obj.stream_len);
        }
        
        // Dictionary content
        printf("        \"dictionary\": \"");
        if (obj.dictionary[0] != '\0') {
            print_json_escaped_string(obj.dictionary, strlen(obj.dictionary));
        }
        printf("\"");
        
        // Attempt decompression and analysis if compressed stream exists
        if (has_stream && (strcmp(compression, "FlateDecode") == 0 ||
                           strcmp(compression, "LZWDecode") == 0)) {
            size_t decompressed_len;
            char *decompressed = decompress_stream(obj.dictionary, obj.stream, obj.stream_len, &decompressed_len);
            if (decompressed && decompressed_len > 0) {
                printf(",\n        \"decompression\": {\n");
                printf("          \"success\": true,\n");
                printf("          \"decompressed_size_bytes\": %zu,\n", decompressed_len);
                
                // Analyze decompressed content for interesting patterns
                int form_fields = 0;
                int xfa_content = 0;
                int pdf_commands = 0;
                
                // Count form field patterns /T( and extract field details
                // Use binary-safe search that doesn't stop at null bytes
                typedef struct {
                    char name[256];
                    char value[512];
                } FormField;
                
                FormField *fields = NULL;
                int fields_capacity = 0;
                
                for (size_t i = 0; i + 2 < decompressed_len; i++) {
                    if (decompressed[i] == '/' && decompressed[i+1] == 'T' && decompressed[i+2] == '(') {
                        // Expand fields array if needed
                        if (form_fields >= fields_capacity) {
                            fields_capacity = fields_capacity == 0 ? 10 : fields_capacity * 2;
                            FormField *new_fields = realloc(fields, sizeof(FormField) * fields_capacity);
                            if (!new_fields) {
                                // Failed to allocate memory, skip this field
                                continue;
                            }
                            fields = new_fields;
                        }
                        
                        // Ensure we have valid memory before proceeding
                        if (!fields) {
                            continue;
                        }
                        
                        // Extract field name
                        extract_form_field_name(decompressed, decompressed_len, i, 
                                               fields[form_fields].name, sizeof(fields[form_fields].name));
                        
                        // Extract field value (search forward from current position)
                        extract_form_field_value(decompressed, decompressed_len, i, 
                                                fields[form_fields].value, sizeof(fields[form_fields].value));
                        
                        form_fields++;
                    }
                }
                
                // Check for XFA content - use binary-safe search
                int found_xfa = 0;
                // Search for "<xfa:"
                for (size_t i = 0; i + 4 < decompressed_len && !found_xfa; i++) {
                    if (memcmp(&decompressed[i], "<xfa:", 5) == 0) {
                        found_xfa = 1;
                    }
                }
                // Search for "xmlns:xfa"
                for (size_t i = 0; i + 8 < decompressed_len && !found_xfa; i++) {
                    if (memcmp(&decompressed[i], "xmlns:xfa", 9) == 0) {
                        found_xfa = 1;
                    }
                }
                // Search for "<?xml"
                for (size_t i = 0; i + 4 < decompressed_len && !found_xfa; i++) {
                    if (memcmp(&decompressed[i], "<?xml", 5) == 0) {
                        found_xfa = 1;
                    }
                }
                if (found_xfa) {
                    xfa_content = 1;
                }
                
                // Count PDF drawing commands
                char *pos = decompressed;
                const char *commands[] = {"BT", "ET", "Tf", "Td", "Tj", "re", "f", "S", "cm", NULL};
                for (int cmd_i = 0; commands[cmd_i]; cmd_i++) {
                    pos = decompressed;
                    while ((pos = strstr(pos, commands[cmd_i])) != NULL) {
                        // Check if it's a standalone command (surrounded by whitespace)
                        int cmd_len = strlen(commands[cmd_i]);
                        if ((pos == decompressed || isspace(pos[-1])) && 
                            (pos + cmd_len >= decompressed + decompressed_len || isspace(pos[cmd_len]))) {
                            pdf_commands++;
                        }
                        pos += cmd_len;
                    }
                }
                
                printf("          \"analysis\": {\n");
                printf("            \"form_fields_count\": %d,\n", form_fields);
                
                // Print form field details if any were found
                if (form_fields > 0) {
                    printf("            \"form_fields\": [\n");
                    for (int f = 0; f < form_fields; f++) {
                        printf("              {\n");
                        printf("                \"name\": \"");
                        print_json_escaped_string(fields[f].name, strlen(fields[f].name));
                        printf("\",\n");
                        printf("                \"value\": \"");
                        print_json_escaped_string(fields[f].value, strlen(fields[f].value));
                        printf("\"\n");
                        printf("              }");
                        if (f < form_fields - 1) {
                            printf(",");
                        }
                        printf("\n");
                    }
                    printf("            ],\n");
                }
                
                printf("            \"has_xfa_content\": %s,\n", xfa_content ? "true" : "false");
                printf("            \"pdf_commands_count\": %d\n", pdf_commands);
                printf("          },\n");
                
                // Free the fields array
                if (fields) {
                    free(fields);
                }
                
                // Full decompressed content
                printf("          \"decompressed_content\": \"");
                if (decompressed && decompressed_len > 0) {
                    print_json_escaped_string(decompressed, decompressed_len);
                } else {
                    printf("(empty)");
                }
                printf("\"\n");
                printf("        }");
                
                free(decompressed);
            } else {
                printf(",\n        \"decompression\": {\n");
                printf("          \"success\": false\n");
                printf("        }");
            }
        } else if (has_stream) {
            printf(",\n        \"decompression\": {\n");
            printf("          \"success\": false,\n");
            printf("          \"reason\": \"compression type not supported or no compression\"\n");
            printf("        }");
        }
        
        printf("\n      }");
        if (i < xref_table->count - 1) {
            printf(",");
        }
        printf("\n");
        
        if (obj.stream) free(obj.stream);
    }
    
    printf("    ]\n");
    printf("  }\n");
    printf("}\n");
}

// Parse XRef stream data to extract object references
// Read a big-endian unsigned integer of `width` bytes.
static unsigned long be_read(const unsigned char *p, int width) {
    unsigned long v = 0;
    for (int i = 0; i < width; i++) v = (v << 8) | p[i];
    return v;
}

// Decode a cross-reference stream. Adds type-1 objects with their byte offset
// and type-2 (in an object stream) objects with the -1 marker so they resolve
// lazily via the object-stream cache. Existing entries are not overwritten (a
// later section wins); parse_xref_table walks the /Prev chain to fold in the
// older sections.
void parse_xref_stream_data(const char *data, size_t data_len, const char *dict,
                            XRefTable *xref_table) {
    // /W [w0 w1 w2] field widths.
    int w[3] = {0, 0, 0};
    const char *wp = dict ? strstr(dict, "/W") : NULL;
    if (wp && (wp = strchr(wp, '['))) {
        wp++;
        for (int i = 0; i < 3 && *wp && *wp != ']'; i++) {
            while (*wp == ' ' || *wp == '\t' || *wp == '\n' || *wp == '\r') wp++;
            w[i] = atoi(wp);
            while (isdigit((unsigned char)*wp)) wp++;
        }
    }
    // Field widths must be sane byte counts. Without this, a crafted /W (e.g.
    // [1000000000 -499999999]) yields a small positive `rec` but a huge w[0],
    // and be_read(e, w[0]) reads far past the decompressed buffer (heap OOB).
    for (int i = 0; i < 3; i++)
        if (w[i] < 0 || w[i] > 8) return;   // unreadable /W -> skip this section
    int rec = w[0] + w[1] + w[2];
    if (rec <= 0) return;   // malformed /W -> skip this section

    // The stream arrives already decompressed and un-predicted (decompress_stream
    // reverses any /Predictor in /DecodeParms), so records are read straight from
    // `data`.
    const unsigned char *entries = (const unsigned char *)data;
    size_t nrec = data_len / (size_t)rec;

    // /Index [start count ...]; default [0 Size].
    int idx[64][2]; int npairs = 0;
    const char *ip = dict ? strstr(dict, "/Index") : NULL;
    if (ip && (ip = strchr(ip, '['))) {
        ip++;
        while (npairs < 64) {
            while (*ip == ' ' || *ip == '\t' || *ip == '\n' || *ip == '\r') ip++;
            if (!isdigit((unsigned char)*ip)) break;
            int start = atoi(ip); while (isdigit((unsigned char)*ip)) ip++;
            while (*ip == ' ' || *ip == '\t' || *ip == '\n' || *ip == '\r') ip++;
            if (!isdigit((unsigned char)*ip)) break;
            int count = atoi(ip); while (isdigit((unsigned char)*ip)) ip++;
            idx[npairs][0] = start; idx[npairs][1] = count; npairs++;
        }
    }
    if (npairs == 0) { idx[0][0] = 0; idx[0][1] = dict_int(dict, "/Size", 0); npairs = 1; }

    size_t ri = 0;
    for (int s = 0; s < npairs; s++) {
        int start = idx[s][0], count = idx[s][1];
        for (int j = 0; j < count && ri < nrec; j++, ri++) {
            const unsigned char *e = entries + ri * (size_t)rec;
            unsigned long type = (w[0] == 0) ? 1 : be_read(e, w[0]);
            unsigned long f2 = be_read(e + w[0], w[1]);
            unsigned long f3 = be_read(e + w[0] + w[1], w[2]);
            int obj = start + j;
            if (obj <= 0) continue;
            // O(1) dedup; the newest section (visited first) wins.
            if (type == 1)                        // uncompressed: f2 = byte offset
                xref_add(xref_table, obj, (long)f2);
            else if (type == 2)                   // compressed: f2 = ObjStm, f3 = index
                xref_add_compressed(xref_table, obj, (int)f2, (int)f3);
            // type 0 = free entry: skip
        }
    }
}

