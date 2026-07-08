# Contributing to ffpdf

Thanks for your interest in improving ffpdf. This document covers how to build,
test, and submit changes.

## Building and testing

ffpdf is plain C with zlib as the only dependency.

```console
make            # build ./ffpdf
make check      # unit tests (test_field_map) + end-to-end tests (test_e2e.sh)
```

`make check` must pass before a change is merged. If you add behavior, add a
test for it: a unit check in `test_field_map.c` for logic, or an end-to-end
case in `test_e2e.sh` for observable CLI behavior. Please also run the
sanitizer build (`make asan` / the ASan-UBSan CI job) for parser or memory
changes.

## Coding style

Match the surrounding code: same naming, bracing, and comment density as the
file you are editing. Keep functions focused, prefer the existing dictionary and
buffer helpers over new ad-hoc parsing, and route diagnostics to **stderr**;
`stdout` carries command output (FDF/XFDF/JSON/PDF) and must not be polluted.

## Licensing of contributions

ffpdf is licensed under the **Apache License, Version 2.0**. Contributions are
accepted **inbound = outbound**: by submitting a change you agree it is provided
under the same Apache-2.0 license, per section 5 of that license. You retain
copyright to your contribution; there is no copyright assignment and no CLA.

## Developer Certificate of Origin (sign-off)

Every commit must be signed off to certify you have the right to submit it under
the project's license. This is the [Developer Certificate of Origin](DCO) (the
same mechanism the Linux kernel uses), not a copyright assignment.

Add a sign-off line to each commit:

```console
git commit -s -m "Your message"
```

which appends:

```
Signed-off-by: Your Name <you@example.com>
```

The name and email must be your real identity (no pseudonyms or noreply
addresses) and must match the commit's author name and email. By adding the
sign-off you certify the statements in the [`DCO`](DCO) file: that you wrote
the change or otherwise have the right to submit it under the project's
license.

### AI-assisted contributions

Contributions written with AI assistance (Claude, Copilot, etc.) are welcome
under the same terms as any other: **you** must have reviewed and understood
every line you submit, your DCO sign-off certifies the contribution regardless
of what tools helped produce it, and you remain responsible for its provenance
and quality. Please don't submit generated code you can't explain; a change
you can't reason about in review is a change we can't merge.

## Reporting security issues

Do **not** open a public issue for a vulnerability. Follow the private
disclosure process in [`SECURITY.md`](SECURITY.md).
