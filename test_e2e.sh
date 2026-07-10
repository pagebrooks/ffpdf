#!/bin/bash
# End-to-end smoke test for ffpdf.
#
# Exercises the real binary across the extract -> fill round-trip and the
# indirect-/Fields case, asserting on observable behaviour (valid FDF/JSON, a
# structurally valid filled PDF whose xref offsets resolve, values present).
# Complements the fast unit tests in test_field_map.c. Requires python3.
#
# Run with `make test-e2e` (or `make check` for unit + e2e).
set -u
cd "$(dirname "$0")" || exit 1

BIN=./ffpdf
PDF=test/pdfs/f8821.pdf
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fails=0
pass() { echo "  ok:   $1"; }
fail() { echo "  FAIL: $1"; fails=$((fails + 1)); }

# ---- prerequisites -------------------------------------------------------
[ -x "$BIN" ] || { echo "building $BIN..."; make >/dev/null || exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is required for the e2e test"; exit 2; }
[ -f "$PDF" ] || { echo "missing fixture: $PDF"; exit 2; }

echo "== command-line interface =="
$BIN --help >/dev/null 2>&1 && pass "--help exits 0" || fail "--help exit status"
$BIN --version 2>/dev/null | grep -q "$($BIN --version 2>/dev/null | head -1)" \
    && $BIN --version 2>/dev/null | grep -qi 'Apache' && pass "--version prints banner + license" || fail "--version output"
$BIN -h 2>/dev/null | grep -q 'USAGE' && pass "-h shows usage sections" || fail "-h usage"
$BIN 2>/dev/null; [ "$?" -eq 1 ] && pass "no args exits 1" || fail "no-args exit status"
$BIN -bogus >/dev/null 2>"$TMP/err"; ec=$?
[ "$ec" -eq 1 ] && grep -q "unknown command" "$TMP/err" && pass "unknown command -> stderr + exit 1" || fail "unknown-command handling"
# help goes to stdout, not stderr (so `cmd --help | less` works)
$BIN --help 2>/dev/null | grep -q USAGE && pass "--help writes to stdout" || fail "--help stream"

echo "== non-ASCII file path =="
# Exercises portable_fopen with a UTF-8 path (a no-op wrapper on POSIX, but on
# Windows it routes through _wfopen). Regression guard for the Unicode-path work.
UPATH="$TMP/tëst_förm_日本.pdf"
cp "$PDF" "$UPATH"
n=$($BIN fdf-extract "$UPATH" 2>/dev/null | grep -c '^/T (')
[ "$n" -gt 0 ] && pass "opens a non-ASCII path ($n fields)" || fail "non-ASCII path failed to open"

echo "== extraction =="
$BIN fdf-extract "$PDF" 2>/dev/null > "$TMP/out.fdf"
head -c 8 "$TMP/out.fdf" | grep -q '%FDF' && pass "fdf-extract emits an FDF header" || fail "fdf-extract header"
nfields=$(grep -c '^/T (' "$TMP/out.fdf")
[ "$nfields" -gt 0 ] && pass "fdf-extract found $nfields field(s)" || fail "fdf-extract found no fields"
if grep -qE 'DEBUG|EXTENDS|decompress' "$TMP/out.fdf"; then
    fail "fdf-extract leaked diagnostics onto stdout"
else
    pass "fdf-extract stdout is clean"
fi
$BIN xref "$PDF" 2>/dev/null | python3 -c 'import json,sys; json.load(sys.stdin)' \
    && pass "xref is valid JSON" || fail "xref invalid JSON"

echo "== fill round-trip =="
# Take a field name straight from fdf-extract and feed it back to fill (proves the two
# paths agree on names).
NAME=$(grep -oE '^/T \(([^)]*)\)' "$TMP/out.fdf" | head -1 | sed 's|^/T (||; s|)$||')
[ -n "$NAME" ] && pass "picked field: $NAME" || fail "no field name to fill"
printf '%%FDF-1.2\n1 0 obj\n<< /FDF << /Fields [ << /T (%s) /V (SMOKE_TEST_VALUE) >> ] >> >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%%%EOF\n' "$NAME" > "$TMP/in.fdf"
$BIN fill "$TMP/in.fdf" "$PDF" 2>/dev/null > "$TMP/filled.pdf"
head -c 5 "$TMP/filled.pdf" | grep -q '%PDF' && pass "filled output is a PDF" || fail "filled output not a PDF"
grep -qa 'SMOKE_TEST_VALUE' "$TMP/filled.pdf" && pass "filled value present" || fail "filled value missing"
# f8821 is a dynamic-XFA form: the value must also land in the XFA datasets XML
# (as element content ">value</"), not only the AcroForm /V.
grep -qa '>SMOKE_TEST_VALUE</' "$TMP/filled.pdf" \
    && pass "XFA datasets synced" || fail "XFA datasets not synced"
# A text appearance stream must be generated so non-regenerating viewers render
# the value: the widget gains /AP /N -> a Form XObject that draws it with Tj.
python3 - "$TMP/filled.pdf" <<'PY' && pass "text appearance stream generated" || fail "no appearance stream"
import sys, re
d = open(sys.argv[1], 'rb').read()
m = re.search(rb'/AP<</N (\d+) 0 R>>', d)
assert m, "field has no /AP /N reference"
n = int(m.group(1))
ap = re.search((r"\b%d 0 obj" % n).encode(), d)
seg = d[ap.start():ap.start()+800]
assert b'/Subtype/Form' in seg, "AP object is not a Form XObject"
assert b'(SMOKE_TEST_VALUE) Tj' in seg, "AP does not draw the value"
PY

# Validate that the appended xref stream's offsets each resolve to the right
# object header, and that NeedAppearances was set.
python3 - "$TMP/filled.pdf" <<'PY' && pass "incremental xref offsets resolve" || fail "xref offsets invalid"
import sys, re
d = open(sys.argv[1], 'rb').read()
sx = d.rfind(b'startxref'); off = int(d[sx:].split()[1]); seg = d[off:off+4000]
mi = re.search(rb'/Index\[([0-9 ]+)\]', seg)
mw = re.search(rb'/W\[(\d+) (\d+) (\d+)\]', seg)
assert mi and mw, "no xref-stream dict at startxref"
W = tuple(map(int, mw.groups()))
sd = seg.split(b'stream\n', 1)[1].split(b'\nendstream', 1)[0]
idx = list(map(int, mi.group(1).split())); objs = []; i = 0
while i < len(idx):
    for c in range(idx[i+1]): objs.append(idx[i] + c)
    i += 2
rec = sum(W)
for n, rn in enumerate(objs):
    o = int.from_bytes(sd[n*rec+1:n*rec+1+W[1]], 'big')
    head = d[o:o+40].split(b'obj')[0].strip()
    assert head == f"{rn} 0".encode(), f"obj {rn}: offset {o} -> {head!r}"
assert b'/NeedAppearances true' in d, "NeedAppearances not set"
PY

echo "== fill output must be re-readable by ffpdf itself =="
# The appended xref stream is unfiltered; decompress_stream once rejected any
# stream without /Filter, so ffpdf could not parse its own output (found via
# the radio-group exploration, 2026-07-09).
$BIN fdf-extract "$TMP/filled.pdf" 2>/dev/null | grep -q "SMOKE_TEST_VALUE" \
    && pass "round-trip: extract reads the filled value back" \
    || fail "ffpdf cannot re-read its own fill output"

echo "== fields: JSON listing (types, options, on-states, values) =="
# The agent-facing contract: names, types, current values, choice options and
# flags, checkbox on-states. Uses the committed example form (14 fields, every
# type) and its filled version.
$BIN fields docs/example-form.pdf 2>/dev/null > "$TMP/fields.json"
python3 - "$TMP/fields.json" <<'PY' && pass "fields: unfilled form JSON contract" || fail "fields JSON wrong (unfilled)"
import json, sys
d = json.load(open(sys.argv[1]))
f = {x["name"]: x for x in d["fields"]}
assert d["count"] == 14
assert f["FullName"]["type"] == "text" and f["FullName"]["value"] == ""
assert f["CoverageType"]["combo"] and not f["CoverageType"]["multi_select"]
assert f["CoverageType"]["options"] == ["Auto", "Home", "Life", "Umbrella"]
assert f["AdditionalCoverages"]["multi_select"]
assert f["PaperlessBilling"]["on_state"] == "On"
assert f["Signature"]["type"] == "signature" and f["Signature"]["value"] is None
# metadata for agents: /TU labels, required (/Ff *-marked), maxlen (/MaxLen)
assert f["FullName"]["label"] == "Full name"
assert f["State"]["label"] == "State" and f["State"]["maxlen"] == 2
assert f["Zip"]["maxlen"] == 10
assert f["FullName"]["required"] is True          # "Full name *"
assert f["Phone"]["required"] is False            # "Phone" (no *)
assert all(x["readonly"] is False for x in d["fields"])
# document-level flags and per-field page numbers
assert d["xfa"] is False and d["dynamic_xfa"] is False
assert all(x["page"] == 1 for x in d["fields"])
PY
$BIN fields docs/example-filled.pdf 2>/dev/null > "$TMP/fields_filled.json"
python3 - "$TMP/fields_filled.json" <<'PY' && pass "fields: filled values incl. multi-select array" || fail "fields JSON wrong (filled)"
import json, sys
f = {x["name"]: x for x in json.load(open(sys.argv[1]))["fields"]}
assert f["FullName"]["value"] == "Avery Whitfield"
assert f["CoverageType"]["value"] == "Home"
assert f["AdditionalCoverages"]["value"] == ["Flood", "Identity theft"]
assert f["PaperlessBilling"]["value"] == "On"
PY
# Real-world file (compressed object streams): output must stay valid JSON.
$BIN fields "$PDF" 2>/dev/null | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["count"] == 45
assert d["xfa"] is True and d["dynamic_xfa"] is False        # static XFA
assert all(x.get("page") == 1 for x in d["fields"])          # via /Annots (no /P)
' && pass "fields: f8821 JSON incl. xfa flag + page numbers" || fail "fields JSON invalid for f8821"
# readonly flag (/Ff bit 1) on a minimal fixture.
python3 - "$TMP/ro.pdf" <<'PY'
import sys
objs = {
 1: b"<</Type/Catalog/Pages 2 0 R/AcroForm 4 0 R>>",
 2: b"<</Type/Pages/Kids[3 0 R]/Count 1>>",
 3: b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 200 200]/Annots[5 0 R]>>",
 4: b"<</Fields[5 0 R]/DA(/Helv 0 Tf 0 g)>>",
 5: b"<</FT/Tx/Ff 1/T(locked)/V(preset)/Type/Annot/Subtype/Widget/Rect[10 10 90 30]/P 3 0 R>>",
}
o = bytearray(b"%PDF-1.7\n"); off = {}
for n in sorted(objs):
    off[n] = len(o); o += b"%d 0 obj\n" % n + objs[n] + b"\nendobj\n"
x = len(o)
o += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs)+1)
o += b"".join(b"%010d 00000 n \n" % off[n] for n in sorted(objs))
o += b"trailer\n<</Size %d/Root 1 0 R>>\nstartxref\n%d\n%%%%EOF\n" % (len(objs)+1, x)
open(sys.argv[1], "wb").write(o)
PY
$BIN fields "$TMP/ro.pdf" 2>/dev/null | python3 -c '
import json, sys
f = json.load(sys.stdin)["fields"][0]
assert f["readonly"] is True and f["required"] is False and f["value"] == "preset"
' && pass "fields: readonly flag reported" || fail "fields readonly flag wrong"
# page numbers across a 2-page form (one field per page).
python3 - "$TMP/2pg.pdf" <<'PY'
import sys
objs = {
 1: b"<</Type/Catalog/Pages 2 0 R/AcroForm 7 0 R>>",
 2: b"<</Type/Pages/Kids[3 0 R 4 0 R]/Count 2>>",
 3: b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 200 200]/Annots[5 0 R]>>",
 4: b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 200 200]/Annots[6 0 R]>>",
 5: b"<</FT/Tx/T(p1)/V()/Type/Annot/Subtype/Widget/Rect[10 10 90 30]/P 3 0 R>>",
 6: b"<</FT/Tx/T(p2)/V()/Type/Annot/Subtype/Widget/Rect[10 10 90 30]/P 4 0 R>>",
 7: b"<</Fields[5 0 R 6 0 R]/DA(/Helv 0 Tf 0 g)>>",
}
o = bytearray(b"%PDF-1.7\n"); off = {}
for n in sorted(objs):
    off[n] = len(o); o += b"%d 0 obj\n" % n + objs[n] + b"\nendobj\n"
x = len(o)
o += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs)+1)
o += b"".join(b"%010d 00000 n \n" % off[n] for n in sorted(objs))
o += b"trailer\n<</Size %d/Root 1 0 R>>\nstartxref\n%d\n%%%%EOF\n" % (len(objs)+1, x)
open(sys.argv[1], "wb").write(o)
PY
$BIN fields "$TMP/2pg.pdf" 2>/dev/null | python3 -c '
import json, sys
f = {x["name"]: x["page"] for x in json.load(sys.stdin)["fields"]}
assert f["p1"] == 1 and f["p2"] == 2, f
' && pass "fields: page number differs per page" || fail "fields page numbering wrong"

echo "== radio group: discovery options + fill sets kid /AS =="
python3 - "$TMP/radio.pdf" <<'PY'
import sys
opts = ["Email", "Phone", "Mail"]
objs = {
 1: b"<</Type/Catalog/Pages 2 0 R/AcroForm 3 0 R>>",
 2: b"<</Type/Pages/Kids[4 0 R]/Count 1>>",
 3: b"<</Fields[8 0 R]/DA(/Helv 0 Tf 0 g)>>",
 4: b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 300 200]/Annots[5 0 R 6 0 R 7 0 R]>>",
 8: b"<</FT/Btn/Ff 32768/T(ContactMethod)/TU(Preferred contact)/V/Off/Kids[5 0 R 6 0 R 7 0 R]>>",
 9: b"<</Type/XObject/Subtype/Form/BBox[0 0 14 14]/Length 0>>\nstream\nendstream",
 10: b"<</Type/XObject/Subtype/Form/BBox[0 0 14 14]/Length 0>>\nstream\nendstream",
}
for i, o in enumerate(opts):
    x = 10 + i * 90
    objs[5+i] = (f"<</Type/Annot/Subtype/Widget/Parent 8 0 R/AS/Off"
                 f"/Rect[{x} 100 {x+14} 114]/AP<</N<</{o} 9 0 R/Off 10 0 R>>>>/P 4 0 R>>").encode()
o = bytearray(b"%PDF-1.7\n"); off = {}
for n in sorted(objs):
    off[n] = len(o); o += b"%d 0 obj\n" % n + objs[n] + b"\nendobj\n"
x = len(o)
o += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs)+1)
o += b"".join(b"%010d 00000 n \n" % off[n] for n in sorted(objs))
o += b"trailer\n<</Size %d/Root 1 0 R>>\nstartxref\n%d\n%%%%EOF\n" % (len(objs)+1, x)
open(sys.argv[1], "wb").write(o)
PY
$BIN fields "$TMP/radio.pdf" 2>/dev/null | python3 -c '
import json, sys
f = json.load(sys.stdin)["fields"][0]
assert f["type"] == "button" and f["label"] == "Preferred contact"
assert f["options"] == ["Email", "Phone", "Mail"], f
' && pass "radio: discovery lists options" || fail "radio options missing"
printf "%%FDF-1.2\n1 0 obj\n<< /FDF << /Fields [ << /T (ContactMethod) /V (Phone) >> ] >> >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%%%EOF\n" > "$TMP/radio.fdf"
$BIN fill -o "$TMP/radio-out.pdf" "$TMP/radio.fdf" "$TMP/radio.pdf" 2>/dev/null
python3 - "$TMP/radio-out.pdf" <<'PY' && pass "radio: fill sets parent /V and matching kid /AS" || fail "radio fill /AS wrong"
import sys, re
d = open(sys.argv[1], "rb").read()
tail = d[d.rfind(b"%PDF"):]  # whole file
# parent obj 8 -> /V/Phone
assert re.search(rb"8 0 obj\b.*?/V\s*/Phone", tail, re.S), "parent /V not /Phone"
# the Phone kid (obj 6) -> /AS/Phone ; an Email/Mail kid -> /AS/Off
assert re.search(rb"6 0 obj\b.*?/AS\s*/Phone", tail, re.S), "phone kid /AS not set"
assert re.search(rb"5 0 obj\b.*?/AS\s*/Off", tail, re.S), "email kid not /Off"
PY

echo "== fill --json / --strict (machine-readable outcome) =="
# An FDF with one good field and one typo -> partial fill.
printf '%%FDF-1.2\n1 0 obj\n<< /FDF << /Fields [ << /T (%s) /V (X) >> << /T (NoSuchField) /V (y) >> ] >> >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%%%EOF\n' "$NAME" > "$TMP/partial.fdf"
# --json: result to stdout (PDF to -o), stderr diagnostics ignored.
$BIN fill --json -o "$TMP/pj.pdf" "$TMP/partial.fdf" "$PDF" 2>/dev/null > "$TMP/result.json"
python3 - "$TMP/result.json" <<'PY' && pass "fill --json: machine-readable result" || fail "fill --json wrong"
import json, sys
d = json.load(open(sys.argv[1]))
assert d["updated_count"] == 1 and d["not_found_count"] == 1
assert d["not_found"] == ["NoSuchField"]
assert len(d["updated"]) == 1
PY
# --json without -o must error (stdout carries the JSON, not the PDF).
$BIN fill --json "$TMP/partial.fdf" "$PDF" >/dev/null 2>&1
[ "$?" -eq 1 ] && pass "fill --json without -o errors" || fail "--json/-o guard missing"
# --strict: any unmatched field -> exit 3; all matched -> exit 0.
$BIN fill --strict -o "$TMP/ps.pdf" "$TMP/partial.fdf" "$PDF" >/dev/null 2>&1
[ "$?" -eq 3 ] && pass "fill --strict: exit 3 on unmatched field" || fail "--strict exit code wrong"
$BIN fill --strict -o "$TMP/ps.pdf" "$TMP/in.fdf" "$PDF" >/dev/null 2>&1
[ "$?" -eq 0 ] && pass "fill --strict: exit 0 when all match" || fail "--strict false positive"
# Zero fields matched is a failure by default (exit 2), no flag needed, and no
# output is written.
printf '%%FDF-1.2\n1 0 obj\n<< /FDF << /Fields [ << /T (NoSuchField) /V (x) >> ] >> >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%%%EOF\n' > "$TMP/none.fdf"
rm -f "$TMP/none.pdf"
$BIN fill -o "$TMP/none.pdf" "$PDF" "$TMP/none.fdf" >/dev/null 2>&1
[ "$?" -eq 2 ] && [ ! -e "$TMP/none.pdf" ] && pass "fill: zero matches exits 2, no output" || fail "zero-match exit/output wrong"
# Flatten is exempt: it removes the form even with no new values.
$BIN fill --flatten -o "$TMP/flatz.pdf" "$PDF" "$TMP/none.fdf" >/dev/null 2>&1
[ "$?" -eq 0 ] && [ -s "$TMP/flatz.pdf" ] && pass "fill --flatten: writes even with zero matches" || fail "flatten zero-match wrong"

echo "== CLI conventions (--, -o, stdin FDF) =="
# '--' ends option parsing on every command (POSIX guideline 10).
n=$($BIN fdf-extract -- "$PDF" 2>/dev/null | grep -c '^/T (')
[ "$n" -gt 0 ] && pass "--: fdf-extract accepts it" || fail "--: fdf-extract broken"
$BIN fill -- "$TMP/in.fdf" "$PDF" 2>/dev/null | head -c 5 | grep -q '%PDF' \
    && pass "--: fill accepts it" || fail "--: fill broken"
# Unknown options must error, not be mistaken for file names.
$BIN fill -z a b >/dev/null 2>"$TMP/opt_err"
[ "$?" -eq 1 ] && grep -q "unknown option" "$TMP/opt_err" \
    && pass "unknown option -> stderr + exit 1" || fail "unknown-option handling"
# -f is the short form of --flatten.
$BIN fill -f "$TMP/in.fdf" "$PDF" 2>"$TMP/f_err" >/dev/null
grep -q "Flattened" "$TMP/f_err" && pass "-f: short form of --flatten" || fail "-f alias broken"
# '-' reads the FDF from stdin (spooled to a temp file internally).
$BIN fill - "$PDF" < "$TMP/in.fdf" 2>/dev/null > "$TMP/stdin_fill.pdf"
grep -qa 'SMOKE_TEST_VALUE' "$TMP/stdin_fill.pdf" \
    && pass "fill -: FDF from stdin" || fail "stdin FDF broken"
# -o is atomic: the file appears on success and never on failure (no .tmp
# residue either way).
$BIN fill -o "$TMP/ofile.pdf" "$TMP/in.fdf" "$PDF" 2>/dev/null
[ -s "$TMP/ofile.pdf" ] && [ ! -e "$TMP/ofile.pdf.tmp" ] \
    && pass "-o: writes the file, no .tmp left" || fail "-o output wrong"
$BIN fill -o "$TMP/never.pdf" "$TMP/in.fdf" "$TMP/does-not-exist.pdf" 2>/dev/null
[ "$?" -ne 0 ] && [ ! -e "$TMP/never.pdf" ] && [ ! -e "$TMP/never.pdf.tmp" ] \
    && pass "-o: failed fill leaves no partial output" || fail "-o atomicity broken"

echo "== fill: /MaxLen truncation + warning + JSON report =="
python3 - "$TMP/ml.pdf" <<'PY'
import sys
objs = {
 1: b"<</Type/Catalog/Pages 2 0 R/AcroForm 4 0 R>>",
 2: b"<</Type/Pages/Kids[3 0 R]/Count 1>>",
 3: b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 300 200]/Annots[5 0 R]>>",
 4: b"<</Fields[5 0 R]/DA(/Helv 12 Tf 0 g)/DR<</Font<</Helv 6 0 R>>>>/NeedAppearances true>>",
 5: b"<</FT/Tx/T(code)/MaxLen 5/V()/Type/Annot/Subtype/Widget/Rect[10 100 200 130]/DA(/Helv 12 Tf 0 g)/P 3 0 R>>",
 6: b"<</Type/Font/Subtype/Type1/BaseFont/Helvetica/Name/Helv>>",
}
o = bytearray(b"%PDF-1.7\n"); off = {}
for n in sorted(objs):
    off[n] = len(o); o += b"%d 0 obj\n" % n + objs[n] + b"\nendobj\n"
x = len(o)
o += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs)+1)
o += b"".join(b"%010d 00000 n \n" % off[n] for n in sorted(objs))
o += b"trailer\n<</Size %d/Root 1 0 R>>\nstartxref\n%d\n%%%%EOF\n" % (len(objs)+1, x)
open(sys.argv[1], "wb").write(o)
PY
# over-length value: truncated to 5, warning on stderr, listed in --json.
printf '%%FDF-1.2\n1 0 obj\n<< /FDF << /Fields [ << /T (code) /V (ABCDEFGHIJKL) >> ] >> >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%%%EOF\n' > "$TMP/ml.fdf"
$BIN fill --json -o "$TMP/mlo.pdf" "$TMP/ml.pdf" "$TMP/ml.fdf" 2>"$TMP/mlerr" > "$TMP/mlres.json"
grep -q "exceeds MaxLen 5; truncated" "$TMP/mlerr" && pass "MaxLen: warns on over-length value" || fail "no MaxLen warning"
python3 - "$TMP/mlres.json" "$TMP/mlo.pdf" <<'PY' && pass "MaxLen: value truncated + reported in JSON" || fail "MaxLen truncation wrong"
import json, sys, re
d = json.load(open(sys.argv[1]))
assert d["truncated"] == ["code"], d
v = re.search(rb"/V\s*\(([^)]*)\)", open(sys.argv[2], "rb").read()[::-1].replace(b")", b")"))  # crude
data = open(sys.argv[2], "rb").read()
m = re.findall(rb"/V\(([A-Z]+)\)", data)
assert m and m[-1] == b"ABCDE", m   # cut to 5 chars
PY

echo "== fill: JSON values + either-order / auto-detected input =="
# The values file may be an FDF or a JSON object, in either order relative to
# the PDF, auto-detected by content. Same JSON shape `fields` produces.
cat > "$TMP/values.json" <<'JSON'
{ "topmostSubform[0].Page1[0].Pg1Header[0].f1_1[0]": "JSON_VALUE",
  "topmostSubform[0].Page1[0].c1_1[0]": true }
JSON
# New order (pdf first), JSON values, with --json result.
$BIN fill --json -o "$TMP/vj.pdf" "$PDF" "$TMP/values.json" 2>/dev/null > "$TMP/vjres.json"
python3 - "$TMP/vjres.json" <<'PY' && pass "fill: JSON values (pdf-first) + result" || fail "JSON values fill wrong"
import json, sys
d = json.load(open(sys.argv[1]))
assert d["updated_count"] == 2 and d["not_found_count"] == 0, d
PY
grep -qa 'JSON_VALUE' "$TMP/vj.pdf" && pass "fill: JSON value present in output" || fail "JSON value missing"
# Old order (values first) must still work (back-compat, keeps demos valid).
$BIN fill -o "$TMP/vo.pdf" "$TMP/values.json" "$PDF" 2>/dev/null
grep -qa 'JSON_VALUE' "$TMP/vo.pdf" && pass "fill: values-first order still accepted" || fail "old order broken"
# JSON from stdin ("-").
echo '{"topmostSubform[0].Page1[0].Pg1Header[0].f1_1[0]": "PIPED"}' \
    | $BIN fill -o "$TMP/vp.pdf" "$PDF" - 2>/dev/null
grep -qa 'PIPED' "$TMP/vp.pdf" && pass "fill: JSON from stdin (-)" || fail "stdin JSON broken"
# Malformed JSON must be rejected (exit 1).
echo '{ bad json' | $BIN fill -o "$TMP/vb.pdf" "$PDF" - >/dev/null 2>&1
[ "$?" -eq 1 ] && pass "fill: malformed JSON rejected" || fail "bad JSON accepted"
# Two PDFs (or two non-PDFs) -> a clear error, not a wrong guess.
$BIN fill -o "$TMP/x.pdf" "$PDF" "$PDF" >/dev/null 2>&1
[ "$?" -ne 0 ] && pass "fill: ambiguous args rejected" || fail "ambiguous args accepted"

echo "== XFA datasets XML escaping =="

echo "== XFA datasets XML escaping =="
# A value with XML metacharacters (& < >) must be escaped when spliced into the
# XFA datasets XML, or the datasets packet becomes malformed. Fill f8821 (a
# dynamic-XFA form) and confirm the appended datasets stream still parses.
printf '%%FDF-1.2\n1 0 obj\n<< /FDF << /Fields [ << /T (%s) /V (a & b <x> c) >> ] >> >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%%%EOF\n' "$NAME" > "$TMP/xfa.fdf"
$BIN fill "$TMP/xfa.fdf" "$PDF" > "$TMP/xfa.pdf" 2>/dev/null
python3 - "$TMP/xfa.pdf" <<'PY' && pass "XFA value escaped; datasets XML well-formed" || fail "XFA datasets XML malformed"
import sys, re
import xml.dom.minidom as M
d = open(sys.argv[1], 'rb').read()
# the appended datasets is an uncompressed stream; grab the <xfa:datasets> packet
# (its tags carry whitespace before '>', so allow it in the closing tag).
m = re.search(rb'(<xfa:datasets\b.*?</xfa:datasets\s*>)', d, re.S)
assert m, "no datasets packet found"
xml = m.group(1).decode('utf-8')
M.parseString(xml)                                   # raises if not well-formed
assert '&amp;' in xml and '&lt;x&gt;' in xml, "metacharacters not escaped"
PY

echo "== indirect /Fields array =="
# A valid AcroForm whose /Fields is an indirect reference (/Fields 5 0 R) rather
# than an inline array. Both fdf-extract and fill must still see 'myfield'.
python3 - "$TMP/indirect.pdf" <<'PY'
import sys
objs = {1: b"<</Type/Catalog/Pages 2 0 R/AcroForm 4 0 R>>",
        2: b"<</Type/Pages/Kids[3 0 R]/Count 1>>",
        3: b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Annots[6 0 R]>>",
        4: b"<</Fields 5 0 R/DA(/Helv 0 Tf 0 g)>>",
        5: b"[6 0 R]",
        6: b"<</FT/Tx/T(myfield)/Type/Annot/Subtype/Widget/Rect[100 700 300 720]/P 3 0 R>>"}
out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"); offs = {}
for n in range(1, 7):
    offs[n] = len(out); out += f"{n} 0 obj\n".encode() + objs[n] + b"\nendobj\n"
xo = len(out); out += b"xref\n0 7\n0000000000 65535 f \n"
for n in range(1, 7): out += f"{offs[n]:010d} 00000 n \n".encode()
out += b"trailer\n<</Size 7/Root 1 0 R>>\nstartxref\n" + str(xo).encode() + b"\n%%EOF\n"
open(sys.argv[1], 'wb').write(out)
PY
$BIN fdf-extract "$TMP/indirect.pdf" 2>/dev/null | grep -q 'myfield' \
    && pass "indirect: fdf-extract lists the field" || fail "indirect: fdf-extract missed the field"
printf '%%FDF-1.2\n1 0 obj\n<< /FDF << /Fields [ << /T (myfield) /V (INDIRECT_OK) >> ] >> >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%%%EOF\n' > "$TMP/ind.fdf"
$BIN fill "$TMP/ind.fdf" "$TMP/indirect.pdf" 2>/dev/null | grep -qa 'INDIRECT_OK' \
    && pass "indirect: fill fills the field" || fail "indirect: fill failed"

echo "== non-ASCII field value =="
# A UTF-8 value with accents, CJK and an astral-plane emoji must be written as a
# UTF-16BE hex string (<FEFF...>) so viewers render it, not as raw literal bytes
# (which readers decode as PDFDocEncoding -> mojibake). Reuses the indirect.pdf
# plain text field so no XFA layer is involved.
python3 - "$TMP/indirect.pdf" "$TMP/uni.fdf" <<'PY'
import sys
val = "café señor 日本 \U0001f600"
fdf = ("%FDF-1.2\n1 0 obj\n<< /FDF << /Fields [ << /T (myfield) /V ("
       + val + ") >> ] >> >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%EOF\n")
open(sys.argv[2], "wb").write(fdf.encode("utf-8"))
PY
$BIN fill "$TMP/uni.fdf" "$TMP/indirect.pdf" 2>/dev/null > "$TMP/uni.pdf"
python3 - "$TMP/uni.pdf" <<'PY' && pass "non-ASCII value round-trips as UTF-16BE" || fail "non-ASCII value mangled"
import sys, re
d = open(sys.argv[1], 'rb').read()
m = [x for x in re.finditer(rb'myfield.*?/V\s*<([0-9A-Fa-f]+)>', d, re.S)]
assert m, "no /V hex string found (value not UTF-16BE encoded)"
b = bytes.fromhex(m[-1].group(1).decode())
assert b[:2] == b'\xfe\xff', "missing UTF-16BE BOM"
assert b[2:].decode('utf-16-be') == "café señor 日本 \U0001f600", "value mismatch"
PY

echo "== emoji field value (astral-plane, UTF-16 surrogate pairs) =="
# Emoji live outside the BMP (e.g. U+1F600), so they must be written as UTF-16BE
# surrogate pairs and decoded back exactly. Exercises both fill (encode) and
# fields (decode), via the JSON input path with raw UTF-8 emoji bytes.
python3 - "$TMP/emoji.json" <<'PY'
import sys
open(sys.argv[1], "w", encoding="utf-8").write('{"myfield": "go \U0001f600 \U0001f44d"}')
PY
$BIN fill -o "$TMP/emoji.pdf" "$TMP/indirect.pdf" "$TMP/emoji.json" 2>/dev/null
python3 - "$TMP/emoji.pdf" <<'PY' && pass "emoji: written as UTF-16BE surrogate pairs" || fail "emoji encoding wrong"
import sys, re
d = open(sys.argv[1], 'rb').read()
m = [x for x in re.finditer(rb'myfield.*?/V\s*<([0-9A-Fa-f]+)>', d, re.S)]
assert m, "no /V hex string (emoji not UTF-16BE encoded)"
b = bytes.fromhex(m[-1].group(1).decode())
assert b[:2] == b'\xfe\xff', "missing UTF-16BE BOM"
assert b[2:].decode('utf-16-be') == "go \U0001f600 \U0001f44d", "emoji value mismatch"
u = b[2:]  # a lead surrogate (0xD800-0xDBFF) must be present
assert any(0xD8 <= u[i] <= 0xDB for i in range(0, len(u) - 1, 2)), "no surrogate pair"
PY
$BIN fields "$TMP/emoji.pdf" 2>/dev/null | python3 -c '
import json, sys
v = [x["value"] for x in json.load(sys.stdin)["fields"] if x["name"] == "myfield"][0]
assert v == "go \U0001f600 \U0001f44d", repr(v)
' && pass "emoji: round-trips through fields (decode)" || fail "emoji decode wrong"

echo "== hybrid-reference file (/XRefStm) =="
# A classic xref table (for legacy readers) plus a cross-reference stream at
# /XRefStm holding the one field, which is compressed inside object stream 5.
# Only a reader that follows /XRefStm can see 'hybridfield'.
python3 - "$TMP/hybrid.pdf" <<'PY'
import sys, zlib
field6 = b"<</FT/Tx/T(hybridfield)/Type/Annot/Subtype/Widget/Rect[100 700 300 720]/P 3 0 R>>"
index = b"6 0 "; first = len(index)
comp = zlib.compress(index + field6)
obj5 = (b"<</Type/ObjStm/N 1/First %d/Length %d/Filter/FlateDecode>>\nstream\n" % (first, len(comp))
        + comp + b"\nendstream")
objs = {1: b"<</Type/Catalog/Pages 2 0 R/AcroForm 4 0 R>>",
        2: b"<</Type/Pages/Kids[3 0 R]/Count 1>>",
        3: b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Annots[6 0 R]>>",
        4: b"<</Fields[6 0 R]/DA(/Helv 0 Tf 0 g)>>"}
xcomp = zlib.compress(bytes([2, 5, 0]))          # obj 6 = type 2, objstm 5, index 0
obj7 = (b"<</Type/XRef/Size 8/Root 1 0 R/W[1 1 1]/Index[6 1]/Length %d/Filter/FlateDecode>>\nstream\n" % len(xcomp)
        + xcomp + b"\nendstream")
out = bytearray(b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n"); off = {}
for n in (1, 2, 3, 4):
    off[n] = len(out); out += (b"%d 0 obj\n" % n) + objs[n] + b"\nendobj\n"
off[5] = len(out); out += b"5 0 obj\n" + obj5 + b"\nendobj\n"
off[7] = len(out); out += b"7 0 obj\n" + obj7 + b"\nendobj\n"
xoff = len(out)
out += b"xref\n0 6\n0000000000 65535 f \n"
for n in (1, 2, 3, 4, 5): out += b"%010d 00000 n \n" % off[n]
out += b"7 1\n%010d 00000 n \n" % off[7]
out += b"trailer\n<</Size 8/Root 1 0 R/XRefStm %d>>\nstartxref\n%d\n%%%%EOF\n" % (off[7], xoff)
open(sys.argv[1], 'wb').write(out)
PY
$BIN fdf-extract "$TMP/hybrid.pdf" 2>/dev/null | grep -q 'hybridfield' \
    && pass "hybrid: fdf-extract lists the /XRefStm field" || fail "hybrid: fdf-extract missed the field"
printf '%%FDF-1.2\n1 0 obj\n<< /FDF << /Fields [ << /T (hybridfield) /V (HYBRID_OK) >> ] >> >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%%%EOF\n' > "$TMP/hyb.fdf"
$BIN fill "$TMP/hyb.fdf" "$TMP/hybrid.pdf" 2>/dev/null | grep -qa 'HYBRID_OK' \
    && pass "hybrid: fill fills the field" || fail "hybrid: fill failed"

echo "== single-stream XFA (/XFA N 0 R) =="
# /XFA can be a single stream reference to the whole XDP packet (not the
# [(datasets) N 0 R ...] array form). fill must locate the datasets inside that
# XDP, edit it, and rewrite the (Flate-compressed) object with its dict
# preserved -- stripped of /Filter so the now-uncompressed stream stays valid.
python3 - "$TMP/sxfa.pdf" <<'PY'
import sys, zlib
xdp = (b'<xdp:xdp xmlns:xdp="http://ns.adobe.com/xdp/">'
       b'<xfa:datasets xmlns:xfa="http://www.xfa.org/schema/xfa-data/1.0/">'
       b'<xfa:data><singlexfa/></xfa:data></xfa:datasets></xdp:xdp>')
comp = zlib.compress(xdp)                             # obj 5 has no /Type, only /Filter
obj5 = b"<</Filter/FlateDecode/Length %d>>\nstream\n" % len(comp) + comp + b"\nendstream"
objs = {1: b"<</Type/Catalog/Pages 2 0 R/AcroForm 4 0 R>>",
        2: b"<</Type/Pages/Kids[3 0 R]/Count 1>>",
        3: b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Annots[6 0 R]>>",
        4: b"<</Fields[6 0 R]/DA(/Helv 0 Tf 0 g)/XFA 5 0 R>>",
        6: b"<</FT/Tx/T(singlexfa)/Type/Annot/Subtype/Widget/Rect[100 700 300 720]/P 3 0 R>>"}
out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"); off = {}
for n in (1, 2, 3, 4): off[n] = len(out); out += (b"%d 0 obj\n" % n) + objs[n] + b"\nendobj\n"
off[5] = len(out); out += b"5 0 obj\n" + obj5 + b"\nendobj\n"
off[6] = len(out); out += b"6 0 obj\n" + objs[6] + b"\nendobj\n"
xo = len(out); out += b"xref\n0 7\n0000000000 65535 f \n"
for n in range(1, 7): out += b"%010d 00000 n \n" % off[n]
out += b"trailer\n<</Size 7/Root 1 0 R>>\nstartxref\n%d\n%%%%EOF\n" % xo
open(sys.argv[1], 'wb').write(out)
PY
printf '%%FDF-1.2\n1 0 obj\n<< /FDF << /Fields [ << /T (singlexfa) /V (SINGLE_XFA_OK) >> ] >> >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%%%EOF\n' > "$TMP/sxfa.fdf"
$BIN fill "$TMP/sxfa.fdf" "$TMP/sxfa.pdf" 2>/dev/null > "$TMP/sxfa.out.pdf"
python3 - "$TMP/sxfa.out.pdf" <<'PY' && pass "single-stream XFA: value synced into datasets, dict preserved" || fail "single-stream XFA sync"
import sys, re
d = open(sys.argv[1], 'rb').read()
# incremental update keeps the original compressed obj 5 and appends the
# rewritten one -- take the LAST "5 0 obj" (the datasets we just wrote).
ms = re.findall(rb'\b5 0 obj\b(.*?)\bendobj\b', d, re.S)
assert ms, "no rewritten obj 5"
body = ms[-1]
assert b'<singlexfa>SINGLE_XFA_OK</singlexfa>' in body, "value not spliced into XDP datasets"
assert b'/Filter' not in body, "stale /Filter left on now-uncompressed stream"
PY

echo "== dynamic-XFA checkbox sync (datasets 0/1) =="
# Dynamic-XFA forms render checkboxes from the datasets packet, encoded as the
# selected value ("1" on / "0" off), not just the AcroForm /AS. Build a form with
# a checkbox whose on-state is "1", fill it on then off, and confirm both the XFA
# datasets element AND the AcroForm appearance state move together.
build_cbxfa() {   # $1=out pdf
python3 - "$1" <<'PY'
import sys, zlib
xdp = (b'<xdp:xdp xmlns:xdp="http://ns.adobe.com/xdp/">'
       b'<xfa:datasets xmlns:xfa="http://www.xfa.org/schema/xfa-data/1.0/">'
       b'<xfa:data><cb1>0</cb1></xfa:data></xfa:datasets></xdp:xdp>')
comp = zlib.compress(xdp)
obj5 = b"<</Filter/FlateDecode/Length %d>>\nstream\n" % len(comp) + comp + b"\nendstream"
# checkbox widget: on-state name is "1" (the non-/Off key of /AP /N)
cb = (b"<</FT/Btn/T(cb1)/Type/Annot/Subtype/Widget/Rect[100 700 120 720]"
      b"/P 3 0 R/AS/Off/AP<</N<</1 90 0 R/Off 91 0 R>>>>>>")
objs = {1: b"<</Type/Catalog/Pages 2 0 R/AcroForm 4 0 R>>",
        2: b"<</Type/Pages/Kids[3 0 R]/Count 1>>",
        3: b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Annots[6 0 R]>>",
        4: b"<</Fields[6 0 R]/DA(/Helv 0 Tf 0 g)/XFA 5 0 R>>",
        6: cb}
out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"); off = {}
for n in (1, 2, 3, 4): off[n] = len(out); out += (b"%d 0 obj\n" % n) + objs[n] + b"\nendobj\n"
off[5] = len(out); out += b"5 0 obj\n" + obj5 + b"\nendobj\n"
off[6] = len(out); out += b"6 0 obj\n" + objs[6] + b"\nendobj\n"
xo = len(out); out += b"xref\n0 7\n0000000000 65535 f \n"
for n in range(1, 7): out += b"%010d 00000 n \n" % off[n]
out += b"trailer\n<</Size 7/Root 1 0 R>>\nstartxref\n%d\n%%%%EOF\n" % xo
open(sys.argv[1], 'wb').write(out)
PY
}
build_cbxfa "$TMP/cbxfa.pdf"
# check ON: value "1" (or "Yes"/"On") -> datasets <cb1>1</cb1>, /AS /1
printf '%%FDF-1.2\n1 0 obj\n<< /FDF << /Fields [ << /T (cb1) /V (1) >> ] >> >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%%%EOF\n' > "$TMP/cbon.fdf"
$BIN fill "$TMP/cbon.fdf" "$TMP/cbxfa.pdf" 2>/dev/null > "$TMP/cbon.pdf"
python3 - "$TMP/cbon.pdf" <<'PY' && pass "checkbox ON: datasets <cb1>1</cb1> + AcroForm /AS /1" || fail "checkbox ON sync"
import sys, re
d = open(sys.argv[1], 'rb').read()
ds = re.findall(rb'\b5 0 obj\b(.*?)\bendobj\b', d, re.S)[-1]
assert b'<cb1>1</cb1>' in ds, "datasets checkbox not set to 1"
fld = re.findall(rb'\b6 0 obj\b(.*?)\bendobj\b', d, re.S)[-1]
assert b'/AS/1' in fld or b'/AS /1' in fld, "AcroForm on-state /AS not set to /1"
PY
# check OFF: value "0" (off-like) -> datasets <cb1>0</cb1>
printf '%%FDF-1.2\n1 0 obj\n<< /FDF << /Fields [ << /T (cb1) /V (0) >> ] >> >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%%%EOF\n' > "$TMP/cboff.fdf"
$BIN fill "$TMP/cboff.fdf" "$TMP/cbxfa.pdf" 2>/dev/null > "$TMP/cboff.pdf"
python3 - "$TMP/cboff.pdf" <<'PY' && pass "checkbox OFF: datasets <cb1>0</cb1>" || fail "checkbox OFF sync"
import sys, re
d = open(sys.argv[1], 'rb').read()
ds = re.findall(rb'\b5 0 obj\b(.*?)\bendobj\b', d, re.S)[-1]
assert b'<cb1>0</cb1>' in ds, "datasets checkbox not cleared to 0"
PY

echo "== malformed startxref must not hang (fuzz regression) =="
# "startxref" with no numeric offset before EOF once made find_startxref loop
# forever (it mutated its own scan index and re-matched the keyword). The tool
# must fail fast, not spin.
printf '%%PDF-1.7\njunk\nstartxref   \n' > "$TMP/nosx.pdf"
timeout 10 $BIN fdf-extract "$TMP/nosx.pdf" >/dev/null 2>&1
[ "$?" -ne 124 ] && pass "startxref-without-offset exits instead of looping" || fail "hung on malformed startxref"

echo "== fill --flatten: bake values in, remove the form =="
if python3 -c 'import pikepdf' 2>/dev/null; then
    $BIN fill --flatten "$TMP/in.fdf" "$PDF" > "$TMP/flat.pdf" 2>/dev/null
    python3 - "$TMP/flat.pdf" <<'PY' && pass "flatten: valid, no AcroForm, no widgets, value baked into page" || fail "flatten output wrong"
import sys, pikepdf
p = pikepdf.open(sys.argv[1])                      # raises if structurally invalid
assert "/AcroForm" not in p.Root, "AcroForm still present"
assert sum(len(pg.obj.get("/Annots", [])) for pg in p.pages) == 0, "widget annots remain"
# the value must live in page content now (an XObject was added to draw it)
assert any("/XObject" in pg.obj.get("/Resources", {}) for pg in p.pages), "no stamped XObject"
PY
else
    echo "  skip:  pikepdf not installed (flatten output not verified)"
fi

echo "== security regressions (malicious inputs must not crash) =="
# These reproduce memory-safety bugs found in the security audit. Each must be
# handled gracefully (no SIGSEGV=139 / SIGABRT=134), not crash.
crashcheck() { # $1=label $2=file
    timeout 10 $BIN fdf-extract "$2" >/dev/null 2>&1; local rc=$?
    if [ "$rc" -eq 139 ] || [ "$rc" -eq 134 ] || [ "$rc" -eq 124 ]; then
        fail "$1 (crash/hang, rc=$rc)"
    else
        pass "$1 (rc=$rc)"
    fi
}
python3 - "$TMP" <<'PY'
import sys, zlib
T = sys.argv[1]
# (H1) crafted /W widths -> OOB read in be_read
data = zlib.compress(b"\x00" * 24)
o = bytearray(b"%PDF-1.5\n"); off = len(o)
d = b"<</Type/XRef/Size 3/Root 1 0 R/W[1000000000 -499999999]/Filter/FlateDecode/Length %d>>" % len(data)
o += b"1 0 obj\n"+d+b"\nstream\n"+data+b"\nendstream\nendobj\nstartxref\n%d\n%%%%EOF\n" % off
open(T+"/poc_w.pdf","wb").write(o)

# (C2) ObjStm with decreasing offsets -> objend<start underflow -> memcpy(SIZE_MAX)
idxtab=b"3 1 99 0 "; body=b"<</Type/Catalog>>x"; First=len(idxtab)
c2=zlib.compress(idxtab+body)
o=bytearray(b"%PDF-1.5\n"); o2=len(o)
o+=b"2 0 obj\n<</Type/ObjStm/N 2/First %d/Filter/FlateDecode/Length %d>>\nstream\n"%(First,len(c2))+c2+b"\nendstream\nendobj\n"
o1=len(o)
def rec(t,a,b): return bytes([t])+a.to_bytes(2,'big')+bytes([b])
xc=zlib.compress(rec(1,o1,0)+rec(1,o2,0)+rec(2,2,0))
o+=b"1 0 obj\n<</Type/XRef/Size 100/Root 3 0 R/W[1 2 1]/Index[1 3]/Filter/FlateDecode/Length %d>>\nstream\n"%len(xc)+xc+b"\nendstream\nendobj\nstartxref\n%d\n%%%%EOF\n"%o1
open(T+"/poc_objstm.pdf","wb").write(o)

# (H2) /Encrypt with huge /Length -> md5(h, key_len, h) over-reads h[16]
O=b"("+b"O"*48+b")"; U=b"("+b"U"*48+b")"
enc=b"<</Filter/Standard/V 2/R 3/Length 99999999/O "+O+b"/U "+U+b"/P -1>>"
objs={1:b"<</Type/Catalog/Pages 2 0 R>>",2:b"<</Type/Pages/Kids[]/Count 0>>",3:enc}
o=bytearray(b"%PDF-1.5\n"); off={}
for n in (1,2,3): off[n]=len(o); o+=b"%d 0 obj\n"%n+objs[n]+b"\nendobj\n"
xo=len(o); o+=b"xref\n0 4\n0000000000 65535 f \n"
for n in (1,2,3): o+=b"%010d 00000 n \n"%off[n]
o+=b"trailer\n<</Size 4/Root 1 0 R/Encrypt 3 0 R/ID[<0123456789abcdef0123456789abcdef><0123456789abcdef0123456789abcdef>]>>\nstartxref\n%d\n%%%%EOF\n"%xo
open(T+"/poc_length.pdf","wb").write(o)
PY
crashcheck "crafted /W widths (OOB read)"        "$TMP/poc_w.pdf"
crashcheck "ObjStm decreasing offsets (heap OF)" "$TMP/poc_objstm.pdf"
crashcheck "encrypt huge /Length (stack OOB)"    "$TMP/poc_length.pdf"

echo "== raw (no-/Filter) streams obey the decompression caps =="
# The unfiltered-stream passthrough must honor PDF_MAX_DECOMPRESSED like the
# real decoders (CodeQL: uncontrolled allocation size). ffpdf's own filled
# output carries a raw xref stream larger than a 10-byte cap, so parsing it
# must fail under that cap and succeed without it.
if PDF_MAX_DECOMPRESSED=10 $BIN fdf-extract "$TMP/filled.pdf" >/dev/null 2>&1; then
    fail "raw stream ignored PDF_MAX_DECOMPRESSED"
else
    pass "raw stream respects PDF_MAX_DECOMPRESSED"
fi

echo "== decompression bomb guard =="
# A tiny file whose object stream inflates to ~2 MB. With a decompressed-size
# limit below that, the stream must be rejected cleanly (field unreachable, no
# crash/OOM); with a limit above it, the field is found normally.
python3 - "$TMP/bomb.pdf" <<'PY'
import sys, zlib
field6 = b"<</FT/Tx/T(bombfield)/Type/Annot/Subtype/Widget/Rect[100 700 300 720]/P 3 0 R>>"
index = b"6 0 "; first = len(index)
plain = index + field6 + b" " * (2 * 1024 * 1024)     # padding is ignored by the parser
comp = zlib.compress(plain)
obj5 = (b"<</Type/ObjStm/N 1/First %d/Length %d/Filter/FlateDecode>>\nstream\n" % (first, len(comp))
        + comp + b"\nendstream")
objs = {1: b"<</Type/Catalog/Pages 2 0 R/AcroForm 4 0 R>>",
        2: b"<</Type/Pages/Kids[3 0 R]/Count 1>>",
        3: b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Annots[6 0 R]>>",
        4: b"<</Fields[6 0 R]/DA(/Helv 0 Tf 0 g)>>"}
out = bytearray(b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n"); off = {}
for n in (1, 2, 3, 4): off[n] = len(out); out += (b"%d 0 obj\n" % n) + objs[n] + b"\nendobj\n"
off[5] = len(out); out += b"5 0 obj\n" + obj5 + b"\nendobj\n"; off[7] = len(out)
def rec(t, a, b): return bytes([t]) + a.to_bytes(2, 'big') + bytes([b])
data = rec(0, 0, 255) + b"".join(rec(1, off[n], 0) for n in (1, 2, 3, 4, 5)) + rec(2, 5, 0) + rec(1, off[7], 0)
xc = zlib.compress(data)
obj7 = (b"<</Type/XRef/Size 8/Root 1 0 R/W[1 2 1]/Index[0 8]/Length %d/Filter/FlateDecode>>\nstream\n" % len(xc)
        + xc + b"\nendstream")
out += b"7 0 obj\n" + obj7 + b"\nendobj\n" + b"startxref\n%d\n%%%%EOF\n" % off[7]
open(sys.argv[1], 'wb').write(out)
PY
# limit below the payload: reject cleanly and promptly (not a hang: rc != 124).
PDF_MAX_DECOMPRESSED=524288 timeout 20 $BIN fdf-extract "$TMP/bomb.pdf" > "$TMP/bomb.out" 2>/dev/null
rc=$?
if [ "$rc" -ne 124 ] && ! grep -q 'bombfield' "$TMP/bomb.out"; then
    pass "bomb: stream over the limit is rejected without hanging"
else
    fail "bomb: hung (rc=$rc) or field reached past the limit"
fi
# limit above the payload: normal decode still works
PDF_MAX_DECOMPRESSED=8388608 $BIN fdf-extract "$TMP/bomb.pdf" 2>/dev/null | grep -q 'bombfield' \
    && pass "bomb: under-limit stream still decodes" || fail "bomb: under-limit stream wrongly rejected"

echo "== cumulative decompression budget (accumulation across streams) =="
# Three object streams, each inflating to ~1 MB (individually under the per-stream
# cap). With a run budget of ~2.5 MB, the first two load and the third is refused,
# proving the budget bounds the TOTAL across streams, not just each one.
python3 - "$TMP/acc.pdf" <<'PY'
import sys, zlib
def objstm(field):
    idx = b"%d 0 " % field[0]; first = len(idx)
    plain = idx + field[1] + b" " * (1024 * 1024)     # ~1 MB decompressed
    comp = zlib.compress(plain)
    return (b"<</Type/ObjStm/N 1/First %d/Length %d/Filter/FlateDecode>>\nstream\n" % (first, len(comp))
            + comp + b"\nendstream")
fields = {6: b"<</FT/Tx/T(f6)/Type/Annot/Subtype/Widget/Rect[0 0 9 9]/P 3 0 R>>",
          7: b"<</FT/Tx/T(f7)/Type/Annot/Subtype/Widget/Rect[0 0 9 9]/P 3 0 R>>",
          8: b"<</FT/Tx/T(f8)/Type/Annot/Subtype/Widget/Rect[0 0 9 9]/P 3 0 R>>"}
top = {1: b"<</Type/Catalog/Pages 2 0 R/AcroForm 4 0 R>>",
       2: b"<</Type/Pages/Kids[3 0 R]/Count 1>>",
       3: b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Annots[6 0 R 7 0 R 8 0 R]>>",
       4: b"<</Fields[6 0 R 7 0 R 8 0 R]/DA(/Helv 0 Tf 0 g)>>"}
out = bytearray(b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n"); off = {}
for n in (1, 2, 3, 4): off[n] = len(out); out += (b"%d 0 obj\n" % n) + top[n] + b"\nendobj\n"
for n, fld in zip((10, 11, 12), (6, 7, 8)):           # one field per object stream
    off[n] = len(out); out += (b"%d 0 obj\n" % n) + objstm((fld, fields[fld])) + b"\nendobj\n"
off[13] = len(out)
def rec(t, a, b): return bytes([t]) + a.to_bytes(3, 'big') + bytes([b])
rows = {i: rec(0, 0, 0) for i in range(14)}           # unused slots (5, 9) stay free
rows[0] = rec(0, 0, 255)
for n in (1, 2, 3, 4, 10, 11, 12, 13): rows[n] = rec(1, off[n], 0)
rows[6] = rec(2, 10, 0); rows[7] = rec(2, 11, 0); rows[8] = rec(2, 12, 0)   # in object streams
data = b"".join(rows[i] for i in range(14))
xc = zlib.compress(data)
obj13 = (b"<</Type/XRef/Size 14/Root 1 0 R/W[1 3 1]/Index[0 14]/Length %d/Filter/FlateDecode>>\nstream\n" % len(xc)
         + xc + b"\nendstream")
out += b"13 0 obj\n" + obj13 + b"\nendobj\n" + b"startxref\n%d\n%%%%EOF\n" % off[13]
open(sys.argv[1], 'wb').write(out)
PY
n=$(PDF_MAX_TOTAL_DECOMPRESSED=2621440 $BIN fdf-extract "$TMP/acc.pdf" 2>/dev/null | grep -c '^/T (')
[ "$n" = 2 ] && pass "budget stops the 3rd ~1 MB stream (2 of 3 fields)" || fail "cumulative budget wrong ($n/3 fields)"
n=$($BIN fdf-extract "$TMP/acc.pdf" 2>/dev/null | grep -c '^/T (')
[ "$n" = 3 ] && pass "all three load under the default budget" || fail "default budget wrongly rejected ($n/3)"

echo "== oversized field dictionary (> old MAX_DICT) =="
# A choice field whose inline /Opt makes the dict ~8 KB (old code capped dict
# rewrites at 4096 and skipped the field). Fill must preserve the whole dict and
# still resolve the selection.
python3 - "$TMP/big.pdf" <<'PY'
import sys
opts = b"".join(b"(Option_%04d)" % i for i in range(600))   # ~9 KB inline /Opt
field = (b"<</FT/Ch/Ff 131072/T(bigc)/Opt[" + opts +
         b"]/Type/Annot/Subtype/Widget/Rect[100 700 300 720]/P 3 0 R/DA(/Helv 10 Tf 0 g)>>")
objs = {1: b"<</Type/Catalog/Pages 2 0 R/AcroForm 4 0 R>>",
        2: b"<</Type/Pages/Kids[3 0 R]/Count 1>>",
        3: b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Annots[5 0 R]>>",
        4: b"<</Fields[5 0 R]/DA(/Helv 0 Tf 0 g)>>", 5: field}
out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"); off = {}
for n in range(1, 6):
    off[n] = len(out); out += (b"%d 0 obj\n" % n) + objs[n] + b"\nendobj\n"
xo = len(out); out += b"xref\n0 6\n0000000000 65535 f \n"
for n in range(1, 6): out += b"%010d 00000 n \n" % off[n]
out += b"trailer\n<</Size 6/Root 1 0 R>>\nstartxref\n%d\n%%%%EOF\n" % xo
open(sys.argv[1], 'wb').write(out)
PY
printf '%%FDF-1.2\n1 0 obj\n<< /FDF << /Fields [ << /T (bigc) /V (Option_0100) >> ] >> >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%%%EOF\n' > "$TMP/big.fdf"
$BIN fill "$TMP/big.fdf" "$TMP/big.pdf" > "$TMP/big_out.pdf" 2>/dev/null
python3 - "$TMP/big.pdf" "$TMP/big_out.pdf" <<'PY' && pass "oversized dict filled without truncation" || fail "oversized dict truncated"
import sys, re
tail = open(sys.argv[2], 'rb').read()[len(open(sys.argv[1], 'rb').read()):]
m = re.search(rb'5 0 obj\n(<<.*?)\nendobj', tail, re.S)
assert m, "field not re-emitted"
b = m.group(1)
assert re.search(rb'/V\(Option_0100\)', b) and re.search(rb'/I\[100\]', b), "value/index wrong"
assert b'Option_0599' in b, "dict truncated (last option missing)"
PY

echo "== CR-only line endings around a stream =="
# Some real PDFs use carriage-return (\r) line endings, so a stream ends
# "<data>\rendstream" with no \n. An fgets-based endstream scan finds nothing;
# the parser must instead trust the direct /Length. Field must still be found.
python3 - "$TMP/cr.pdf" <<'PY'
import sys, zlib
objs = {1: b"<</Type/Catalog/Pages 2 0 R/AcroForm 4 0 R>>",
        2: b"<</Type/Pages/Kids[3 0 R]/Count 1>>",
        3: b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Annots[5 0 R]>>",
        4: b"<</Fields[5 0 R]/DA(/Helv 0 Tf 0 g)>>",
        5: b"<</FT/Tx/T(crfield)/Type/Annot/Subtype/Widget/Rect[0 0 9 9]/P 3 0 R>>"}
out = bytearray(b"%PDF-1.5\r%\xe2\xe3\xcf\xd3\r"); off = {}
for n in (1, 2, 3, 4, 5):
    off[n] = len(out); out += (b"%d 0 obj\r" % n) + objs[n] + b"\rendobj\r"
off[6] = len(out)
rec = lambda t, a, b: bytes([t]) + a.to_bytes(2, 'big') + bytes([b])
data = rec(0, 0, 255) + b"".join(rec(1, off[n], 0) for n in (1, 2, 3, 4, 5)) + rec(1, off[6], 0)
comp = zlib.compress(data)
# xref stream terminated with a bare CR before endstream (no \n after the data)
obj6 = (b"6 0 obj\r<</Type/XRef/Size 7/Root 1 0 R/W[1 2 1]/Index[0 7]/Length %d/Filter/FlateDecode>>\r\nstream\r\n"
        % len(comp) + comp + b"\rendstream\rendobj\r")
out += obj6 + b"startxref\r%d\r%%%%EOF\r" % off[6]
open(sys.argv[1], 'wb').write(out)
PY
$BIN fdf-extract "$TMP/cr.pdf" 2>/dev/null | grep -q 'crfield' \
    && pass "CR-delimited stream parsed via /Length" || fail "CR line endings broke the parse"

echo "== TIFF Predictor 2 cross-reference stream =="
# A cross-reference stream whose /DecodeParms uses /Predictor 2 (TIFF horizontal
# differencing). Only a decoder that reverses it can resolve the object offsets
# and reach 'tifffield'. The brute-force file scan was retired, so a wrong decode
# means the field is unreachable.
python3 - "$TMP/tiff.pdf" <<'PY'
import sys, zlib
objs = {
 1: b"<</Type/Catalog/Pages 2 0 R/AcroForm 4 0 R>>",
 2: b"<</Type/Pages/Kids[3 0 R]/Count 1>>",
 3: b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Annots[5 0 R]>>",
 4: b"<</Fields[5 0 R]/DA(/Helv 0 Tf 0 g)>>",
 5: b"<</FT/Tx/T(tifffield)/Type/Annot/Subtype/Widget/Rect[100 700 300 720]/P 3 0 R>>",
}
out = bytearray(b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n"); off = {}
for n in (1, 2, 3, 4, 5):
    off[n] = len(out); out += (b"%d 0 obj\n" % n) + objs[n] + b"\nendobj\n"
off[6] = len(out)
rec = lambda t, a, b: bytes([t]) + a.to_bytes(2, 'big') + bytes([b])
rows = [rec(0, 0, 255)] + [rec(1, off[n], 0) for n in (1, 2, 3, 4, 5)] + [rec(1, off[6], 0)]
raw = b"".join(rows); COLS = 4
enc = bytearray(raw)                                  # TIFF predictor 2 encode (colors=1)
for r in range(len(raw) // COLS):
    for i in range(r*COLS + COLS - 1, r*COLS, -1):
        enc[i] = (enc[i] - enc[i-1]) & 0xff
comp = zlib.compress(bytes(enc))
obj6 = (b"<</Type/XRef/Size 7/Root 1 0 R/W[1 2 1]/Index[0 7]"
        b"/DecodeParms<</Predictor 2/Columns 4/Colors 1/BitsPerComponent 8>>"
        b"/Filter/FlateDecode/Length %d>>\nstream\n" % len(comp) + comp + b"\nendstream")
out += b"6 0 obj\n" + obj6 + b"\nendobj\n"
out += b"startxref\n%d\n%%%%EOF\n" % off[6]
open(sys.argv[1], 'wb').write(out)
PY
$BIN fdf-extract "$TMP/tiff.pdf" 2>/dev/null | grep -q 'tifffield' \
    && pass "TIFF pred 2: fdf-extract resolves offsets and lists the field" || fail "TIFF pred 2: fdf-extract missed the field"
printf '%%FDF-1.2\n1 0 obj\n<< /FDF << /Fields [ << /T (tifffield) /V (TIFF_OK) >> ] >> >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%%%EOF\n' > "$TMP/tiff.fdf"
$BIN fill "$TMP/tiff.fdf" "$TMP/tiff.pdf" 2>/dev/null | grep -qa 'TIFF_OK' \
    && pass "TIFF pred 2: fill fills the field" || fail "TIFF pred 2: fill failed"

echo "== choice fields (/Opt matching, /I selection) =="
# A combo box (plain-string /Opt) and a list box ([export display] /Opt) plus a
# signature field. Filling must: set the combo /V + /I and an appearance, set the
# list /V to the EXPORT value + /I (matching by display text), skip the signature,
# and leave the AcroForm well-formed.
python3 - "$TMP/choice.pdf" <<'PY'
import sys
objs = {
 1: b"<</Type/Catalog/Pages 2 0 R/AcroForm 4 0 R>>",
 2: b"<</Type/Pages/Kids[3 0 R]/Count 1>>",
 3: b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Annots[6 0 R 7 0 R 8 0 R]>>",
 4: b"<</Fields[6 0 R 7 0 R 8 0 R]/DA(/Helv 0 Tf 0 g)>>",
 6: b"<</FT/Ch/Ff 131072/T(combo1)/Opt[(Red)(Green)(Blue)]/Type/Annot/Subtype/Widget/Rect[100 700 300 720]/P 3 0 R/DA(/Helv 12 Tf 0 g)>>",
 7: b"<</FT/Ch/T(list1)/Opt[[(a)(Apple)][(b)(Banana)][(c)(Cherry)]]/Type/Annot/Subtype/Widget/Rect[100 650 300 690]/P 3 0 R>>",
 8: b"<</FT/Sig/T(sig1)/Type/Annot/Subtype/Widget/Rect[100 600 300 640]/P 3 0 R>>",
}
out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"); off = {}
for n in (1, 2, 3, 4, 6, 7, 8):
    off[n] = len(out); out += (b"%d 0 obj\n" % n) + objs[n] + b"\nendobj\n"
xo = len(out); out += b"xref\n0 9\n0000000000 65535 f \n"
for n in range(1, 9):
    out += (b"%010d 00000 n \n" % off[n]) if n in off else b"0000000000 00000 f \n"
out += b"trailer\n<</Size 9/Root 1 0 R>>\nstartxref\n%d\n%%%%EOF\n" % xo
open(sys.argv[1], 'wb').write(out)
PY
printf '%%FDF-1.2\n1 0 obj\n<< /FDF << /Fields [ << /T (combo1) /V (Green) >> << /T (list1) /V (Banana) >> << /T (sig1) /V (x) >> ] >> >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%%%EOF\n' > "$TMP/choice.fdf"
$BIN fill "$TMP/choice.fdf" "$TMP/choice.pdf" > "$TMP/choice_out.pdf" 2>"$TMP/choice.err"
python3 - "$TMP/choice_out.pdf" <<'PY' && pass "choice: /V, /I, sig-skip, valid AcroForm" || fail "choice fill wrong"
import sys, re
d = open(sys.argv[1], 'rb').read()
assert b'/NeedAppearances true>>' in d, "AcroForm not well-formed"
tail = d[d.find(b'%%EOF')+5:]
def field(num):
    m = [x for x in re.finditer((r"\b%d 0 obj\n(<<.*?)\nendobj" % num).encode(), tail, re.S)]
    return m[-1].group(1) if m else b""
combo, lst = field(6), field(7)
assert re.search(rb'/V\(Green\)', combo) and re.search(rb'/I\[1\]', combo), "combo /V or /I wrong"
assert b'/AP<</N' in combo, "combo missing appearance"
assert re.search(rb'/V\(b\)', lst) and re.search(rb'/I\[1\]', lst), "list export /V or /I wrong"
assert b'8 0 obj' not in tail, "signature field must not be filled"
PY
echo "== checkbox on-state + multi-select choice =="
# check1's on appearance state is named "1" (not "Yes"), so filling with "Yes"
# must map to /V/1 /AS/1; multi1 is a multi-select list box filled with an array.
python3 - "$TMP/cb.pdf" <<'PY'
import sys
objs = {
 1: b"<</Type/Catalog/Pages 2 0 R/AcroForm 4 0 R>>",
 2: b"<</Type/Pages/Kids[3 0 R]/Count 1>>",
 3: b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Annots[6 0 R 7 0 R]>>",
 4: b"<</Fields[6 0 R 7 0 R]/DA(/Helv 0 Tf 0 g)>>",
 6: b"<</FT/Btn/T(check1)/AP<</N<</1 9 0 R/Off 10 0 R>>>>/AS/Off/V/Off/Type/Annot/Subtype/Widget/Rect[100 700 120 720]/P 3 0 R>>",
 7: b"<</FT/Ch/Ff 2097152/T(multi1)/Opt[(a)(b)(c)(d)]/Type/Annot/Subtype/Widget/Rect[100 600 300 680]/P 3 0 R>>",
 9: b"<</Type/XObject/Subtype/Form/BBox[0 0 20 20]/Length 0>>\nstream\n\nendstream",
 10:b"<</Type/XObject/Subtype/Form/BBox[0 0 20 20]/Length 0>>\nstream\n\nendstream",
}
out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"); off = {}
for n in (1, 2, 3, 4, 6, 7, 9, 10):
    off[n] = len(out); out += (b"%d 0 obj\n" % n) + objs[n] + b"\nendobj\n"
xo = len(out); out += b"xref\n0 11\n0000000000 65535 f \n"
for n in range(1, 11):
    out += (b"%010d 00000 n \n" % off[n]) if n in off else b"0000000000 00000 f \n"
out += b"trailer\n<</Size 11/Root 1 0 R>>\nstartxref\n%d\n%%%%EOF\n" % xo
open(sys.argv[1], 'wb').write(out)
PY
printf '%%FDF-1.2\n1 0 obj\n<< /FDF << /Fields [ << /T (check1) /V (Yes) >> << /T (multi1) /V [(b)(d)] >> ] >> >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%%%EOF\n' > "$TMP/cb.fdf"
$BIN fill "$TMP/cb.fdf" "$TMP/cb.pdf" > "$TMP/cb_out.pdf" 2>/dev/null
python3 - "$TMP/cb.pdf" "$TMP/cb_out.pdf" <<'PY' && pass "checkbox on-state mapped; multi-select /V+/I arrays" || fail "checkbox/multi-select wrong"
import sys, re
tail = open(sys.argv[2], 'rb').read()[len(open(sys.argv[1], 'rb').read()):]
def field(n):
    m = re.search((r'\b%d 0 obj\n(<<.*?)\nendobj' % n).encode(), tail, re.S)
    return m.group(1) if m else b""
cb, mu = field(6), field(7)
assert re.search(rb'/V/1\b', cb) and re.search(rb'/AS/1\b', cb), "checkbox not mapped to on-state 1"
assert re.search(rb'/V\[\(b\)\(d\)\]', mu), "multi-select /V array wrong"
assert re.search(rb'/I\[1 3\]', mu), "multi-select /I array wrong"
# the list box gets a multi-row appearance: every option drawn, selected rows filled
am = re.search(rb'/AP<</N (\d+) 0 R', mu)
assert am, "list box has no /AP appearance"
apn = int(am.group(1))
ap = re.search((r'\b%d 0 obj\n(<<.*?stream\n)(.*?)\nendstream' % apn).encode(), tail, re.S)[2]
assert ap.count(b') Tj') == 4, "list box appearance should draw all 4 options"
assert ap.count(b're\nf') == 2, "list box appearance should highlight 2 selected rows"
PY

echo "== predictor on an object stream (Flate + /Predictor 12) =="
# A field object in an object stream that is Flate-compressed AND carries a PNG
# "Up" predictor in /DecodeParms. Predictors were previously only undone in the
# xref-stream path, so 'predfield' was unreachable -- the object-stream cache saw
# still-predicted bytes. Now decompress_stream reverses the predictor for every
# stream, so extract and fill both resolve it.
python3 - "$TMP/predobjstm.pdf" <<'PY'
import sys, zlib
def png12(raw):                     # PNG "Up" predictor, columns=1 (tag byte 2 per row)
    out = bytearray(); prev = 0
    for b in raw:
        out.append(2); out.append((b - prev) & 0xFF); prev = b
    return bytes(out)
field6 = b"<</FT/Tx/T(predfield)/Type/Annot/Subtype/Widget/Rect[100 700 300 720]/P 3 0 R>>"
index = b"6 0 "; first = len(index)
comp = zlib.compress(png12(index + field6))
obj5 = (b"<</Type/ObjStm/N 1/First %d/Length %d/Filter/FlateDecode"
        b"/DecodeParms<</Predictor 12/Columns 1>>>>\nstream\n" % (first, len(comp))
        + comp + b"\nendstream")
objs = {1: b"<</Type/Catalog/Pages 2 0 R/AcroForm 4 0 R>>",
        2: b"<</Type/Pages/Kids[3 0 R]/Count 1>>",
        3: b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Annots[6 0 R]>>",
        4: b"<</Fields[6 0 R]/DA(/Helv 0 Tf 0 g)>>"}
out = bytearray(b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n"); off = {}
for n in (1, 2, 3, 4):
    off[n] = len(out); out += (b"%d 0 obj\n" % n) + objs[n] + b"\nendobj\n"
off[5] = len(out); out += b"5 0 obj\n" + obj5 + b"\nendobj\n"
off[7] = len(out)
def rec(t, a, b): return bytes([t]) + a.to_bytes(2, 'big') + bytes([b])
data = rec(0, 0, 255)               # xref stream itself is NOT predicted
for n in (1, 2, 3, 4, 5): data += rec(1, off[n], 0)
data += rec(2, 5, 0) + rec(1, off[7], 0)     # obj 6 -> ObjStm 5 idx 0; obj 7 = xref
xcomp = zlib.compress(data)
obj7 = (b"<</Type/XRef/Size 8/Root 1 0 R/W[1 2 1]/Index[0 8]/Length %d/Filter/FlateDecode>>\nstream\n" % len(xcomp)
        + xcomp + b"\nendstream")
out += b"7 0 obj\n" + obj7 + b"\nendobj\n"
out += b"startxref\n%d\n%%%%EOF\n" % off[7]
open(sys.argv[1], 'wb').write(out)
PY
$BIN fdf-extract "$TMP/predobjstm.pdf" 2>/dev/null | grep -q 'predfield' \
    && pass "predictor-objstm: fdf-extract un-predicts the object stream" || fail "predictor-objstm: field unreachable"
printf '%%FDF-1.2\n1 0 obj\n<< /FDF << /Fields [ << /T (predfield) /V (PREDICTOR_OK) >> ] >> >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%%%EOF\n' > "$TMP/pred.fdf"
$BIN fill "$TMP/pred.fdf" "$TMP/predobjstm.pdf" 2>/dev/null | grep -qa 'PREDICTOR_OK' \
    && pass "predictor-objstm: fill fills the field" || fail "predictor-objstm: fill failed"

echo "== LZWDecode object stream =="
# A field object stored in an LZWDecode-compressed object stream. Only a decoder
# that implements PDF LZW can reach 'lzwfield'. The stream is compressed with a
# real LZW encoder (PIL's TIFF LZW, which is byte-compatible with PDF LZWDecode);
# skipped when PIL is unavailable.
if python3 -c 'import PIL' 2>/dev/null; then
    python3 - "$TMP/lzw.pdf" <<'PY'
import sys, io, zlib
from PIL import Image
def lzw(data):
    b = io.BytesIO(); Image.frombytes('L', (len(data), 1), data).save(b, format="TIFF", compression="tiff_lzw")
    raw = b.getvalue(); im = Image.open(io.BytesIO(raw)); im.load()
    o = im.tag_v2[273]; c = im.tag_v2[279]
    if isinstance(o, int): o = (o,); c = (c,)
    return raw[o[0]:o[0]+c[0]]
field6 = b"<</FT/Tx/T(lzwfield)/Type/Annot/Subtype/Widget/Rect[100 700 300 720]/P 3 0 R>>"
index = b"6 0 "; first = len(index)
comp = lzw(index + field6)
obj5 = (b"<</Type/ObjStm/N 1/First %d/Length %d/Filter/LZWDecode>>\nstream\n" % (first, len(comp))
        + comp + b"\nendstream")
objs = {1: b"<</Type/Catalog/Pages 2 0 R/AcroForm 4 0 R>>",
        2: b"<</Type/Pages/Kids[3 0 R]/Count 1>>",
        3: b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Annots[6 0 R]>>",
        4: b"<</Fields[6 0 R]/DA(/Helv 0 Tf 0 g)>>"}
out = bytearray(b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n"); off = {}
for n in (1, 2, 3, 4):
    off[n] = len(out); out += (b"%d 0 obj\n" % n) + objs[n] + b"\nendobj\n"
off[5] = len(out); out += b"5 0 obj\n" + obj5 + b"\nendobj\n"
off[7] = len(out)
def rec(t, a, b): return bytes([t]) + a.to_bytes(2, 'big') + bytes([b])
data = rec(0, 0, 255)
for n in (1, 2, 3, 4, 5): data += rec(1, off[n], 0)
data += rec(2, 5, 0) + rec(1, off[7], 0)
xcomp = zlib.compress(data)
obj7 = (b"<</Type/XRef/Size 8/Root 1 0 R/W[1 2 1]/Index[0 8]/Length %d/Filter/FlateDecode>>\nstream\n" % len(xcomp)
        + xcomp + b"\nendstream")
out += b"7 0 obj\n" + obj7 + b"\nendobj\n"
out += b"startxref\n%d\n%%%%EOF\n" % off[7]
open(sys.argv[1], 'wb').write(out)
PY
    $BIN fdf-extract "$TMP/lzw.pdf" 2>/dev/null | grep -q 'lzwfield' \
        && pass "LZW: fdf-extract lists the field from the LZW object stream" || fail "LZW: fdf-extract missed the field"
    printf '%%FDF-1.2\n1 0 obj\n<< /FDF << /Fields [ << /T (lzwfield) /V (LZW_OK) >> ] >> >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%%%EOF\n' > "$TMP/lzw.fdf"
    $BIN fill "$TMP/lzw.fdf" "$TMP/lzw.pdf" 2>/dev/null | grep -qa 'LZW_OK' \
        && pass "LZW: fill fills the field" || fail "LZW: fill failed"
else
    echo "  skip:  PIL not installed (LZW object stream not exercised)"
fi

echo "== encrypted documents (standard handler, empty user password) =="
# Generate RC4-128, AES-128 and AES-256 encrypted copies of the fixture with
# pikepdf, then check both extraction (45 fields, as plaintext) and fill (output
# re-opens under the same encryption and the value round-trips). Skipped when
# pikepdf is unavailable.
if python3 -c 'import pikepdf' 2>/dev/null; then
    NAME=$(grep -oE '^/T \(([^)]*)\)' "$TMP/out.fdf" | head -1 | sed 's|^/T (||; s|)$||')
    python3 - "$PDF" "$TMP" <<'PY'
import sys, pikepdf
src, tmp = sys.argv[1], sys.argv[2]
variants = {
    "rc4":  pikepdf.Encryption(owner="", user="", R=3, aes=False, metadata=False),
    "aes1": pikepdf.Encryption(owner="", user="", R=4, aes=True),
    "aes2": pikepdf.Encryption(owner="", user="", R=6, aes=True),
}
for name, enc in variants.items():
    pdf = pikepdf.open(src)
    pdf.save(f"{tmp}/enc_{name}.pdf", encryption=enc)
PY
    nplain=$(grep -c '^/T (' "$TMP/out.fdf")
    printf '%%FDF-1.2\n1 0 obj\n<< /FDF << /Fields [ << /T (%s) /V (ENC_ROUNDTRIP) >> ] >> >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%%%EOF\n' "$NAME" > "$TMP/enc.fdf"
    for v in rc4 aes1 aes2; do
        n=$($BIN fdf-extract "$TMP/enc_$v.pdf" 2>/dev/null | grep -c '^/T (')
        [ "$n" = "$nplain" ] && pass "$v: extracted $n fields (decrypted)" || fail "$v: extracted $n/$nplain fields"
        $BIN fill "$TMP/enc.fdf" "$TMP/enc_$v.pdf" > "$TMP/encfilled_$v.pdf" 2>/dev/null
        python3 - "$TMP/encfilled_$v.pdf" <<'PY' && pass "$v: fill re-opens encrypted, value round-trips" || fail "$v: filled output broken"
import sys, pikepdf
pdf = pikepdf.open(sys.argv[1])          # empty password; raises if inconsistent
hit = [False]
def walk(f):
    if '/V' in f and str(f.get('/V')) == "ENC_ROUNDTRIP": hit[0] = True
    for k in f.get('/Kids', []): walk(k)
for f in pdf.Root.AcroForm.Fields: walk(f)
assert hit[0], "filled value not found after re-decryption"
PY
    done
else
    echo "  skip:  pikepdf not installed (encryption round-trip not exercised)"
fi

echo ""
if [ "$fails" -eq 0 ]; then
    echo "E2E: all checks passed"
    exit 0
else
    echo "E2E: $fails failure(s)"
    exit 1
fi
