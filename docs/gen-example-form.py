#!/usr/bin/env python3
"""Generate docs/example-form.pdf: a realistic, styled fillable form.

A one-page "insurance policyholder" form exercising every field type ffpdf
handles: ten text fields, a checkbox (with real appearance streams), a combo
box, a multi-select list box, and a signature field (which fill leaves
untouched). Used by the README's worked example; example-filled.pdf and
example-flattened.pdf are produced from it by `ffpdf fill` (see the
Makefile `examples` target).

PDF gotchas encoded here, learned the hard way:
  - object numbers must be contiguous (the classic xref table below has no
    gaps), and
  - /DR needs a /ZaDb (ZapfDingbats) font because viewers regenerating
    checkbox appearances under /NeedAppearances expect it.
"""
import sys

NAVY = "0.10 0.21 0.36"
GOLD = "0.83 0.62 0.18"
GRAY_TXT = "0.35"
BOX_BORDER = "0.62"

W, H = 612, 792
LM, RM = 54, 558          # left/right margin x


class Content:
    def __init__(self):
        self.ops = []

    def raw(self, s):
        self.ops.append(s)

    def rect(self, x, y, w, h, fill=None, stroke=None, lw=0.75):
        if fill:
            self.ops.append(f"{fill} rg {x} {y} {w} {h} re f")
        if stroke:
            self.ops.append(f"{stroke} {stroke} {stroke} RG {lw} w {x} {y} {w} {h} re S")

    def text(self, x, y, s, font="Helv", size=10, color="0 0 0"):
        s = s.replace("\\", r"\\").replace("(", r"\(").replace(")", r"\)")
        self.ops.append(f"BT /{font} {size} Tf {color} rg {x} {y} Td ({s}) Tj ET")

    def hline(self, x, y, w, color=NAVY, lh=0.8):
        self.ops.append(f"{color} rg {x} {y} {w} {lh} re f")

    def bytes(self):
        return ("\n".join(self.ops) + "\n").encode()


c = Content()
widgets = []   # raw widget dictionary bodies, in object order


def label(x, y, s):
    c.text(x, y, s.upper(), font="HelvB", size=7.5,
           color=f"{GRAY_TXT} {GRAY_TXT} {GRAY_TXT}")


def box(x, y, w, h):
    c.rect(x, y, w, h, fill="1 1 1", stroke=BOX_BORDER)


def text_field(name, lab, x, y, w, h=22):
    label(x, y + h + 5, lab)
    box(x, y, w, h)
    widgets.append(
        f"<< /Type /Annot /Subtype /Widget /FT /Tx /T ({name}) /V () "
        f"/Rect [{x} {y} {x + w} {y + h}] /DA (/Helv 11 Tf 0 g) "
        f"/F 4 /P 3 0 R >>")


def combo_field(name, lab, x, y, w, opts, h=22):
    label(x, y + h + 5, lab)
    box(x, y, w, h)
    ax = x + w - 15                                   # dropdown affordance
    c.raw(f"0.45 g {ax} {y + 13} m {ax + 8} {y + 13} l {ax + 4} {y + 7} l f")
    opt = "".join(f"({o})" for o in opts)
    widgets.append(
        f"<< /Type /Annot /Subtype /Widget /FT /Ch /Ff 131072 /T ({name}) "
        f"/V () /Opt [{opt}] /Rect [{x} {y} {x + w} {y + h}] "
        f"/DA (/Helv 11 Tf 0 g) /F 4 /P 3 0 R >>")


def list_field(name, lab, x, y, w, h, opts):
    label(x, y + h + 5, lab)
    box(x, y, w, h)
    opt = "".join(f"({o})" for o in opts)
    widgets.append(
        f"<< /Type /Annot /Subtype /Widget /FT /Ch /Ff 2097152 /T ({name}) "
        f"/V () /Opt [{opt}] /Rect [{x} {y} {x + w} {y + h}] "
        f"/DA (/Helv 10 Tf 0 g) /F 4 /P 3 0 R >>")


# ---- header banner -------------------------------------------------------
c.rect(0, 720, W, 72, fill=NAVY)
c.rect(0, 716, W, 4, fill=GOLD)
c.text(LM, 748, "Policyholder Information", font="HelvB", size=21, color="1 1 1")
c.text(LM, 730, "Meridian Mutual Insurance   |   Form MM-204", size=9.5,
       color="0.78 0.83 0.91")
c.text(430, 748, "OFFICE USE ONLY", font="HelvB", size=7, color="0.55 0.62 0.74")
c.text(LM, 698, "Please complete all sections in blue or black ink. "
                "Fields marked * are required.", size=9,
       color=f"{GRAY_TXT} {GRAY_TXT} {GRAY_TXT}")


def section(n, title, y):
    c.text(LM, y, f"{n}.  {title}", font="HelvB", size=11, color=NAVY)
    c.hline(LM, y - 6, RM - LM)


# ---- 1. applicant --------------------------------------------------------
section(1, "APPLICANT", 668)
text_field("FullName",    "Full name *",     LM,  620, 326)
text_field("DateOfBirth", "Date of birth *", 400, 620, 158)
text_field("Email",       "Email address",   LM,  572, 246)
text_field("Phone",       "Phone",           330, 572, 228)

# ---- 2. mailing address --------------------------------------------------
section(2, "MAILING ADDRESS", 536)
text_field("StreetAddress", "Street address *", LM, 488, 504)
text_field("City",          "City *",           LM, 440, 246)
text_field("State",         "State *",          330, 440, 70)
text_field("Zip",           "ZIP *",            430, 440, 128)

# ---- 3. policy -----------------------------------------------------------
section(3, "POLICY", 404)
text_field("PolicyNumber", "Policy number *", LM, 356, 246)
combo_field("CoverageType", "Coverage type *", 330, 356, 228,
            ["Auto", "Home", "Life", "Umbrella"])

list_field("AdditionalCoverages", "Additional coverages (select all that apply)",
           LM, 284, 246, 48, ["Flood", "Earthquake", "Identity theft", "Pet injury"])

# checkbox (widget draws its own check via AP; page draws the square)
CB = (330, 300, 346, 316)
c.rect(CB[0], CB[1], 16, 16, fill="1 1 1", stroke=BOX_BORDER)
c.text(CB[2] + 8, CB[1] + 4, "Enroll in paperless billing", size=9.5,
       color="0.13 0.13 0.13")
CHECKBOX_DICT_INDEX = len(widgets)      # patched with AP refs after numbering
widgets.append(None)

# ---- signature -----------------------------------------------------------
# A real /Sig field: ffpdf deliberately leaves signature fields untouched,
# so it stays empty in the filled output.
c.hline(LM, 232, 300, color="0.35 0.35 0.35", lh=0.7)
c.text(LM, 220, "SIGNATURE", font="HelvB", size=7.5,
       color=f"{GRAY_TXT} {GRAY_TXT} {GRAY_TXT}")
widgets.append(
    f"<< /Type /Annot /Subtype /Widget /FT /Sig /T (Signature) "
    f"/Rect [{LM} 234 354 258] /F 4 /P 3 0 R >>")
text_field("SignatureDate", "Date", 430, 232, 128)

# ---- return instructions -------------------------------------------------
c.rect(LM, 118, RM - LM, 74, fill="0.95 0.96 0.97")
c.rect(LM, 118, 3, 74, fill=GOLD)
c.text(LM + 16, 172, "RETURNING THIS FORM", font="HelvB", size=8, color=NAVY)
c.text(LM + 16, 156, "Mail the completed form to Meridian Mutual Insurance, "
                     "PO Box 2204, Portsmouth, NH 03801,", size=9,
       color="0.25 0.25 0.25")
c.text(LM + 16, 142, "or email a scanned copy to forms@meridianmutual.example. "
                     "Processing takes 5 business days.", size=9,
       color="0.25 0.25 0.25")

# ---- footer --------------------------------------------------------------
c.hline(LM, 72, RM - LM, color="0.75 0.75 0.75", lh=0.6)
c.text(LM, 58, "Meridian Mutual Insurance  ·  Form MM-204 (rev. 2026)  ·  Page 1 of 1",
       size=7.5, color="0.5 0.5 0.5")
c.text(LM, 46, "This sample form is part of the ffpdf documentation and is not a real document.",
       size=7, color="0.65 0.65 0.65")

# ---- objects (contiguous numbering; see module docstring) -----------------
content = c.bytes()
first_widget = 7
ap_on = first_widget + len(widgets)
ap_off = ap_on + 1
zadb = ap_off + 1
acro = zadb + 1

widgets[CHECKBOX_DICT_INDEX] = (
    f"<< /Type /Annot /Subtype /Widget /FT /Btn /T (PaperlessBilling) "
    f"/V /Off /AS /Off /Rect [{CB[0]} {CB[1]} {CB[2]} {CB[3]}] "
    f"/F 4 /P 3 0 R /MK << /CA (4) >> "
    f"/AP << /N << /On {ap_on} 0 R /Off {ap_off} 0 R >> >> >>")

annot_refs = " ".join(f"{first_widget + i} 0 R" for i in range(len(widgets)))

objs = {
    1: b"<< /Type /Catalog /Pages 2 0 R /AcroForm %d 0 R >>" % acro,
    2: b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    3: (f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {W} {H}] "
        f"/Annots [{annot_refs}] "
        f"/Resources << /Font << /Helv 5 0 R /HelvB 6 0 R >> >> "
        f"/Contents 4 0 R >>").encode(),
    4: b"<< /Length %d >>\nstream\n%s\nendstream" % (len(content), content),
    5: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Name /Helv >>",
    6: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Name /HelvB >>",
}
for i, wdict in enumerate(widgets):
    objs[first_widget + i] = wdict.encode()

check = f"{NAVY} RG 2 w 3.5 8 m 6.5 4.5 l 12.5 12 l S\n".encode()
objs[ap_on] = (b"<< /Type /XObject /Subtype /Form /BBox [0 0 16 16] /Length %d >>\n"
               b"stream\n%sendstream" % (len(check), check))
objs[ap_off] = (b"<< /Type /XObject /Subtype /Form /BBox [0 0 16 16] /Length 0 >>\n"
                b"stream\nendstream")
objs[zadb] = b"<< /Type /Font /Subtype /Type1 /BaseFont /ZapfDingbats /Name /ZaDb >>"
objs[acro] = (f"<< /Fields [{annot_refs}] /NeedAppearances true "
              f"/DA (/Helv 11 Tf 0 g) "
              f"/DR << /Font << /Helv 5 0 R /HelvB 6 0 R /ZaDb {zadb} 0 R >> >> >>").encode()

out = bytearray(b"%PDF-1.7\n")
off = {}
for n in sorted(objs):
    off[n] = len(out)
    out += b"%d 0 obj\n" % n + objs[n] + b"\nendobj\n"
xref_off = len(out)
out += b"xref\n0 %d\n" % (len(objs) + 1)
out += b"0000000000 65535 f \n"
out += b"".join(b"%010d 00000 n \n" % off[n] for n in sorted(objs))
out += (b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n"
        % (len(objs) + 1, xref_off))

open(sys.argv[1], "wb").write(out)
print(f"wrote {sys.argv[1]} ({len(out)} bytes, {len(widgets)} fields)")
