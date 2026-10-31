#!/bin/bash
# Runs the screenshots in the psilinux-build container (as tools/lxbuild
# does), with a small made-up Psion at /c and /d for Files, and a made-up
# /proc for System (PSIWM_PROC; under qemu-arm /proc is the host's).
#   ports/psiwm/test/run.sh [NAME...]     -> build/psiwm-shots/NAME.png
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
WORK=${PSION_WORK:-$(cd "$REPO/.." && pwd)}
FR=$REPO/build/psiwm/test/root
rm -rf "$FR"
mkdir -p "$FR/c/Documents" "$FR/c/System/Apps" "$FR/c/Linux/root" "$FR/d/Install" "$FR/d/Music" "$FR/d/Pictures"
printf 'Shopping\n\n- milk\n- bread\n- AA batteries for the 5mx\n' > "$FR/c/Documents/Shopping.txt"
cp "$HERE/notes-sample.txt" "$FR/c/Documents/Plans.txt"
printf 'Dear Dan,\n\nThank you for the Psion.\n' > "$FR/c/Documents/Letter to Dan.txt"
head -c 48213 /dev/zero > "$FR/c/Documents/Holiday photo.jpg"
head -c 2340 /dev/zero > "$FR/c/Documents/Accounts.xls"
head -c 6100 /dev/zero > "$FR/c/Documents/Agenda"
head -c 182044 /dev/zero > "$FR/d/Install/PsiLinux.sis"
touch -d '2026-10-12 09:30' "$FR/c/Documents/Letter to Dan.txt"
touch -d '2026-09-03 18:02' "$FR/c/Documents/Holiday photo.jpg"
touch -d '2026-10-29 21:14' "$FR/c/Documents/Accounts.xls"
cp -r "$HERE/proc" "$FR/fakeproc"
mkdir -p "$FR/d/Linux/Doom"; printf '#!/bin/sh\necho not the real Doom\n' > "$FR/d/Linux/Doom/doom"; chmod +x "$FR/d/Linux/Doom/doom"
docker run --rm -i --user "$(id -u):$(id -g)" -v "$WORK:$WORK" -v "$FR/c:/c" -v "$FR/d:/d" -w "$REPO" \
	-e QEMUOPT="${QEMUOPT:-}" -e HOME="$WORK" -e TZ="${TZ:-GMT0}" psilinux-build bash -c "ports/psiwm/test/shots.sh $*"
