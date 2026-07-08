# Security Policy

`ffpdf` parses untrusted PDF (and FDF) input, so its security is taken
seriously. This document covers how to report issues and what has been done to
harden the tool.

## Reporting a vulnerability

Please report suspected vulnerabilities **privately** to **pb@0x00f.foo**
(you may encrypt if you wish). Include a description, affected version/commit,
and, ideally, a minimal reproducer file.

- We aim to acknowledge reports within **3 business days** and to provide a
  remediation plan within **10 business days**.
- Please give us a reasonable chance to fix the issue before public disclosure.
- There is no bug-bounty program, but we are glad to credit reporters.

Do **not** open a public issue for a security report.

## Supported versions

This is a single-branch project; security fixes land on `main` and in the next
tagged release. Use the latest release.

## Threat model

The tool is a local, offline command-line program. The assets are the integrity
of the process (no memory corruption / code execution from a malicious file)
and the confidentiality of the data being processed (which may be sensitive,
e.g. PHI/PII on medical or financial forms).

In scope:
- Memory-corruption (heap/stack overflow, OOB read/write, use-after-free) or
  denial-of-service (crash, hang, unbounded memory) from a crafted **PDF** or
  **FDF** input.
- Unintended disclosure of processed data (e.g. to logs or temp files).

Out of scope:
- The correctness of the *content* an operator chooses to put in a form.
- Compromise of the host, OS, or C library.
- Cracking password-protected PDFs (unsupported by design; see README).

## What we do to reduce risk

- **Open source (Apache-2.0).** The entire ~5k-line C source is auditable; the only
  runtime dependency is zlib.
- **Continuous fuzzing.** An AddressSanitizer + UndefinedBehaviorSanitizer
  harness (`make fuzz` / `fuzz.sh`) mutates real PDFs, including encrypted
  variants, through both the **read** and **fill** paths. Tens of thousands of
  iterations run clean.
- **Security audits.** The parser, cross-reference/object-stream decoding, the
  crypto handler, and the fill/write path have been reviewed adversarially;
  findings are fixed with regression tests (see `test_e2e.sh`).
- **Hardened builds.** Compiled with `-fstack-protector-strong`,
  `_FORTIFY_SOURCE=2`, PIE, and full RELRO on Linux (equivalents on macOS and
  Windows). Decompression is bounded to defuse "zip bombs"
  (`PDF_MAX_DECOMPRESSED`, `PDF_MAX_TOTAL_DECOMPRESSED`).
- **Data minimization.** No network access, no telemetry, no temporary files in
  default operation (only the opt-in `fill -` and `-o` modes stage private,
  self-cleaning temp files; see the datasheet), and filled values (potential
  PHI/PII) are **not** written to logs by default.

For privacy/PHI deployment guidance, see
[`docs/SECURITY-PRIVACY.md`](docs/SECURITY-PRIVACY.md).
