# ffpdf

[![CI](https://github.com/pagebrooks/ffpdf/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/pagebrooks/ffpdf/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/pagebrooks/ffpdf)](LICENSE)
[![Latest release](https://img.shields.io/github/v/release/pagebrooks/ffpdf)](https://github.com/pagebrooks/ffpdf/releases/latest)

**ffpdf** is a small, fast command-line tool for **reading and filling PDF form fields**. It is written in plain C with no dependency beyond zlib. No Java, no pdftk, no headless browser. Point it at a PDF, hand it a list of values, and it writes a filled PDF to stdout.

![ffpdf demo: discover fields, fill the form, get a valid PDF](docs/demo.gif)

It’s built to handle real-world PDFs: government forms, bank documents, LiveCycle/XFA forms, and encrypted “secured” PDFs, not just clean textbook files.

```console
$ ./ffpdf fill answers.fdf form.pdf > filled.pdf
```

---

## Purpose

Most "fill a PDF form" tools drag in a huge runtime or require commercial licensing. This one is a single ~100 KB binary that:

- **Starts instantly and stays small.** Filling streams the original file straight through and only buffers the bytes it appends, so memory stays proportional to *your changes*, not the file size. A 22 MB PDF parses in a fraction of a second.
- **Preserves the original.** Fills are written as a PDF *incremental update*: the original bytes are copied verbatim and the changes are appended. Nothing is silently rewritten.
- **Handles the hard cases** (encryption, XFA, compressed object streams, weird cross-reference layouts).
- **Made for the thousand-PDF loop.** With no runtime to start, a fill is just a process exec: ~600 real forms per second on one core. See [Filling PDFs by the thousands](#filling-pdfs-by-the-thousands).

---

## Quick start

```console
# 1. Build it (needs gcc and zlib)
make

# 2. See what fields a form has
./ffpdf fdf-extract form.pdf

# 3. Fill it
./ffpdf fill answers.fdf form.pdf > filled.pdf
```

That’s the whole loop: **discover → edit → fill.**

### A complete example

Say `form.pdf` has a text field named `applicant_name` and a checkbox named `agree`.

**1. Discover the field names** with `fdf-extract` (it prints an FDF listing every field):

```console
$ ./ffpdf fdf-extract form.pdf
%FDF-1.2
1 0 obj
<<
/FDF
<<
/Fields [
<<
/T (applicant_name)
/V ()
>>
<<
/T (agree)
/V ()
>>
]
...
```

Each field is a `<< /T (name) /V (current value) >>` block; the `/T` is the name you fill by.

**2. Write your answers** into a small FDF file, `answers.fdf`:

```
%FDF-1.2
1 0 obj
<< /FDF << /Fields [
  << /T (applicant_name) /V (Ada Lovelace) >>
  << /T (agree) /V (Yes) >>
] >> >>
endobj
trailer
<< /Root 1 0 R >>
%%EOF
```

**3. Fill:**

```console
$ ./ffpdf fill answers.fdf form.pdf > filled.pdf
```

Open `filled.pdf`: the name is typed in and the box is checked. The tool figures out the right on‑state for the checkbox, generates the visual appearance so the values show in *any* viewer, and (for dynamic XFA forms) also updates the XFA data packet so Adobe renders it too.

> **Tip:** field names can be long and nested (e.g. `topmostSubform[0].Page1[0].f1_1[0]`). You can use the full name from `fdf-extract`, or just the last `.`-separated piece if it’s unique.

### See it on a real form

Start with a blank form:

![The unfilled example form](docs/example-form.png)

One `ffpdf fill` command produces the filled version on the left; adding `--flatten` produces the right one, with the same values baked permanently into the page and no interactive form left to edit:

| Filled by ffpdf | Filled with `--flatten` |
|:---:|:---:|
| ![The form filled by ffpdf: text, dropdown, list selections, and checkbox all set, still editable](docs/example-filled.png) | ![The flattened result: the values baked into the page content, with the form removed](docs/example-flattened.png) |

The `docs/` folder carries the actual files, which you can open right in your browser:

| File | What it is |
|---|---|
| [`example-form.pdf`](docs/example-form.pdf) | The unfilled form: a realistic one-page application exercising every field type (text fields, a checkbox, a combo box, a multi-select list box, and a signature field) |
| [`example-answers.fdf`](docs/example-answers.fdf) | The plain-text FDF holding the values, including a multi-select array |
| [`example-filled.pdf`](docs/example-filled.pdf) | The result: text filled, box checked, dropdown set, both list selections highlighted. The signature field stays empty (a signature is not a fillable value), and the form is still editable |
| [`example-flattened.pdf`](docs/example-flattened.pdf) | The `--flatten` result: the same values baked into the page content, with the form removed (nothing left to edit) |

```console
$ ffpdf fill -o example-filled.pdf example-answers.fdf example-form.pdf
$ ffpdf fill --flatten -o example-flattened.pdf example-answers.fdf example-form.pdf
```

The PDFs in the repo were produced by ffpdf itself (`make examples` regenerates all of them), and the values are visible in any viewer because fill generates the visual appearance streams rather than relying on the viewer to draw them.

---

## Filling PDFs by the thousands

Where this tool really earns its keep is **batch filling**. An FDF is plain text, so the file `fdf-extract` gave you is one search-and-replace away from being a **template**. And because ffpdf is a ~100 KB binary with no runtime to warm up, invoking it once per record costs almost nothing.

![ffpdf bulk-fill (mail-merge) demo: a CSV plus an FDF template renders one filled PDF per record](docs/demo-batch.gif)

Turn the extracted FDF into a template by putting placeholders where the values go:

```
%FDF-1.2
1 0 obj
<< /FDF << /Fields [
  << /T (Address) /V (${ADDRESS}) >>
  << /T (City)    /V (${CITY}) >>
  << /T (State)   /V (${STATE}) >>
  << /T (Zip)     /V (${ZIP}) >>
] >> >>
endobj
trailer
<< /Root 1 0 R >>
%%EOF
```

then loop over your data; `envsubst` (from gettext) does the substitution:

```bash
while IFS=, read -r ADDRESS CITY STATE ZIP; do
  export ADDRESS CITY STATE ZIP
  envsubst < template.fdf | ffpdf fill -o "out/$ZIP-$CITY.pdf" - form.pdf
done < customers.csv
```

(`-` reads the FDF straight from the pipe, with no intermediate file to manage, and `-o` writes each PDF **atomically**, so a killed job never leaves partial files in `out/`.)

Measured on a single ordinary core: **1,000 filled PDFs in about a second** for a small form, and **~600 fills/second on a real 74 KB IRS form**, roughly 36,000 a minute before you even reach for `xargs -P`. The per-invocation startup cost that makes JVM-based tools painful in this loop simply isn’t there.

Two practical notes:

- **Escape your data.** `(`, `)` and `\` inside a value must be backslash-escaped in FDF strings, e.g. `/V (Smith \(Jr\))`.
- **Parallelize trivially.** Every fill is an independent process with no shared state: split the input and run N loops, or hand the whole thing to `xargs -P`/GNU `parallel`.

---

## Commands

| Command | What it does |
|---|---|
| `fdf-extract  <pdf>` | Extract all form fields as an **FDF** (to stdout) |
| `xfdf-extract <pdf>` | Extract all form fields as an **XFDF** (XML flavour, to stdout) |
| `fields <pdf>` | List all form fields as **JSON**: names, types, current values, a choice field's options and flags, a checkbox's on-state. The machine-readable companion to `fdf-extract`, built for scripts and AI agents |
| `fill [-f\|--flatten] [-o FILE] <fdf> <pdf>` | Fill `<pdf>` with the values in `<fdf>` (`-` reads the FDF from stdin), write the result to stdout or, with **`-o`**, atomically to `FILE`. With **`--flatten`** (**`-f`**), bake the values into the page and remove the interactive form, producing a non‑editable PDF |
| `xref <pdf>` | Dump the parsed cross-reference table as JSON (a debugging aid) |
| `help` (`-h`, `--help`) | Print full help to stdout and exit |
| `version` (`-v`, `--version`) | Print version and license info and exit |

All commands are subcommands of `ffpdf` (e.g. `ffpdf fill data.fdf form.pdf`).

Everything goes to **stdout**, so redirect it (`... > out.pdf`), or use `-o out.pdf`, which writes **atomically** (the file is assembled as `out.pdf.tmp` and renamed into place only on success, so a failed fill never leaves a partial output). As a safety net, `fill` refuses to write PDF bytes to a terminal. Progress and warnings go to stderr, so they won’t corrupt the output. `--` ends option parsing for file names that begin with `-`. Run `ffpdf --help` for a built-in summary, or see the `ffpdf(1)` man page.

---

## What it supports

**Form types**
- AcroForm fields (the standard kind)
- Dynamic **XFA** forms (Adobe LiveCycle): the AcroForm layer *and* the XFA data packet are kept in sync

**Field types** 

| Type | Filling behaviour |
|---|---|
| Text | Value written; visual appearance generated (UTF‑16 for non‑ASCII) |
| Checkbox / radio | Maps `Yes`/`On`/`1` (or an explicit state) to the widget’s real on‑state |
| Choice (combo / list box) | Matches your value against the options, sets the selection index, and draws it, single **or** multi‑select |
| Signature | Left untouched (a signature isn’t a fillable value) |

**Under the hood.** The parsing machinery that makes the above work on real files:
- Flate **and** LZW stream decompression, with PNG and TIFF‑2 predictors
- Compressed object streams, classic + cross‑reference‑stream + hybrid xref layouts
- **Encryption**: the standard security handler with an empty user password (RC4, AES‑128, and AES‑256), for both reading *and* filling
- Correct handling of the fiddly bits: non‑ASCII values, on‑state discovery, appearance streams that render even in viewers that ignore `NeedAppearances`

---

## How it compares

This tool deliberately does **one job**, reading and filling form fields, and does it as a tiny, dependency-free binary. [pdftk](https://gitlab.com/pdftk-java/pdftk) and [Aspose.PDF](https://products.aspose.com/pdf/) are much broader tools; here’s the honest trade-off.

| | **ffpdf** | **pdftk** (pdftk-java) | **Aspose.PDF** |
|---|:---:|:---:|:---:|
| **Cost** | Free | Free | 💲 Paid commercial license |
| **License** | Apache-2.0 (permissive) | GPLv2 (copyleft) | Proprietary |
| **What it is** | A CLI + small C codebase | A CLI (Java app) | An SDK/library (.NET, Java, C++, Python…) |
| **Runtime required** | none (just zlib) | a Java runtime (JRE) | a .NET / JVM / native runtime |
| **Footprint** | ~100 KB binary | JRE + jar (tens of MB) | large SDK |
| **Cold-start speed** | instant | pays JVM startup each run | in-process (it’s a library) |
| **Scope** | read & fill form fields | full toolkit: merge, split, stamp, rotate… | practically everything: edit, convert, OCR, sign… |
| **AcroForm filling** | ✅ | ✅ | ✅ |
| **Dynamic XFA filling** | ✅ syncs the XFA data packet | ⚠️ can *drop* XFA, not sync it | ✅ |
| **Encrypted read + fill** | ✅ empty-password RC4/AES | ✅ (supply the password) | ✅ |
| **Memory Usage** | ✅ incremental + streamed | loads the document | loads the document |
| **Drop into a shell pipeline** | ✅ trivial | ✅ (heavy per call) | ❌ you write code |
| **Read the whole source** | ✅ ~5k lines of C | large Java project | ❌ closed source |

**Why pick this one?** If your job is *“take these values, fill this form, hand me a PDF”*, especially in a script, a build step, or a container where you’d rather not ship a JVM or pay for an SDK, then a 100 KB binary with no runtime and no install is hard to beat. It also handles the two things that quietly break other free tools: **dynamic XFA** forms (it updates the data packet Adobe actually renders, instead of leaving a stale one) and **encrypted** forms (read *and* refill, no password needed for the common empty-user-password case).

**When to reach for the others instead:** pdftk if you need general PDF surgery (merging, splitting, stamping, attachments) and a JVM is fine; Aspose.PDF if you need deep editing, format conversion, OCR, or signing inside a larger application and have the budget for a commercial library. This tool won’t do any of that, and that’s the point.

**Why the license matters.** Apache-2.0 is a *permissive* license: you can vendor ffpdf’s source into a closed-source or commercial product, modify it, statically link it, and ship it. The obligations are essentially attribution (keep the `LICENSE`/`NOTICE` files), and it includes an **explicit patent grant** from contributors, which corporate legal teams increasingly require. GPLv2 (pdftk) is *copyleft*: merely **running** the `pdftk` binary from your scripts is fine and imposes nothing, but the moment you vendor, modify, or link its code into something you distribute, your derivative must be GPLv2 too (a non-starter for most proprietary products), and GPLv2 predates explicit patent-grant language. In practice: if you’ll only ever shell out to a CLI, either license works; if there’s any chance you’ll embed the code in a product, an appliance, or a static binary you ship to customers, Apache-2.0 removes the legal friction entirely.

> *pdftk facts reflect the maintained [pdftk-java](https://gitlab.com/pdftk-java/pdftk) fork; Aspose.PDF is a commercial product with a watermarked free trial. “Focused” is a feature here, not an oversight: fewer moving parts, nothing to install, nothing to audit but a small C tree.*

---

## Configuration

Two environment variables cap decompression so a malicious “bomb” PDF can’t exhaust memory. The defaults are generous; you only need these to tighten things in an untrusted‑input setting.

| Variable | Default | Meaning |
|---|---|---|
| `PDF_MAX_DECOMPRESSED` | 256 MB | Max size of any single decompressed stream |
| `PDF_MAX_TOTAL_DECOMPRESSED` | 1 GB | Max total decompressed across one run |

Streams that would blow the budget are treated as corrupt and skipped, rather than allocating unbounded memory.

---

## Building & testing

**Requirements:** `gcc` (C99) and `zlib`. That’s it. (The test suite additionally uses `python3`, and a couple of optional checks use the `pikepdf` and `Pillow` Python packages if present.)

```console
make            # build ./ffpdf
make test       # fast unit tests (no PDFs needed)
make check      # unit tests + end-to-end tests against real fixtures
make clean      # remove build artifacts
make install    # install binary + man page under PREFIX (default /usr/local)
```

`make install` respects `PREFIX` and `DESTDIR` (e.g. `make install PREFIX=~/.local`) and installs the `ffpdf(1)` man page alongside the binary; `make uninstall` removes both.

### Platforms

The code is portable C99 + zlib with no OS-specific headers, so it builds on **Linux, macOS, and Windows**. CI builds and tests all three on every push.

- **Linux:** `gcc`/`clang` + `zlib1g-dev`. Builds out of the box.
- **macOS:** `clang` (Xcode CLT) + the system zlib: `make` just works.
- **Windows:** build in an [MSYS2](https://www.msys2.org/) MinGW-w64 shell (`pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-zlib make`, then `make STATIC=1`), or cross-compile from Linux with `make CC=x86_64-w64-mingw32-gcc STATIC=1`. `STATIC=1` produces a **self-contained `ffpdf.exe`** with no DLL dependencies: drop it anywhere and run. Windows builds also force stdout to binary mode (so `fill` output isn't mangled) and accept **Unicode file paths** (accented names, OneDrive/CJK folders) via `_wfopen`. Prebuilt archives for all three platforms are attached to each release; each bundles the binary with `LICENSE`, `NOTICE`, this README, and the man page.

**A note on zlib on Windows:** there's no usable "native" zlib to link against; the Win32 Compression API speaks MS-specific formats (MSZIP/XPRESS/LZMS), not the zlib/DEFLATE stream PDF needs. Supply zlib via MSYS2/MinGW, [vcpkg](https://vcpkg.io/), or by vendoring zlib's source for a fully self-contained `.exe`.

The end‑to‑end tests in `test_e2e.sh` drive the real binary through the extract → fill round‑trip and exercise the tricky paths (encryption, LZW, predictors, oversized dicts, decompression‑bomb rejection).

---

## How it’s put together

The code is organised into focused modules:

| Module | Responsibility |
|---|---|
| `pdf_lex.{c,h}` | Low‑level PDF syntax: find keys, read strings/references, walk dictionary text |
| `pdf_parser.{c,h}` | Cross‑reference tables & streams, object parsing, predictors |
| `field_map.{c,h}` | The AcroForm field tree, object access, the lazy object‑stream cache |
| `crypto.{c,h}` | MD5/SHA‑2/RC4/AES primitives and the PDF standard security handler |
| `xfa.{c,h}` | XFA datasets sync (navigating the XML by field path) |
| `form_extractor.{c,h}` | The `fdf-extract` / `xfdf-extract` extractors |
| `pdf_filler.{c,h}` | The `fill` command: builds the incremental update |
| `utils.{c,h}` | Decompression (Flate/LZW), the dynamic xref table, shared helpers |

Two design choices shape everything:

- **Incremental updates.** Filling never edits existing bytes; it appends new object versions plus a fresh cross‑reference stream. Original offsets stay valid, and a failure part‑way through leaves no half‑written file.
- **Lazy, streamed I/O.** Object streams are decompressed only when an object inside them is actually needed (and cached), and the original file is streamed to output in chunks rather than held in memory.

---

## References

The formats ffpdf implements are all publicly specified:

- **PDF**: [ISO 32000‑1:2008](https://opensource.adobe.com/dc-acrobat-sdk-docs/pdfstandards/PDF32000_2008.pdf) (Adobe’s free copy). Cross‑reference tables and streams, object streams, encryption, and the AcroForm field model all come from here.
- **FDF / XFDF**: the field‑data formats produced by `fdf-extract` / `xfdf-extract`, defined in the PDF spec above (FDF) and Adobe’s *XML Forms Data Format Specification* (XFDF).
- **XFA**: Adobe’s *XFA Specification*, for the datasets packet that dynamic (LiveCycle) forms render from.

---

## Good to know / limitations

- **Empty‑password encryption only.** Encrypted files that need an *actual* password aren’t supported (by design; this isn’t a cracking tool).
- **Checkbox/radio filling targets the common “field == widget” case.** Radio groups with separate kid widgets fall back to writing your value verbatim.
- **The `xref` command** is a debugging/inspection aid, not part of the fill workflow.
- Output goes to **stdout** by default: remember the `> file.pdf`, or use `-o file.pdf` for an atomic write.

---

## Security & privacy

The tool parses untrusted PDF/FDF input and often handles sensitive data, so it
is built with that in mind: no network access, no telemetry, no temp files in
its default operation (only the opt-in `fill -` and `-o` modes stage private,
self-cleaning temp files; see the [datasheet](docs/SECURITY-PRIVACY.md)), and
filled values are never logged by default (set `FFPDF_VERBOSE=1` to
opt in). It is continuously fuzzed (ASan/UBSan, over the read *and* fill paths,
including encrypted inputs), adversarially audited, and compiled with hardening
(stack canaries, FORTIFY, PIE, RELRO).

- Report a vulnerability: see [`SECURITY.md`](SECURITY.md) (privately to pb@0x00f.foo).
- Deploying for PHI/PII (HIPAA-conscious workflows): see the
  [Security & Privacy Datasheet](docs/SECURITY-PRIVACY.md).

## Support this project

If this tool is useful to you, consider [sponsoring its development](https://github.com/sponsors/pagebrooks).

## License

Copyright 2026 Page Brooks

Licensed under the **Apache License, Version 2.0** (the "License"); you may not use this software except in compliance with the License. You may obtain a copy in the [`LICENSE`](LICENSE) file or at <https://www.apache.org/licenses/LICENSE-2.0>.

Unless required by applicable law or agreed to in writing, software distributed under the License is distributed on an **"AS IS" BASIS, without warranties or conditions of any kind**, either express or implied. See the [`LICENSE`](LICENSE) for the specific language governing permissions and limitations.

**ffpdf** is a trademark of Page Brooks: the license covers the code, not the name or logo (see [`NOTICE`](NOTICE)). Contributions are accepted under the same license via the Developer Certificate of Origin; see [`CONTRIBUTING.md`](CONTRIBUTING.md).

---

*Built in C, with zlib as the only runtime dependency.*
