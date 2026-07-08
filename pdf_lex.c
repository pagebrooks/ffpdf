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

#include "pdf_lex.h"

#include <string.h>
#include <ctype.h>
#include <stdlib.h>

// Skip a literal string starting at '(' (handling escapes and balanced parens);
// return a pointer just past the closing ')'.
static const char *skip_literal_string(const char *p) {
    int depth = 1; p++;
    while (*p && depth > 0) {
        if (*p == '\\' && p[1]) { p += 2; continue; }
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        p++;
    }
    return p;
}

const char *match_dict_end(const char *start) {
    const char *p = start + 2;
    int depth = 1;
    while (*p) {
        if (*p == '(') { p = skip_literal_string(p); continue; }
        if (p[0] == '<' && p[1] == '<') { depth++; p += 2; continue; }
        if (p[0] == '>' && p[1] == '>') { depth--; p += 2; if (depth == 0) return p; continue; }
        if (*p == '<') {                      // hex string
            p++;
            while (*p && *p != '>') p++;
            if (*p == '>') p++;
            continue;
        }
        p++;
    }
    return NULL;
}

// Find `key` (e.g. "/T") as a top-level dictionary key in `dict`, returning a
// pointer to its leading '/', or NULL. Skips over string/array/nested-dict
// contents so the same byte sequence inside a value (e.g. "/V" inside a /TU
// string) is not mistaken for a key. `dict` may or may not include the outer
// "<<"/">>"; keys are matched at the outermost dictionary level either way.
static const char *find_key_pos(const char *dict, const char *key) {
    size_t klen = strlen(key);
    int top = (dict[0] == '<' && dict[1] == '<') ? 1 : 0;
    int depth = 0;
    const char *p = dict;
    while (*p) {
        if (*p == '(') { p = skip_literal_string(p); continue; }
        if (p[0] == '<' && p[1] == '<') { depth++; p += 2; continue; }
        if (p[0] == '>' && p[1] == '>') { depth--; p += 2; continue; }
        if (*p == '<') { p++; while (*p && *p != '>') p++; if (*p == '>') p++; continue; }  // hex
        if (*p == '[') { depth++; p++; continue; }
        if (*p == ']') { depth--; p++; continue; }
        if (*p == '/' && depth == top && strncmp(p, key, klen) == 0) {
            char nc = p[klen];
            if (!(isalnum((unsigned char)nc) || nc == '_' || nc == '.' || nc == '-'))
                return p;
        }
        p++;
    }
    return NULL;
}

const char *find_key(const char *dict, const char *key) {
    const char *kp = find_key_pos(dict, key);
    if (!kp) return NULL;
    const char *v = kp + strlen(key);
    while (*v == ' ' || *v == '\n' || *v == '\r' || *v == '\t' || *v == '\f') v++;
    return v;
}

const char *skip_value(const char *v) {
    if (*v == '(') {                       // literal string
        int depth = 1; v++;
        while (*v && depth > 0) {
            if (*v == '\\' && v[1]) { v += 2; continue; }
            if (*v == '(') depth++;
            else if (*v == ')') depth--;
            v++;
        }
        return v;
    }
    if (v[0] == '<' && v[1] == '<') {      // dictionary
        int depth = 1; v += 2;
        while (*v && depth > 0) {
            if (v[0] == '<' && v[1] == '<') { depth++; v += 2; }
            else if (v[0] == '>' && v[1] == '>') { depth--; v += 2; }
            else v++;
        }
        return v;
    }
    if (*v == '<') {                       // hex string
        v++;
        while (*v && *v != '>') v++;
        if (*v == '>') v++;
        return v;
    }
    if (*v == '[') {                       // array
        int depth = 1; v++;
        while (*v && depth > 0) {
            if (*v == '[') depth++;
            else if (*v == ']') depth--;
            v++;
        }
        return v;
    }
    if (*v == '/') {                       // name
        v++;
        while (*v && !isspace((unsigned char)*v) && !strchr("()<>[]{}/%", *v)) v++;
        return v;
    }
    // number/boolean/keyword token, possibly an indirect ref "N G R"
    const char *tok_end = v;
    while (*tok_end && !isspace((unsigned char)*tok_end) && !strchr("()<>[]{}/%", *tok_end)) tok_end++;
    const char *r = tok_end;
    while (*r == ' ' || *r == '\t' || *r == '\n' || *r == '\r') r++;
    if (isdigit((unsigned char)*r)) {      // maybe "N G R"
        while (isdigit((unsigned char)*r)) r++;
        while (*r == ' ' || *r == '\t') r++;
        if (*r == 'R') return r + 1;
    }
    return tok_end;
}

void remove_entry(char *dict, const char *key) {
    size_t klen = strlen(key);
    char *p;
    while ((p = (char *)find_key_pos(dict, key)) != NULL) {
        const char *v = p + klen;
        while (*v == ' ' || *v == '\n' || *v == '\r' || *v == '\t' || *v == '\f') v++;
        const char *vend = skip_value(v);
        memmove(p, vend, strlen(vend) + 1);   // includes NUL
    }
}

int extract_dict_inner(const char *obj_dict, char *out, size_t out_size) {
    const char *open = strstr(obj_dict, "<<");
    if (!open) return -1;
    const char *end = match_dict_end(open);   // string-aware; just past ">>"
    if (!end) return -1;                       // unbalanced => truncated dictionary
    const char *inner = open + 2;
    size_t len = (size_t)((end - 2) - inner);
    if (len >= out_size) return -1;
    memcpy(out, inner, len);
    out[len] = '\0';
    return 0;
}

const char *dict_find(const char *dict, const char *key) {
    const char *p = dict ? strstr(dict, key) : NULL;
    if (!p) return NULL;
    p += strlen(key);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

int dict_int(const char *dict, const char *key, int dflt) {
    const char *p = dict_find(dict, key);
    if (!p || !isdigit((unsigned char)*p)) return dflt;
    return atoi(p);
}

const char *read_literal(const char *p, char *out, size_t out_size) {
    p++;   // skip '('
    int depth = 1;
    size_t k = 0;
    while (*p && depth > 0) {
        if (*p == '\\' && p[1]) { p++; if (k < out_size - 1) out[k++] = *p; p++; continue; }
        if (*p == '(') depth++;
        else if (*p == ')') { depth--; if (depth == 0) { p++; break; } }
        if (k < out_size - 1) out[k++] = *p;
        p++;
    }
    out[k] = '\0';
    return p;
}

const char *read_literal_alloc(const char *p, char **out) {
    p++;   // skip '('
    int depth = 1;
    size_t k = 0, cap = 64;
    char *buf = malloc(cap);
    if (!buf) { *out = NULL; return p; }
    while (*p && depth > 0) {
        char c;
        if (*p == '\\' && p[1]) { p++; c = *p++; }
        else if (*p == '(') { depth++; c = *p++; }
        else if (*p == ')') { depth--; if (depth == 0) { p++; break; } c = *p++; }
        else c = *p++;
        if (k + 1 >= cap) { cap *= 2; char *g = realloc(buf, cap); if (!g) { free(buf); *out = NULL; return p; } buf = g; }
        buf[k++] = c;
    }
    buf[k] = '\0';
    *out = buf;
    return p;
}

char *extract_dict_inner_alloc(const char *obj_dict) {
    const char *open = strstr(obj_dict, "<<");
    if (!open) return NULL;
    const char *end = match_dict_end(open);
    if (!end) return NULL;
    const char *inner = open + 2;
    size_t len = (size_t)((end - 2) - inner);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, inner, len);
    out[len] = '\0';
    return out;
}

int parse_ref_num(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return atoi(p);
}

void parse_indirect_ref(const char *p, int *num, int *gen) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    *num = atoi(p);
    while (isdigit((unsigned char)*p)) p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    *gen = atoi(p);
}
