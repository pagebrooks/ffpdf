#!/bin/sh
# ffpdf installer.
#
#   curl -fsSL https://raw.githubusercontent.com/pagebrooks/ffpdf/main/install.sh | sh
#
# Downloads the release archive for your OS/CPU from GitHub, verifies it against
# the release's checksums.txt, and installs the `ffpdf` binary (plus its man
# page, best effort) into a bin directory on your PATH.
#
# No root required by default: installs into ~/.local/bin. Override with the
# environment variables or flags below.
#
# Environment variables (all optional):
#   FFPDF_VERSION       release to install: a tag like v0.1.1, or "latest"
#                       (default: latest)
#   FFPDF_INSTALL_DIR   directory to install the binary into
#                       (default: ~/.local/bin)
#   FFPDF_REPO          owner/name of the GitHub repo to pull from
#                       (default: pagebrooks/ffpdf)
#
# Flags (override the environment):
#   --version <tag>     same as FFPDF_VERSION
#   --dir <path>        same as FFPDF_INSTALL_DIR
#   -h, --help          show this help and exit
#
# The download is verified with sha256 before anything is installed. For an
# even stronger check, each archive also carries a sigstore attestation:
#   gh attestation verify <archive> -R pagebrooks/ffpdf

set -eu

# ---- defaults (env overrides these; flags override env) ----------------------
VERSION="${FFPDF_VERSION:-latest}"
INSTALL_DIR="${FFPDF_INSTALL_DIR:-$HOME/.local/bin}"
REPO="${FFPDF_REPO:-pagebrooks/ffpdf}"

# ---- helpers -----------------------------------------------------------------
info()  { printf '%s\n' "$*"; }
warn()  { printf 'warning: %s\n' "$*" >&2; }
die()   { printf 'error: %s\n' "$*" >&2; exit 1; }

usage() {
  # Print the leading comment block (the lines above) as help text.
  sed -n '2,/^set -eu/{/^set -eu/d;s/^# \{0,1\}//;p;}' "$0"
}

# Download URL -> file. Prefers curl, falls back to wget.
download() {
  # $1 = url, $2 = output path
  if command -v curl >/dev/null 2>&1; then
    curl -fSL --proto '=https' --tlsv1.2 -o "$2" "$1"
  elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$2" "$1"
  else
    die "need curl or wget to download files"
  fi
}

# Verify $1 (a bare filename in the current dir) against checksums.txt in the
# current dir. Fails closed: a missing line is an error, not a silent pass.
verify_sha256() {
  asset="$1"
  line=$(grep "  ${asset}\$" checksums.txt) \
    || die "no checksum for ${asset} in checksums.txt"
  if command -v sha256sum >/dev/null 2>&1; then
    printf '%s\n' "$line" | sha256sum -c - >/dev/null
  elif command -v shasum >/dev/null 2>&1; then
    printf '%s\n' "$line" | shasum -a 256 -c - >/dev/null
  else
    die "need sha256sum or shasum to verify the download"
  fi
}

# ---- parse flags -------------------------------------------------------------
while [ $# -gt 0 ]; do
  case "$1" in
    --version) [ $# -ge 2 ] || die "--version needs an argument"; VERSION="$2"; shift 2 ;;
    --version=*) VERSION="${1#*=}"; shift ;;
    --dir)     [ $# -ge 2 ] || die "--dir needs an argument"; INSTALL_DIR="$2"; shift 2 ;;
    --dir=*)   INSTALL_DIR="${1#*=}"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1 (try --help)" ;;
  esac
done

# ---- detect platform and pick the release asset ------------------------------
os=$(uname -s)
arch=$(uname -m)

case "$os" in
  Linux)
    case "$arch" in
      x86_64|amd64) asset="ffpdf-linux-x86_64.tar.gz" ;;
      aarch64|arm64)
        die "no prebuilt Linux arm64 binary yet. Build from source:
  git clone https://github.com/${REPO}.git && cd ffpdf && make && sudo make install" ;;
      *) die "unsupported Linux architecture: ${arch}" ;;
    esac
    ;;
  Darwin)
    # Universal2 archive runs natively on both Apple Silicon and Intel.
    asset="ffpdf-macos-universal2.tar.gz"
    ;;
  MINGW*|MSYS*|CYGWIN*|Windows_NT)
    die "this script targets Unix shells. On Windows, download the .zip from:
  https://github.com/${REPO}/releases" ;;
  *)
    die "unsupported operating system: ${os}" ;;
esac

# ---- resolve the download base URL -------------------------------------------
if [ "$VERSION" = "latest" ]; then
  base="https://github.com/${REPO}/releases/latest/download"
else
  base="https://github.com/${REPO}/releases/download/${VERSION}"
fi

# ---- download, verify, unpack in a scratch dir -------------------------------
tmp=$(mktemp -d 2>/dev/null || mktemp -d -t ffpdf) \
  || die "could not create a temporary directory"
trap 'rm -rf "$tmp"' EXIT INT TERM

info "Installing ffpdf (${VERSION}) for ${os}/${arch}..."

download "${base}/${asset}"       "${tmp}/${asset}"
download "${base}/checksums.txt"  "${tmp}/checksums.txt"

( cd "$tmp" && verify_sha256 "$asset" ) \
  || die "checksum verification failed for ${asset}"
info "Checksum OK."

tar -xzf "${tmp}/${asset}" -C "$tmp" \
  || die "could not extract ${asset}"
[ -f "${tmp}/ffpdf" ] || die "archive did not contain an 'ffpdf' binary"

# ---- install the binary ------------------------------------------------------
mkdir -p "$INSTALL_DIR" || die "could not create ${INSTALL_DIR}"
install -m 0755 "${tmp}/ffpdf" "${INSTALL_DIR}/ffpdf" 2>/dev/null \
  || { cp "${tmp}/ffpdf" "${INSTALL_DIR}/ffpdf" && chmod 0755 "${INSTALL_DIR}/ffpdf"; } \
  || die "could not install to ${INSTALL_DIR} (try: sudo FFPDF_INSTALL_DIR=/usr/local/bin sh install.sh)"

# ---- best-effort man page (mirrors the prefix layout of `make install`) ------
# If installing into <prefix>/bin, put ffpdf.1 in <prefix>/share/man/man1.
if [ -f "${tmp}/ffpdf.1" ]; then
  case "$INSTALL_DIR" in
    */bin)
      mandir="${INSTALL_DIR%/bin}/share/man/man1"
      if mkdir -p "$mandir" 2>/dev/null && cp "${tmp}/ffpdf.1" "$mandir/ffpdf.1" 2>/dev/null; then
        info "Installed man page to ${mandir}/ffpdf.1"
      fi
      ;;
  esac
fi

info "Installed ffpdf to ${INSTALL_DIR}/ffpdf"

# ---- PATH nudge (don't edit the user's rc files silently) --------------------
case ":${PATH}:" in
  *":${INSTALL_DIR}:"*) : ;;
  *)
    info ""
    info "${INSTALL_DIR} is not on your PATH. Add it, e.g.:"
    info "    export PATH=\"${INSTALL_DIR}:\$PATH\""
    ;;
esac

# ---- show what we installed --------------------------------------------------
"${INSTALL_DIR}/ffpdf" --version 2>/dev/null | head -n 1 || true
