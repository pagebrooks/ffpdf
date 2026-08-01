class Ffpdf < Formula
  desc "Fast PDF form-field extractor and filler"
  homepage "https://github.com/pagebrooks/ffpdf"
  url "https://github.com/pagebrooks/ffpdf/archive/refs/tags/v0.1.1.tar.gz"
  sha256 "a1c5503d9a53450bb1c0f9f4f332b4f4152bcb9c1cd0b2ec6f7b6b387d0bdd97"
  license "Apache-2.0"
  head "https://github.com/pagebrooks/ffpdf.git", branch: "main"

  # zlib is the only runtime dependency. Use the system copy on macOS; pull the
  # Homebrew formula on Linux, where it isn't guaranteed.
  uses_from_macos "zlib"

  def install
    # The Makefile builds a native, hardened binary and installs the binary plus
    # the ffpdf(1) man page under PREFIX. CC is passed explicitly so the build
    # uses Homebrew's compiler rather than the Makefile's default `gcc`.
    system "make", "install", "CC=#{ENV.cc}", "PREFIX=#{prefix}"
  end

  test do
    assert_match "ffpdf #{version}", shell_output("#{bin}/ffpdf --version")
  end
end
