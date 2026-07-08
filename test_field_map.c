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

/* Unit tests for the pure dictionary/field helpers in field_map.c.
 *
 * These exercise the byte-level PDF parsing that has been the main source of
 * bugs (string-aware key matching, balanced dict scanning, hex field names,
 * inline-vs-indirect arrays). They need no PDF file, so they run instantly.
 *
 * field_map.c is #included directly so the static helpers can be tested as a
 * white box. Build/run with `make test`.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "field_map.c"
#include "xfa.h"          // xfa_datasets_set (linked from xfa.o)

static int g_checks = 0, g_fails = 0;

#define CHECK(cond) do {                                                      \
    g_checks++;                                                              \
    if (!(cond)) { g_fails++;                                                 \
        fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }     \
} while (0)

// Convenience: does find_key return a value starting with `prefix`?
static int key_val_is(const char *dict, const char *key, const char *prefix) {
    const char *v = find_key(dict, key);
    return v && strncmp(v, prefix, strlen(prefix)) == 0;
}

static void test_find_key(void) {
    // Basic top-level lookup.
    CHECK(key_val_is("<</T(abc)/V(x)>>", "/V", "(x)"));
    CHECK(key_val_is("<</FT/Tx/Ff 1>>", "/FT", "/Tx"));

    // Prefix keys must not match: "/V" is not inside "/DV".
    CHECK(find_key("<</DV(x)>>", "/V") == NULL);

    // String-awareness: a "/V" that only appears inside another key's string
    // value must not be treated as a key.
    CHECK(find_key("<</TU(press /V now)/FT/Tx>>", "/V") == NULL);

    // ...but a real top-level /V after such a string must still be found.
    CHECK(key_val_is("<</TU(press /V now)/V(real)>>", "/V", "(real)"));

    // Keys inside a nested dict are not top-level.
    CHECK(find_key("<</MK<</BC(x)>>/Ff 1>>", "/BC") == NULL);
}

static void test_match_dict_end(void) {
    const char *a = "<</A 1>>";
    CHECK(match_dict_end(a) == a + strlen(a));

    // ">>" inside a literal string must not end the dictionary.
    const char *b = "<</V(a>>b)>>";
    CHECK(match_dict_end(b) == b + strlen(b));

    // Nested dictionaries balance correctly.
    const char *c = "<</MK<<>>/X 1>>";
    CHECK(match_dict_end(c) == c + strlen(c));

    // Unbalanced / truncated -> NULL.
    CHECK(match_dict_end("<</A 1") == NULL);
}

static void test_extract_dict_inner(void) {
    char out[512];
    CHECK(extract_dict_inner("<</A 1>>", out, sizeof(out)) == 0 && strcmp(out, "/A 1") == 0);

    // Preserve ">>" that lives inside a string value.
    CHECK(extract_dict_inner("<</V(a>>b)>>", out, sizeof(out)) == 0 && strcmp(out, "/V(a>>b)") == 0);

    // Truncated dictionary is reported, not silently accepted.
    CHECK(extract_dict_inner("<</A 1", out, sizeof(out)) == -1);
}

static void test_remove_entry(void) {
    char buf[256];

    strcpy(buf, "/T(f)/V(old)/TU(keep)");
    remove_entry(buf, "/V");
    CHECK(strstr(buf, "/V(old)") == NULL);
    CHECK(strstr(buf, "/T(f)") != NULL);
    CHECK(strstr(buf, "/TU(keep)") != NULL);

    // String-aware: the "/V" inside the /TU string must survive; only the real
    // top-level /V is removed.
    strcpy(buf, "/TU(a /V b)/V(real)");
    remove_entry(buf, "/V");
    CHECK(strstr(buf, "/TU(a /V b)") != NULL);
    CHECK(strstr(buf, "/V(real)") == NULL);
}

static void test_read_partial_name(void) {
    char name[256];

    // Literal string, with an escaped ')'.
    CHECK(read_partial_name("<</T(a\\)b)/FT/Tx>>", name, sizeof(name)) && strcmp(name, "a)b") == 0);

    // Hex UTF-16BE with a BOM: FEFF 0068 0069 -> "hi".
    CHECK(read_partial_name("<</T<FEFF00680069>>>", name, sizeof(name)) && strcmp(name, "hi") == 0);

    // No /T -> no name.
    CHECK(read_partial_name("<</FT/Tx>>", name, sizeof(name)) == 0);
}

static void test_field_type(void) {
    CHECK(field_type_from_dict("<</FT/Tx>>", 0) == 'T');
    CHECK(field_type_from_dict("<</FT/Btn>>", 0) == 'B');
    CHECK(field_type_from_dict("<</FT/Ch>>", 0) == 'C');
    // No /FT -> inherit the parent's type.
    CHECK(field_type_from_dict("<</Ff 1>>", 'B') == 'B');
}

static void test_skip_value(void) {
    CHECK(strcmp(skip_value("(abc)tail"), "tail") == 0);
    CHECK(strcmp(skip_value("/Name tail"), " tail") == 0);
    CHECK(strcmp(skip_value("[1 2 3]tail"), "tail") == 0);
    CHECK(strcmp(skip_value("12 0 R tail"), " tail") == 0);   // indirect reference
    CHECK(strcmp(skip_value("<AABB>tail"), "tail") == 0);     // hex string
}

static void test_field_array_text_inline(void) {
    // The inline-array branch of field_array_text touches neither FILE nor xref,
    // so it can be tested directly. (Indirect arrays are covered end-to-end.)
    char *a = field_array_text(NULL, NULL, "<</DA(x)/Fields[1 0 R 2 0 R]/XFA[9 0 R]>>", "/Fields");
    CHECK(a != NULL && strcmp(a, "[1 0 R 2 0 R]") == 0);
    free(a);

    // Absent key -> NULL.
    CHECK(field_array_text(NULL, NULL, "<</DA(x)>>", "/Fields") == NULL);
}

static void test_xfa_datasets_set(void) {
    char *r;

    // Self-closing element -> content, with a transparent container (Page1) that
    // is absent from the data and must be skipped.
    const char *f8821 = "<xfa:datasets><xfa:data><topmostSubform>"
                        "<Pg1Header><f1_1/></Pg1Header><f1_6/>"
                        "</topmostSubform></xfa:data></xfa:datasets>";
    r = xfa_datasets_set(f8821, "topmostSubform[0].Page1[0].Pg1Header[0].f1_1[0]", "Jane");
    CHECK(strstr(r, "<f1_1>Jane</f1_1>") != NULL); free(r);
    r = xfa_datasets_set(f8821, "topmostSubform[0].Page1[0].f1_6[0]", "Y");
    CHECK(strstr(r, "<f1_6>Y</f1_6>") != NULL); free(r);

    // Existing content is replaced; value is XML-escaped.
    r = xfa_datasets_set("<xfa:datasets><xfa:data><A><c1>0</c1></A></xfa:data></xfa:datasets>",
                         "A[0].c1[0]", "1");
    CHECK(strstr(r, "<c1>1</c1>") != NULL); free(r);
    r = xfa_datasets_set("<xfa:datasets><xfa:data><A><x/></A></xfa:data></xfa:datasets>",
                         "A[0].x[0]", "a<b&c");
    CHECK(strstr(r, "<x>a&lt;b&amp;c</x>") != NULL); free(r);

    // Leaf-name collision: two <amount> under two occurrences of a repeated
    // subform. Path + occurrence must target the RIGHT one (a plain leaf match
    // would set both on the first).
    const char *coll = "<xfa:datasets><xfa:data><Table>"
                       "<Row><amount/></Row><Row><amount/></Row>"
                       "</Table></xfa:data></xfa:datasets>";
    char *a = xfa_datasets_set(coll, "f[0].Table[0].Row[0].amount[0]", "AAA");
    char *b = xfa_datasets_set(a,    "f[0].Table[0].Row[1].amount[0]", "BBB");
    const char *pa = strstr(b, "<amount>AAA</amount>");
    const char *pb = strstr(b, "<amount>BBB</amount>");
    CHECK(pa != NULL);              // first row kept AAA
    CHECK(pb != NULL);              // second row got BBB
    CHECK(pa && pb && pa < pb);     // in order -> not both written to the first
    free(a); free(b);

    // Unknown leaf -> unchanged.
    r = xfa_datasets_set("<xfa:datasets><xfa:data><A><x/></A></xfa:data></xfa:datasets>",
                         "A[0].zzz[0]", "v");
    CHECK(strstr(r, "zzz") == NULL); free(r);
}

static int hexeq(const unsigned char *b, int n, const char *hexexp) {
    char buf[256];
    for (int i = 0; i < n; i++) sprintf(buf + 2*i, "%02x", b[i]);
    buf[2*n] = '\0';
    return strcmp(buf, hexexp) == 0;
}

static void test_crypto(void) {
    unsigned char h[64];
    md5((const unsigned char *)"abc", 3, h);
    CHECK(hexeq(h, 16, "900150983cd24fb0d6963f7d28e17f72"));
    sha256((const unsigned char *)"abc", 3, h);
    CHECK(hexeq(h, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    sha384((const unsigned char *)"abc", 3, h);
    CHECK(hexeq(h, 48, "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
                       "8086072ba1e7cc2358baeca134c825a7"));
    sha512((const unsigned char *)"abc", 3, h);
    CHECK(hexeq(h, 64, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                       "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));

    // RC4 known-answer (key "Key", plaintext "Plaintext").
    unsigned char rc[16];
    rc4((const unsigned char *)"Key", 3, (const unsigned char *)"Plaintext", 9, rc);
    CHECK(hexeq(rc, 9, "bbf316e8d940af0ad3"));

    // Per-object encrypt -> decrypt round-trip for each PDF cipher.
    const char *msg = "Hello, PDF encryption! cafe\xc3\xa9";
    size_t ml = strlen(msg);
    int cfms[3]  = { CFM_RC4, CFM_AESV2, CFM_AESV3 };
    int klens[3] = { 16, 16, 32 };
    for (int i = 0; i < 3; i++) {
        PdfCrypt c;
        memset(&c, 0, sizeof(c));
        c.cfm = cfms[i];
        c.key_len = klens[i];
        for (int k = 0; k < c.key_len; k++) c.key[k] = (unsigned char)(k*7 + 1);
        unsigned char enc[256], dec[256];
        int el = pdf_encrypt(&c, 12, 0, (const unsigned char *)msg, ml, enc, sizeof(enc));
        CHECK(el > 0);
        int dl = pdf_decrypt(&c, 12, 0, enc, el, dec);
        CHECK(dl == (int)ml && memcmp(dec, msg, ml) == 0);
        // RC4/AESV2 derive a per-object key, so a wrong object number must fail.
        if (cfms[i] != CFM_AESV3) {
            int dl2 = pdf_decrypt(&c, 99, 0, enc, el, dec);
            CHECK(!(dl2 == (int)ml && memcmp(dec, msg, ml) == 0));
        }
    }
}

static void test_lzw(void) {
    // The canonical PDF-spec (ISO 32000, 7.4.4.2) LZWDecode example: the string
    // "-----A---B" encodes to 80 0B 60 50 22 0C 0C 85 01.
    const char spec_in[]  = "-----A---B";
    const unsigned char spec_lzw[] = { 0x80,0x0b,0x60,0x50,0x22,0x0c,0x0c,0x85,0x01 };
    size_t ol = 0;
    char *d = decompress_lzw((const char *)spec_lzw, sizeof(spec_lzw), &ol);
    CHECK(d && ol == strlen(spec_in) && memcmp(d, spec_in, ol) == 0);
    free(d);

    // A repetitive string that forces the dictionary to grow.
    const char rep_in[] = "ABABABABABABABABABABABABABAB";
    const unsigned char rep_lzw[] = {
        0x80,0x10,0x48,0x50,0x28,0x24,0x0e,0x0d,0x05,0x84,0x41,0xe0,0xd0,0x10 };
    d = decompress_lzw((const char *)rep_lzw, sizeof(rep_lzw), &ol);
    CHECK(d && ol == strlen(rep_in) && memcmp(d, rep_in, ol) == 0);
    free(d);
}

int main(void) {
    test_find_key();
    test_crypto();
    test_lzw();
    test_match_dict_end();
    test_extract_dict_inner();
    test_remove_entry();
    test_read_partial_name();
    test_field_type();
    test_skip_value();
    test_field_array_text_inline();
    test_xfa_datasets_set();

    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
