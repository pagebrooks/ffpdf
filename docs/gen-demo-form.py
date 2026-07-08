#!/usr/bin/env python3
"""Generate docs/demo-form.pdf: a tiny AcroForm with Address/City/State/Zip."""
import sys

FIELDS = [  # (name, label y, rect)
    ("Address", 700, (150, 693, 460, 713)),
    ("City",    660, (150, 653, 340, 673)),
    ("State",   620, (150, 613, 210, 633)),
    ("Zip",     580, (150, 573, 250, 593)),
]

content = "".join(
    f"BT /Helv 12 Tf 72 {y} Td ({name}:) Tj ET\n" for name, y, _ in FIELDS
)
content = ("BT /Helv 16 Tf 72 740 Td (ffpdf demo form) Tj ET\n" + content).encode()

annot_refs = " ".join(f"{4+i} 0 R" for i in range(len(FIELDS)))
objs = {
    1: b"<< /Type /Catalog /Pages 2 0 R /AcroForm 10 0 R >>",
    10: f"<< /Fields [{annot_refs}] "
        f"/NeedAppearances true /DA (/Helv 12 Tf 0 g) "
        f"/DR << /Font << /Helv 8 0 R >> >> >>".encode(),
    2: b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    3: f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
       f"/Annots [{annot_refs}] /Resources << /Font << /Helv 8 0 R >> >> "
       f"/Contents 9 0 R >>".encode(),
    8: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Name /Helv >>",
    9: b"<< /Length %d >>\nstream\n%s\nendstream" % (len(content) + 1, content),
}
for i, (name, _, r) in enumerate(FIELDS):
    objs[4 + i] = (
        f"<< /Type /Annot /Subtype /Widget /FT /Tx /T ({name}) /V () "
        f"/Rect [{r[0]} {r[1]} {r[2]} {r[3]}] /DA (/Helv 12 Tf 0 g) "
        f"/F 4 /P 3 0 R >>".encode()
    )

out = bytearray(b"%PDF-1.7\n")
off = {}
for n in sorted(objs):
    off[n] = len(out)
    out += b"%d 0 obj\n" % n + objs[n] + b"\nendobj\n"
xref_off = len(out)
out += b"xref\n0 %d\n" % (len(objs) + 1)
out += b"0000000000 65535 f \n"
out += b"".join(b"%010d 00000 n \n" % off[n] for n in sorted(objs))
out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (
    len(objs) + 1, xref_off)

open(sys.argv[1], "wb").write(out)
print(f"wrote {sys.argv[1]} ({len(out)} bytes)")
