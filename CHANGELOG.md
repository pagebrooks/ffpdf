# Changelog

All notable changes to ffpdf are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- **Field names may change for forms with non-ASCII names.** Field names
  (`/T`), labels (`/TU`), values and choice options are now decoded as PDF
  text strings into UTF-8 (#22). A name such as `(payee\220s name)` is now
  reported and filled as `payee’s name`; it used to come out as
  `payee220s name`, and that spelling no longer matches. UTF-16 names that
  used to show `?` for each non-ASCII character (`Ca?f? ?`) now keep the real
  characters (`Café 中`). Run `fields` again to get the current names. Plain
  ASCII names are unchanged.

- The Windows release binary is now built in MSYS2's UCRT64 environment
  instead of MINGW64, so `ffpdf.exe` links the Universal C Runtime
  (`ucrtbase.dll`) instead of the legacy `msvcrt.dll`. The UCRT ships with
  Windows 10, Windows 11 and Windows Server 2016 and later; Windows 7 and 8.1
  need update KB2999226. The binary is still statically linked with no other
  DLL dependencies. MSYS2 no longer publishes pikepdf (used by the end-to-end
  tests) for MINGW64, which broke the Windows CI job. The README's Windows
  build instructions now use the UCRT64 package names.

### Fixed

- `fill --flatten` no longer blanks a page whose `/Contents` is an indirect
  reference to an array of streams (`/Contents 12 0 R` with object 12
  `[6 0 R 7 0 R]`). The array was nested inside the new `/Contents` array,
  which viewers reject, so the whole page rendered blank, original content
  included. Its streams are now spliced into one flat array (#20).
- `fill --flatten` keeps a page's original resources when its `/Resources` is
  an indirect reference or is inherited from the page tree. The rewritten page
  used to get a resource dictionary holding only the stamped fields, so the
  original page content lost its fonts and images. An indirect `/XObject`
  dictionary inside the resources is merged the same way.
- `fill --flatten` flattens the widgets of a page whose `/Annots` is an
  indirect reference to an array stored outside an object stream. Those
  widgets were left on the page as live annotations after the form was
  removed.
- PDF text strings are decoded per ISO 32000-1: literal escapes (octal such
  as `\220`, `\n`, line continuations, balanced parentheses), PDFDocEncoding
  bytes (0x90 is `’`, not an invalid UTF-8 byte), UTF-16BE including
  surrogate pairs, and UTF-8 with a byte order mark. `fields` no longer emits
  invalid UTF-8 JSON for forms with raw PDFDocEncoding labels, such as the
  California SCGR-1 (#22). Hand-written FDF files may still use raw UTF-8.
- `fdf-extract` writes non-ASCII field names as UTF-16BE hex strings, so they
  feed back into `fill`, and FDF values written as hex strings (`<FEFF...>`)
  are now read instead of being treated as empty. `xfdf-extract` now exports
  hex-string values too.
- `fields` lists every option of a choice field. Options with characters
  outside WinAnsi used to make it omit the whole list.
- Values and options containing typographic punctuation (`’ “ ” – — € …`)
  now get a generated appearance stream instead of relying on the viewer.
- An `/AcroForm` written as a direct dictionary in the catalog
  (`/AcroForm << /Fields [...] >>`, which pikepdf produces by default) is now
  read like the usual indirect reference. Before, `fields` reported
  `count: 0` with exit 0 and `fill` failed with "no AcroForm found". `fill`
  moves the inline dictionary into a new object (with `/NeedAppearances`) and
  points the catalog at it; `--flatten` removes it as before (#23).
- A catalog whose `/AcroForm` entry is present but cannot be resolved (a
  reference to a missing object or to something that is not a dictionary, or
  a value that is neither) is now an error with exit 1, instead of being
  treated as a document with no form. `/AcroForm null` still means no form.
- Looking up an uncompressed object that is not a dictionary (an array or a
  number, for example) no longer returns the dictionary of the object that
  follows it in the file.
- Comb text fields (`/Ff` Comb flag with `/MaxLen`) are now drawn one
  character per cell: the field width is split into `MaxLen` equal cells and
  each character is centered in its cell. They used to be drawn as one
  left-aligned run, so digits bunched up at the left instead of lining up with
  the printed boxes on forms such as tax IDs and ZIP codes. Applies to both
  plain and `--flatten` fills (#21).
- Object dictionaries that end in a hex string right before `>>` (for example
  `/V<ab>>>`) are now read whole. The hex string's closing `>` was paired with
  the dictionary's, cutting the dictionary one byte short. For a stream object
  such as an object stream, the stream was then never found and the fields
  inside it silently disappeared (#15).
- Encrypted PDFs whose `/O`, `/U` or `/ID` strings are written as literal
  strings with escapes (`\n`, `\r`, `\ddd` octal, line continuations) now
  decrypt. Each escape was read as the escaped letter, so ffpdf derived the
  wrong file key and `fields` reported `count: 0` with exit 0 (#14). Literal
  strings inside encrypted objects are now decoded the same spec-compliant way.
- An encrypted PDF that cannot be decrypted is now an error instead of a silent
  empty or garbled result. This covers files that need a password, use an
  unsupported security handler, or derive a key that fails `/U` authentication.
  `fields`, `fdf-extract`, `xfdf-extract` and `fill` exit 1 with the reason on
  stderr and write nothing to stdout.
- Encrypted forms: field names stored outside object streams no longer read as
  `????`. Decrypted strings are re-emitted as hex, and hex field names were
  always decoded as UTF-16BE even without the FE FF byte-order mark. `fields`
  listed garbled names, and `fill` reported every such field as not found. This
  hit the first fill of forms saved without object streams, and any refill of a
  field an earlier fill had rewritten.

## [0.1.2] - 2026-07-31

### Added

- Homebrew install: `brew install pagebrooks/tap/ffpdf`. The formula builds from
  the release source tarball, so one formula covers Intel and Apple Silicon
  macOS as well as Homebrew-on-Linux. The canonical copy lives in
  `packaging/homebrew/`.
- `install.sh`: a POSIX shell installer that downloads the archive for the
  detected OS and CPU, verifies it against the release's `checksums.txt`, and
  installs into `~/.local/bin` without root. `FFPDF_INSTALL_DIR` and
  `FFPDF_VERSION` override the destination and the release.
- `checksums.txt`: a single combined SHA-256 manifest covering every release
  asset, alongside the existing per-file `.sha256` sidecars, so a download can
  be verified with one `sha256sum -c`.
- `ARCHFLAGS` in the Makefile, appended to both the compile and link steps, for
  building multi-architecture binaries.

### Changed

- The macOS release artifact is now a universal2 binary (arm64 + x86_64) and is
  named `ffpdf-macos-universal2.tar.gz` (previously `ffpdf-macos.tar.gz`).

## [0.1.1] - 2026-07-10

### Added

- `fields` command: list every fillable field as JSON, with names,
  human-readable labels (`/TU`), types, current values (UTF-16 values decoded
  to UTF-8), `required`/`readonly`/`maxlen` constraints, choice options and
  combo/multi-select flags, and checkbox on-state names. A machine-readable
  companion to `fdf-extract` for scripts and AI agents.

- `fill --json` writes a machine-readable result to stdout (the `updated`
  and `not_found` field names and their counts; requires `-o` for the PDF),
  and `fill --strict` exits 3 if any FDF field did not match a form field.
  For scripts and AI agents that need to verify a fill by exit code or
  parsed output rather than scraping stderr.
- Documented that multi-select choice fields are filled with an array,
  `/V [(opt1) (opt2)]`.
- `fill` now accepts a **JSON** values object as well as an FDF, auto-detected
  from the file's content, and takes the PDF and values file in **either
  order** (`-` reads the values from stdin). JSON values may be a string, a
  multi-select array, a number, or a boolean (to check/clear a checkbox). This
  closes the discover-then-fill loop in one format for agents. The documented
  argument order is now `fill <pdf> <values>`.
- Radio-button groups: `fields` now reports the group's valid option names,
  and `fill` sets the selected button's appearance state (`/AS`) so the choice
  renders, not just the group value.

### Changed

- `fill` now exits **2** (and writes no output) when no field in the input
  matched the form. A no-op fill — e.g. every field name misspelled — is
  treated as a failure by default rather than a silent success, so callers
  need no flag to detect it. `--flatten` is exempt (it removes the form even
  with no new values), and a partial match (some fields land) still exits 0
  unless `--strict` is given.

- A value longer than a text field's `/MaxLen` is now truncated to fit (on a
  UTF-8 character boundary) with a warning, instead of writing an over-length,
  non-conformant value; `fill --json` lists such fields under `truncated`.

### Fixed

- ffpdf can now re-read PDFs it filled. Streams without a `/Filter` (legal
  PDF, and exactly what fill emits for its appended cross-reference stream)
  were rejected by the stream decoder, so every filled output failed to parse
  with "Failed to parse xref table".
- `fdf-extract` and `xfdf-extract` now emit each field's current `/V` value
  (strings, names, and multi-select arrays). Previously the value slot was
  always printed empty, despite the documentation saying otherwise.
- Hardened two attacker-influenced allocation sites (CodeQL
  `cpp/uncontrolled-allocation-size`): a stream's declared `/Length` is now
  capped at the bytes remaining in the file before allocation (also avoiding
  32-bit overflow on Windows), and the XFA datasets loader routes every
  stream through the capped decoder instead of copying raw bytes unbounded.

## [0.1.0] - 2026-07-08

Initial public release.

### Added

- `fdf-extract` / `xfdf-extract`: extract AcroForm form fields (names, values,
  types) as FDF or XFDF, ready to edit and fill back in.
- `fill`: fill a PDF from an FDF, written as an incremental update; original
  bytes are preserved verbatim and changes are appended. Generates text
  appearance streams and sets checkbox/radio and choice (`/Opt`, `/I`) states.
- `fill --flatten` (short form `-f`): bake filled values into page content and
  remove the form, producing a non-editable PDF.
- `xref`: dump the parsed cross-reference table as JSON (debugging aid).
- CLI conventions: `--` ends option parsing on every command; `fill` accepts
  `-` to read the FDF from stdin and `-o FILE` to write the PDF atomically
  (assembled as `FILE.tmp`, renamed into place only on success), and refuses
  to write PDF bytes to a terminal.
- Encrypted-PDF support for the standard security handler with an empty user
  password: RC4-40/128 and AES-128/256 (R3–R6).
- XFA-aware filling: values are synced into the XFA `datasets` packet (with XML
  escaping) so LiveCycle forms display filled values.
- Non-ASCII field values round-trip as UTF-16BE; non-ASCII file paths work on
  Windows (UTF-8 argv).
- Robust parsing for real-world PDFs: compressed xref and object streams, PNG
  and TIFF predictors, LZWDecode, hybrid-reference files (`/XRefStm`),
  indirect `/Fields` arrays, CR-only line endings.
- Hardening for untrusted input: decompression-bomb caps
  (`PDF_MAX_DECOMPRESSED`, `PDF_MAX_TOTAL_DECOMPRESSED`), hardened compilation
  (stack canaries, FORTIFY, PIE, RELRO), no network access, no telemetry, and
  values never logged unless `FFPDF_VERBOSE=1`.
- Single ~100 KB binary for Linux, macOS, and Windows; zlib is the only
  runtime dependency. Includes a man page (`ffpdf.1`) and `make install`.
  Release downloads are archives bundling the binary with `LICENSE`, `NOTICE`,
  the README, and the man page, each with a SHA-256 checksum and a sigstore
  attestation.
