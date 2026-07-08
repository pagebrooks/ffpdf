# Security & Privacy Datasheet

A reference for security/privacy reviewers evaluating `ffpdf` for use in
regulated environments (e.g. workflows handling PHI under HIPAA, or PII).

> **Not legal advice.** HIPAA compliance is a property of *your organization*
> and its risk analysis, policies, and safeguards, not of any software product.
> No tool is "HIPAA certified." This datasheet describes how the software
> behaves so you can slot it into your own assessment.

## What it is

A single, self-contained command-line binary that reads PDF form fields and
writes filled PDFs. Written in C; the only runtime dependency is zlib. It runs
entirely on the local machine; there is no service, server, agent, or account.

## Data flow

| Property | Behavior |
|---|---|
| Network access | **None.** No sockets, no HTTP, no DNS. Verifiable with `strace`/a network monitor. |
| Telemetry / analytics | **None.** |
| Temporary files | **None in default operation** (path arguments in, stdout out). Two opt-in modes use them: `fill -` spools the piped FDF (which contains form values) to a private temp file (`mkstemp`, mode 0600, in `$TMPDIR`; the user temp directory on Windows), and `-o FILE` stages the output as `FILE.tmp` beside the destination. Both are deleted when the run ends, but a hard kill (SIGKILL, power loss) can leave them behind; avoid `fill -` for PHI if an unencrypted `$TMPDIR` is a concern, or point `$TMPDIR` at protected storage. |
| Persistence / caching | **None.** State lives in process memory and is gone at exit. |
| Input | The PDF and FDF paths you pass (or the FDF on stdin with `fill -`); read-only. |
| Output | The filled PDF is written to **stdout**, or to a file of your choosing with `-o` (staged as `FILE.tmp`, renamed into place on success). |
| Logging | Progress/warnings go to **stderr**: field **names**, object numbers, counts, byte sizes. Filled **values are not logged** unless `FFPDF_VERBOSE=1` is set. |

Because processing is entirely local and the software author never receives your
data, **the author is not a Business Associate and no BAA is required** to use
the tool. (Contrast cloud/SaaS PDF tools, which do receive your data and do
require a BAA.)

## Handling of sensitive values (PHI/PII)

- Filled field values may be sensitive. They are **never written to disk** by
  the tool and are **not logged by default**. Keep `FFPDF_VERBOSE`
  unset in production.
- The original input is preserved: fills are appended as a PDF *incremental
  update*; existing bytes are not rewritten.

## Recommended deployment hardening (operator responsibilities)

The tool is one component; the surrounding environment matters:

1. **Send output to a protected location.** `... > /secure/path/filled.pdf`;
   apply your normal file permissions/encryption-at-rest to it.
2. **Disable core dumps** for the process (`ulimit -c 0`) so a crash cannot
   write process memory (which may hold PHI) to a core file.
3. **Avoid capturing stderr with values on.** Do not set
   `FFPDF_VERBOSE` when handling real data; if you capture stderr, note
   it still contains field *names*.
4. **Mind swap and shell history.** Consider encrypted swap; avoid putting PHI
   on the command line where it could land in shell history.
5. **Verify provenance.** Check the sigstore attestation
   (`gh attestation verify <file> -R pagebrooks/ffpdf`) and the SHA-256
   checksum, or rebuild from source (the build is reproducible).

## Security assurance

- **Open source (Apache-2.0)**: fully auditable.
- **Continuous fuzzing** (ASan + UBSan) over read, fill, and encrypted paths.
- **Adversarial security audits** with regression tests for fixed issues.
- **Hardened compilation** (stack canaries, FORTIFY, PIE, RELRO) and bounded
  decompression (anti–zip-bomb).
- **Cross-platform CI** builds and tests on Linux, macOS, and Windows, plus a
  dedicated ASan/UBSan job that runs the suite and a fuzz batch on every push.
- **Signed provenance (sigstore):** every published binary carries a keyless
  GitHub build-provenance attestation proving it was built by this repo's CI at
  a specific commit, plus an SBOM attestation binding a per-platform CycloneDX
  SBOM (generated at build time from [`sbom.json`](../sbom.json), stamped with
  the exact zlib version linked, the only dependency). Verify with
  `gh attestation verify <file> -R pagebrooks/ffpdf`. SHA-256 checksums ship
  alongside each binary.

## Known limitations (disclose to users)

- **Encryption:** only the empty user password is supported. PDFs requiring a
  real password will not open (this is not a password cracker).
- **Dynamic XFA forms** (some government forms) keep fields outside the AcroForm
  tree; field extraction finds nothing for those.
- **Implementation language is C.** The mitigations above (fuzzing, ASan/UBSan,
  audits, hardening) are how memory-safety risk is managed; they reduce but do
  not eliminate the inherent risk of a memory-unsafe language.

## Vulnerability reporting

See [`SECURITY.md`](../SECURITY.md). Report privately to **pb@0x00f.foo**.
