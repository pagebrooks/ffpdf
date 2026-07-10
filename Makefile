CC = gcc
# gnu99 (not c99) so POSIX libc functions the code relies on -- strdup, and
# fmemopen in the test/fuzz harness -- are declared on glibc, macOS, and
# MinGW-w64 alike without per-file feature-test macros.
CFLAGS = -Wall -Wextra -std=gnu99 -O2
LDFLAGS = -lz

# ---- Platform / toolchain detection ----
# Linux and macOS (Darwin) build out of the box. For Windows, either build in an
# MSYS2/MinGW shell or cross-compile from Linux with:
#     make CC=x86_64-w64-mingw32-gcc
# (needs a MinGW zlib: pacman -S mingw-w64-x86_64-zlib, or vcpkg, or vendored).
UNAME_S := $(shell uname -s 2>/dev/null)
WINDOWS :=
ifneq (,$(findstring mingw,$(CC))$(findstring MINGW,$(UNAME_S))$(findstring MSYS,$(UNAME_S))$(findstring CYGWIN,$(UNAME_S)))
  WINDOWS := 1
endif
EXEEXT :=
ifdef WINDOWS
  EXEEXT := .exe
  LDFLAGS += -lshell32          # CommandLineToArgvW (UTF-8 argv)
endif

# STATIC=1 links a self-contained binary (no libgcc/winpthread/zlib DLLs) --
# the recommended way to build a drop-in ffpdf.exe for Windows.
ifdef STATIC
  LDFLAGS += -static
endif

# WERROR=1 turns warnings into errors. Used in CI to keep the build at zero
# warnings; off by default so local builds with other compilers aren't punished.
ifdef WERROR
  CFLAGS += -Werror
endif

# Security hardening (defense in depth for a tool that processes sensitive PDFs).
# Applied per-platform so the flags don't break macOS ld64 or the MinGW PE link.
# Disable with HARDEN=0.
HARDEN ?= 1
ifeq ($(HARDEN),1)
  CFLAGS += -fstack-protector-strong
  ifdef WINDOWS
    LDFLAGS += -Wl,--dynamicbase,--nxcompat            # PE ASLR + DEP
  else
    CFLAGS += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -fPIE
    ifeq ($(UNAME_S),Darwin)
      LDFLAGS += -Wl,-pie
    else
      LDFLAGS += -pie -Wl,-z,relro,-z,now              # full RELRO + PIE
    endif
  endif
endif

# Single source of truth for the version: PROG_VERSION in main.c. The SBOM is
# stamped with it at build time (make sbom); ffpdf.1 and the sbom.json template
# are rewritten by `make bump NEW=x.y.z`, and check-version fails the build if
# the man page drifts.
VERSION := $(shell awk -F'"' '/define PROG_VERSION/{print $$2; exit}' main.c)

TARGET = ffpdf$(EXEEXT)
SOURCES = main.c utils.c pdf_lex.c pdf_parser.c form_extractor.c field_map.c xfa.c pdf_filler.c crypto.c os_compat.c
OBJECTS = $(SOURCES:.c=.o)
HEADERS = utils.h pdf_lex.h pdf_parser.h form_extractor.h field_map.h xfa.h pdf_filler.h crypto.h os_compat.h

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- Unit tests ----
UNIT_TEST = test_field_map$(EXEEXT)
# test_field_map.c #includes field_map.c, so link the *other* objects it needs
# (parse_obj_at_offset, decompress_flate, ...) but NOT field_map.o.
UNIT_TEST_DEPS = utils.o pdf_lex.o pdf_parser.o form_extractor.o xfa.o crypto.o

$(UNIT_TEST): test_field_map.c field_map.c $(HEADERS) $(UNIT_TEST_DEPS)
	$(CC) $(CFLAGS) test_field_map.c $(UNIT_TEST_DEPS) -o $(UNIT_TEST) $(LDFLAGS)

# Run the fast, PDF-free unit tests.
test: $(UNIT_TEST)
	./$(UNIT_TEST)

# End-to-end smoke test: drives the built binary (extract/fill round-trip).
test-e2e: $(TARGET)
	./test_e2e.sh

# The man page and changelog must carry the same version as main.c (the SBOM
# is stamped at build time, so these are the only copies that can drift).
check-version:
	@grep -q '"ffpdf $(VERSION)"' ffpdf.1 || \
	  { echo "ffpdf.1 version does not match main.c ($(VERSION)); run: make bump NEW=$(VERSION)"; exit 1; }
	@grep -q '^## \[$(VERSION)\]' CHANGELOG.md || \
	  { echo "CHANGELOG.md has no entry for $(VERSION); add a '## [$(VERSION)]' section"; exit 1; }

# Everything: version consistency, unit tests, then the end-to-end smoke test.
check: check-version test test-e2e

# One-command version bump: rewrites main.c, the ffpdf.1 header (version +
# man-page date), and the sbom.json template.
bump:
	@test -n "$(NEW)" || { echo "usage: make bump NEW=x.y.z"; exit 1; }
	@sed -i.bak 's/^#define PROG_VERSION ".*"/#define PROG_VERSION "$(NEW)"/' main.c && rm -f main.c.bak
	@sed -i.bak 's/^\.TH FFPDF 1 "[^"]*" "ffpdf [^"]*"/.TH FFPDF 1 "'"$$(date '+%B %Y')"'" "ffpdf $(NEW)"/' ffpdf.1 && rm -f ffpdf.1.bak
	@sed -i.bak 's/"version": "[^"]*"/"version": "$(NEW)"/' sbom.json && rm -f sbom.json.bak
	@echo "bumped to $(NEW): main.c, ffpdf.1, sbom.json"


# ---- Sanitizers (ASan+UBSan) ----
SANFLAGS = -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
           -D_POSIX_C_SOURCE=200809L

# Instrumented build of the CLI itself (overwrites ./ffpdf). CI's sanitizers
# job runs the e2e suite against this, so the source list lives only here.
asan: $(SOURCES) $(HEADERS)
	$(CC) -std=gnu99 $(SANFLAGS) $(SOURCES) -o $(TARGET) -lz

# ---- Fuzzing (PDF read path, ASan+UBSan) ----
FUZZ_SRC = $(filter-out main.c,$(SOURCES))
fuzz_harness: fuzz_harness.c $(FUZZ_SRC) $(HEADERS)
	$(CC) -std=c99 $(SANFLAGS) fuzz_harness.c $(FUZZ_SRC) -o $@ $(LDFLAGS)

# Build the harness and drive it with mutated inputs.
fuzz: fuzz_harness
	./fuzz.sh

# ---- SBOM ----
# Stamp sbom.json (the template) with the zlib version this build actually
# links: the probe is compiled with the same CC/CFLAGS/LDFLAGS as the binary,
# so zlibVersion() reports the linked library, not whatever header is lying
# around. Output name is overridable for per-platform SBOMs in CI.
SBOM_OUT ?= sbom.gen.json
sbom:
	@printf '#include <zlib.h>\n#include <stdio.h>\nint main(void){printf("%%s",zlibVersion());return 0;}\n' > .zlibver.c
	@$(CC) $(CFLAGS) .zlibver.c -o .zlibver$(EXEEXT) $(LDFLAGS)
	@python3 -c 'import json,sys,uuid; d=json.load(open("sbom.json")); d["serialNumber"]="urn:uuid:"+str(uuid.uuid5(uuid.NAMESPACE_URL, "ffpdf-"+sys.argv[1]+"-zlib-"+sys.argv[2])); d["version"]=1; d["metadata"]["component"]["version"]=sys.argv[1]; [c.update(version=sys.argv[2], purl="pkg:generic/zlib@"+sys.argv[2]) for c in d["components"] if c["name"]=="zlib"]; json.dump(d, open(sys.argv[3],"w"), indent=2)' "$(VERSION)" "$$(./.zlibver$(EXEEXT))" $(SBOM_OUT)
	@rm -f .zlibver.c .zlibver$(EXEEXT)
	@echo "wrote $(SBOM_OUT) (zlib $$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["components"][0]["version"])' $(SBOM_OUT)))"

# ---- README demo GIF ----
# Regenerates docs/demo.gif when the tape, the demo FDF, or the binary's
# output changes. Opt-in (`make demo`), not part of all/check: it needs vhs +
# ttyd + ffmpeg (https://github.com/charmbracelet/vhs), which most build
# environments won't have.
demo: docs/demo.gif docs/demo-batch.gif

# The demo form itself is generated; the checked-in PDF is the build product
# of this rule, regenerated when the script changes.
docs/demo-form.pdf: docs/gen-demo-form.py
	python3 docs/gen-demo-form.py $@

docs/demo.gif: docs/demo.tape docs/demo-form.pdf $(TARGET)
	@command -v vhs >/dev/null || \
	  { echo "vhs not found -- install it (and ttyd + ffmpeg): https://github.com/charmbracelet/vhs"; exit 1; }
	vhs docs/demo.tape

docs/demo-batch.gif: docs/demo-batch.tape docs/demo-template.fdf docs/demo-customers.csv docs/demo-form.pdf $(TARGET)
	@command -v vhs >/dev/null || \
	  { echo "vhs not found -- install it (and ttyd + ffmpeg): https://github.com/charmbracelet/vhs"; exit 1; }
	vhs docs/demo-batch.tape

# ---- README example form ----
# The worked example linked from the README: a styled fillable form, the FDF
# with the answers, and the filled result produced by ffpdf itself. All three
# are committed; this regenerates them when the generator or answers change.
examples: docs/example-form.pdf docs/example-filled.pdf docs/example-flattened.pdf \
          docs/example-form.png docs/example-filled.png

# PNG renders embedded in the README so the before/after is visible without
# GitHub's PDF viewer (which some browser privacy settings block).
docs/example-form.png: docs/example-form.pdf
	@command -v pdftoppm >/dev/null || \
	  { echo "pdftoppm not found -- install poppler-utils"; exit 1; }
	pdftoppm -png -r 110 -singlefile docs/example-form.pdf docs/example-form

docs/example-filled.png: docs/example-filled.pdf
	@command -v pdftoppm >/dev/null || \
	  { echo "pdftoppm not found -- install poppler-utils"; exit 1; }
	pdftoppm -png -r 110 -singlefile docs/example-filled.pdf docs/example-filled

docs/example-form.pdf: docs/gen-example-form.py
	python3 docs/gen-example-form.py $@

docs/example-filled.pdf: docs/example-form.pdf docs/example-answers.fdf $(TARGET)
	./$(TARGET) fill -o $@ docs/example-answers.fdf docs/example-form.pdf

docs/example-flattened.pdf: docs/example-form.pdf docs/example-answers.fdf $(TARGET)
	./$(TARGET) fill --flatten -o $@ docs/example-answers.fdf docs/example-form.pdf

# ---- Install ----
PREFIX  ?= /usr/local
BINDIR   = $(DESTDIR)$(PREFIX)/bin
MANDIR   = $(DESTDIR)$(PREFIX)/share/man/man1
MANPAGE  = $(TARGET).1

install: $(TARGET)
	install -d $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)/$(TARGET)
ifndef WINDOWS
	install -d $(MANDIR)
	install -m 644 $(MANPAGE) $(MANDIR)/$(MANPAGE)
endif

uninstall:
	rm -f $(BINDIR)/$(TARGET) $(MANDIR)/$(MANPAGE)

clean:
	rm -f $(OBJECTS) $(TARGET) $(UNIT_TEST) fuzz_harness

rebuild: clean all

.PHONY: all clean test test-e2e check check-version rebuild asan fuzz sbom bump demo examples install uninstall
