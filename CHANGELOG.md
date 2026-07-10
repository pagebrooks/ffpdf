# Changelog

All notable changes to ffpdf are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `fields` command: list every fillable field as JSON, with names, types,
  current values (UTF-16 values decoded to UTF-8), choice options and
  combo/multi-select flags, and checkbox on-state names. A machine-readable
  companion to `fdf-extract` for scripts and AI agents.

### Fixed

- ffpdf can now re-read PDFs it filled. Streams without a `/Filter` (legal
  PDF, and exactly what fill emits for its appended cross-reference stream)
  were rejected by the stream decoder, so every filled output failed to parse
  with "Failed to parse xref table".
- `fdf-extract` and `xfdf-extract` now emit each field's current `/V` value
  (strings, names, and multi-select arrays). Previously the value slot was
  always printed empty, despite the documentation saying otherwise.

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
