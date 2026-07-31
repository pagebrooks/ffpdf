# Homebrew tap for ffpdf

`ffpdf.rb` in this directory is the source of truth for the Homebrew formula. It
builds from the release source tarball, so a single formula works on Intel and
Apple Silicon macOS and on Homebrew-on-Linux, and it is the form
[homebrew-core](https://github.com/Homebrew/homebrew-core) accepts if the
formula is ever submitted upstream.

## One-time: create the tap repository

A Homebrew tap is just a GitHub repo whose name starts with `homebrew-`. The tap
is named **`homebrew-tap`** (rather than `homebrew-ffpdf`) so it can hold formulae
for future tools too; each formula is one file under `Formula/`.

1. Create a public repo named **`homebrew-tap`** under the `pagebrooks` account.
2. Add the formula at `Formula/ffpdf.rb` (copy it from here):

   ```console
   git clone https://github.com/pagebrooks/homebrew-tap.git
   mkdir -p homebrew-tap/Formula
   cp packaging/homebrew/ffpdf.rb homebrew-tap/Formula/ffpdf.rb
   cd homebrew-tap && git add Formula/ffpdf.rb && git commit -m "Add ffpdf 0.1.1" && git push
   ```

Users then install with either:

```console
brew install pagebrooks/tap/ffpdf
# or
brew tap pagebrooks/tap && brew install ffpdf
```

`brew install --HEAD pagebrooks/tap/ffpdf` builds from the tip of `main`.

## Test the formula before publishing

From a machine with Homebrew:

```console
brew install --build-from-source ./ffpdf.rb   # build + install locally
brew test ffpdf                               # runs the `test do` block
brew audit --strict --online ffpdf            # lint (use --new for a core submission)
```

## Updating the formula for a new release

On each new tagged release, bump two lines in `ffpdf.rb`: `url` (the new tag) and
`sha256` (of that tag's source tarball). Compute the checksum with:

```console
VER=0.2.0
curl -fsSL "https://github.com/pagebrooks/ffpdf/archive/refs/tags/v${VER}.tar.gz" | sha256sum
```

Then update `url`/`sha256` in the tap's `Formula/ffpdf.rb`, commit, and push.

Homebrew can also do this for you:

```console
brew bump-formula-pr --url "https://github.com/pagebrooks/ffpdf/archive/refs/tags/v${VER}.tar.gz" pagebrooks/tap/ffpdf
```

### Optional: automate the bump on release

Add a job to the main repo's release workflow that opens a PR (or pushes a
commit) to `homebrew-tap` whenever a `v*` tag is published. The community
action [`dawidd6/action-homebrew-bump-formula`](https://github.com/dawidd6/action-homebrew-bump-formula)
does exactly this; it needs a personal access token with write access to the tap
repo stored as a secret. Left as a follow-up so the tap can be published and
verified by hand first.

## Notes

- The formula's only dependency is `zlib`, declared with `uses_from_macos` so the
  system copy is used on macOS and the Homebrew formula on Linux.
- The build uses `CC=#{ENV.cc}` so Homebrew's compiler is used rather than the
  Makefile's default `gcc`.
- Keep this copy and the tap's `Formula/ffpdf.rb` in sync; this directory is the
  canonical version.
