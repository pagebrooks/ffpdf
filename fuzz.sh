#!/bin/bash
# Mutation fuzzer for the PDF read path.
#
# Builds the sanitizer-instrumented harness (`make fuzz_harness`), then feeds it
# thousands of mutated copies of the test PDFs and watches for crashes (ASan /
# UBSan aborts) and hangs (timeouts). Findings are saved under fuzz-findings/.
#
#   ./fuzz.sh [iterations]     # default 5000
#
# Requires python3. No coverage guidance (no libFuzzer/AFL here), but ASan+UBSan
# over many mutated real PDFs still surfaces memory bugs, UB and infinite loops.
set -u
cd "$(dirname "$0")" || exit 1

ITERS="${1:-5000}"
OUT=fuzz-findings
mkdir -p "$OUT"

command -v python3 >/dev/null || { echo "python3 required"; exit 2; }
make fuzz_harness >/dev/null || exit 2

ASAN_OPTIONS="abort_on_error=1:halt_on_error=1:detect_leaks=0" \
UBSAN_OPTIONS="halt_on_error=1:abort_on_error=1" \
python3 - "$ITERS" "$OUT" <<'PY'
import os, sys, random, subprocess, glob, time
iters, out = int(sys.argv[1]), sys.argv[2]
seeds = [open(p, "rb").read() for p in sorted(glob.glob("test/pdfs/*.pdf"))]
if not seeds: print("no seeds in test/pdfs"); sys.exit(2)

# Add encrypted variants (RC4 / AES-128 / AES-256, empty password) as seeds so
# mutations exercise crypto.c. Best-effort: skipped if pikepdf is unavailable.
try:
    import io, pikepdf
    base = pikepdf.open(io.BytesIO(seeds[0]))
    variants = [dict(R=4, aes=False, metadata=False),  # RC4-128
                dict(R=4, aes=True),                    # AES-128 (AESV2)
                dict(R=6, aes=True)]                    # AES-256 (AESV3)
    n0 = len(seeds)
    for v in variants:
        b = io.BytesIO()
        base.save(b, encryption=pikepdf.Encryption(user="", owner="", **v))
        seeds.append(b.getvalue())
    print(f"seeds: {len(seeds)} (incl. {len(seeds)-n0} encrypted)")
except Exception as e:
    print(f"seeds: {len(seeds)} (no encrypted variants: {e})")
cap = max(len(s) for s in seeds) * 2 + 4096
rng = random.Random()
def mutate():
    b = bytearray(rng.choice(seeds))
    for _ in range(rng.randint(1, 8)):
        if not b: b = bytearray(b"%PDF-1.7\n")
        op = rng.randrange(8)
        if op == 0: i = rng.randrange(len(b)); b[i] ^= 1 << rng.randrange(8)
        elif op == 1: i = rng.randrange(len(b)); b[i] = rng.randrange(256)
        elif op == 2: i = rng.randrange(len(b)+1); b[i:i] = bytes(rng.randrange(256) for _ in range(rng.randint(1,16)))
        elif op == 3: i = rng.randrange(len(b)); del b[i:i+rng.randint(1,32)]
        elif op == 4: i = rng.randrange(len(b)); b[i:i] = b[i:i+rng.randint(1,64)]
        elif op == 5: i = rng.randrange(len(b)); j = i+rng.randint(1,64); b[i:j] = bytes([rng.choice((0,255))])*(j-i)
        elif op == 6: b = b[:rng.randrange(len(b)+1)]
        else:
            o = rng.choice(seeds); oi = rng.randrange(len(o))
            i = rng.randrange(len(b)+1); b[i:i] = o[oi:oi+rng.randint(1,128)]
    return bytes(b[:cap])
tmp = os.path.join(out, ".input")
crashes = hangs = 0; t0 = time.time()
for it in range(iters):
    data = mutate(); open(tmp, "wb").write(data)
    try:
        r = subprocess.run(["./fuzz_harness", tmp], capture_output=True, timeout=10)
    except subprocess.TimeoutExpired:
        hangs += 1; open(f"{out}/hang_{it}.pdf","wb").write(data); print(f"[{it}] HANG"); continue
    if r.returncode != 0:
        crashes += 1; open(f"{out}/crash_{it}.pdf","wb").write(data)
        print(f"[{it}] CRASH rc={r.returncode}")
        for l in r.stderr.decode("latin1","replace").splitlines():
            if any(k in l for k in ("ERROR","runtime error","SUMMARY","#0","#1")): print("   ", l[:160])
    if it and it % 2000 == 0: print(f"  ...{it} iters, {crashes} crashes, {hangs} hangs, {it/(time.time()-t0):.0f}/s")
print(f"\nDONE: {iters} iters, {crashes} crashes, {hangs} hangs in {time.time()-t0:.1f}s")
sys.exit(1 if (crashes or hangs) else 0)
PY
