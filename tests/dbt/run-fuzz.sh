#!/bin/bash
# run-fuzz.sh - the translator's differential tests (fuzz.py): each seed's
# program runs natively (qemu-arm -cpu arm1176, an ARMv6KZ with VFPv2) and
# through psihost (qemu-arm -cpu ti925t, an ARMv4T as the 5mx); the outputs
# must be the same, byte for byte.
#   linux/tools/lxbuild "linux/tests/dbt/run-fuzz.sh [FIRST_SEED [COUNT [CASES [KINDS]]]]"
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
O=$REPO/build/linux/fuzz
mkdir -p "$O" "$O/root"
make -C "$REPO/linux/runtime" host > /dev/null
[ -f "$O/root/ROOT.IMG" ] || python3 "$REPO/linux/tools/mkroot.py" "$O/root" "$O/root/ROOT.IMG" > /dev/null 2>&1 || true
first=${1:-1}; count=${2:-10}; cases=${3:-300}; only=${4:+--only $4}
fail=0
for seed in $(seq $first $((first + count - 1))); do
	python3 "$HERE/fuzz.py" --seed $seed --cases $cases $only "$O/f$seed.S"
	$LXTC/bin/arm-linux-musleabi-as --no-warn -o "$O/f$seed.o" "$O/f$seed.S"
	$LXTC/bin/arm-linux-musleabi-ld -pie --no-dynamic-linker -e _start -o "$O/f$seed" "$O/f$seed.o"
	rm -f "$O/f$seed.native" "$O/f$seed.dbt"
	qemu-arm -cpu arm1176 "$O/f$seed" "$O/f$seed.native"
	LX_DRIVE_T=$O timeout 600 qemu-arm -cpu ti925t "$REPO/build/linux/psihost" -r "$O/root" /t/f$seed /t/f$seed.dbt > "$O/f$seed.out" 2> "$O/f$seed.err" < /dev/null || true
	touch "$O/f$seed.dbt"
	if cmp -s "$O/f$seed.native" "$O/f$seed.dbt"; then echo "seed $seed: same ($(stat -c %s "$O/f$seed.native") bytes)"
	else echo "seed $seed: DIFFERENT"; python3 "$HERE/fuzzdiff.py" "$O/f$seed.S" "$O/f$seed.native" "$O/f$seed.dbt" | head -20; fail=1; fi
done
exit $fail
