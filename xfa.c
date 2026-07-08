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

#define _POSIX_C_SOURCE 200809L
#include "xfa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ==========================================================================
 * Minimal XML navigation over the raw datasets text (no XML library). The
 * datasets packet is small and well-formed, so we track element nesting by hand.
 * ========================================================================== */

// Given a pointer at a tag's '<', return the '>' that closes its open tag
// (skipping quoted attribute values), or NULL.
static const char *open_tag_end(const char *p) {
    p++;                                    // past '<'
    while (*p && *p != '>') {
        if (*p == '"' || *p == '\'') {      // skip a quoted attribute value
            char q = *p++;
            while (*p && *p != q) p++;
            if (*p) p++;
        } else {
            p++;
        }
    }
    return (*p == '>') ? p : NULL;
}

// For an element whose open tag starts at `open` (within [.., limit)), compute
// its content region [*cstart,*cend), the position just past the whole element
// (*after), and whether it is self-closing. Returns 1 on success.
static int element_extent(const char *open, const char *limit,
                          const char **cstart, const char **cend,
                          const char **after, int *self_closing) {
    const char *gt = open_tag_end(open);
    if (!gt || gt >= limit) return 0;
    if (gt > open && gt[-1] == '/') {       // <tag ... />
        *self_closing = 1;
        *cstart = *cend = gt;
        *after = gt + 1;
        return 1;
    }
    *self_closing = 0;
    *cstart = gt + 1;

    int depth = 1;
    const char *p = gt + 1;
    while (p < limit && depth > 0) {
        if (*p != '<') { p++; continue; }
        if (p[1] == '/') {                  // close tag
            const char *e = open_tag_end(p);
            depth--;
            if (depth == 0) {
                *cend = p;
                *after = e ? e + 1 : p;
                return 1;
            }
            p = e ? e + 1 : p + 1;
        } else if (p[1] == '!' || p[1] == '?') {   // comment / PI / decl
            const char *e = strchr(p, '>');
            p = e ? e + 1 : limit;
        } else {                            // nested open tag
            const char *e = open_tag_end(p);
            if (e && e[-1] == '/') p = e + 1;       // nested self-closing
            else { depth++; p = e ? e + 1 : p + 1; }
        }
    }
    return 0;                               // unbalanced
}

// Return the '<' of the idx-th direct child element named `name` within
// [start,end), or NULL.
static const char *find_nth_child(const char *start, const char *end,
                                  const char *name, int idx) {
    size_t nlen = strlen(name);
    int count = 0;
    const char *p = start;
    while (p < end) {
        if (*p != '<') { p++; continue; }
        if (p[1] == '/' || p[1] == '!' || p[1] == '?') {   // not a child open tag
            const char *e = strchr(p, '>');
            p = e ? e + 1 : end;
            continue;
        }
        const char *cs, *ce, *after;
        int sc;
        if (!element_extent(p, end, &cs, &ce, &after, &sc)) break;
        int match = (strncmp(p + 1, name, nlen) == 0);
        if (match) {
            char a = p[1 + nlen];
            match = (a == ' ' || a == '\t' || a == '\r' || a == '\n' || a == '/' || a == '>');
        }
        if (match) {
            if (count == idx) return p;
            count++;
        }
        p = after;
    }
    return NULL;
}

/* ==========================================================================
 * SOM path.
 * ========================================================================== */
typedef struct { char name[128]; int idx; } SomComp;

// Parse "a[0].b[1].c[0]" -> components. Returns the count.
static int parse_som(const char *s, SomComp *comps, int max) {
    int n = 0;
    const char *p = s;
    while (*p && n < max) {
        size_t k = 0;
        while (*p && *p != '.' && *p != '[' && k < sizeof(comps[n].name) - 1)
            comps[n].name[k++] = *p++;
        comps[n].name[k] = '\0';
        int idx = 0;
        if (*p == '[') { p++; idx = atoi(p); while (*p && *p != ']') p++; if (*p == ']') p++; }
        comps[n].idx = idx;
        if (k > 0) n++;
        if (*p == '.') p++;
    }
    return n;
}

/* ==========================================================================
 * Value splicing.
 * ========================================================================== */
static size_t escaped_len(const char *s) {
    size_t n = 0;
    for (; *s; s++)
        n += (*s == '&') ? 5 : (*s == '<' || *s == '>') ? 4 : 1;
    return n;
}

static char *append_escaped(char *o, const char *s) {
    for (; *s; s++) {
        if (*s == '&')      { memcpy(o, "&amp;", 5); o += 5; }
        else if (*s == '<') { memcpy(o, "&lt;", 4);  o += 4; }
        else if (*s == '>') { memcpy(o, "&gt;", 4);  o += 4; }
        else                *o++ = *s;
    }
    return o;
}

// Build a new XML string with the element at `open` (named `tag`) set to `value`.
static char *splice_value(const char *xml, const char *open, const char *xml_end,
                          const char *tag, const char *value) {
    const char *cs, *ce, *after;
    int sc;
    if (!element_extent(open, xml_end, &cs, &ce, &after, &sc)) return strdup(xml);

    size_t tlen = strlen(tag);
    size_t vlen = escaped_len(value);
    // Worst case output size: original + value + "></tag>" wrapper.
    char *out = malloc((size_t)(xml_end - xml) + vlen + tlen + 8);
    if (!out) return NULL;
    char *o = out;

    if (sc) {
        // "<tag .../>"  ->  "<tag ...>value</tag>"
        const char *slash = open_tag_end(open) - 1;   // the '/'
        memcpy(o, xml, (size_t)(slash - xml)); o += (slash - xml);
        *o++ = '>';
        o = append_escaped(o, value);
        *o++ = '<'; *o++ = '/'; memcpy(o, tag, tlen); o += tlen; *o++ = '>';
        memcpy(o, after, (size_t)(xml_end - after)); o += (xml_end - after);
    } else {
        // "<tag ...>old</tag>"  ->  replace old (content region [cs,ce))
        memcpy(o, xml, (size_t)(cs - xml)); o += (cs - xml);
        o = append_escaped(o, value);
        memcpy(o, ce, (size_t)(xml_end - ce)); o += (xml_end - ce);
    }
    *o = '\0';
    return out;
}

char *xfa_datasets_set(const char *xml, const char *field_name, const char *value) {
    const char *xml_end = xml + strlen(xml);

    // The data root: <xfa:data> ... </xfa:data>. Match "<xfa:data" only when the
    // next char is a tag delimiter, so it does not match "<xfa:datasets".
    const char *data = NULL;
    for (const char *p = xml; (p = strstr(p, "<xfa:data")) != NULL; p += 9) {
        char a = p[9];
        if (a == '>' || a == '/' || a == ' ' || a == '\t' || a == '\r' || a == '\n') { data = p; break; }
    }
    const char *root_start, *root_end, *after;
    int sc;
    if (!data || !element_extent(data, xml_end, &root_start, &root_end, &after, &sc) || sc)
        return strdup(xml);

    SomComp comps[64];
    int nc = parse_som(field_name, comps, 64);
    if (nc == 0) return strdup(xml);

    // Navigate to the target element, skipping components whose element is not a
    // direct child (transparent subforms).
    const char *node_start = root_start, *node_end = root_end;
    const char *target = NULL;
    const char *target_tag = comps[nc - 1].name;

    for (int k = 0; k < nc; k++) {
        const char *child = find_nth_child(node_start, node_end, comps[k].name, comps[k].idx);
        if (!child) {
            if (k == nc - 1) break;         // leaf missing on this path -> fall back
            continue;                        // transparent container -> skip
        }
        if (k == nc - 1) { target = child; break; }
        const char *cs, *ce, *af;
        int s;
        if (!element_extent(child, node_end, &cs, &ce, &af, &s) || s) break;
        node_start = cs;
        node_end = ce;
    }

    // Fallback: if the path did not resolve, set the first element with the leaf
    // name anywhere in the datasets (correct when the leaf is unique).
    if (!target)
        target = find_nth_child(root_start, root_end, target_tag, 0);
    if (!target) {
        // deep search: the leaf may be nested below the data root's direct children
        const char *p = root_start;
        size_t tlen = strlen(target_tag);
        while ((p = strchr(p, '<')) != NULL && p < root_end) {
            if (strncmp(p + 1, target_tag, tlen) == 0) {
                char a = p[1 + tlen];
                if (a == ' ' || a == '\t' || a == '\r' || a == '\n' || a == '/' || a == '>') { target = p; break; }
            }
            p++;
        }
    }
    if (!target) return strdup(xml);

    char *out = splice_value(xml, target, xml_end, target_tag, value);
    return out ? out : strdup(xml);
}
