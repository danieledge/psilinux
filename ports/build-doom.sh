#!/bin/bash
# Builds Doom for PsiLinux with the PsiLinux toolchain: doomgeneric (pinned
# below) with a PsiLinux backend (ports/doom/i_psilinux.c: /dev/fb0 in 8-bit
# palette mode, /dev/input's keyboard and pen), and the shareware doom1.wad,
# into build/psilinux-root/doom/, laid out as PsiLinux-Doom.sis installs it
# (!:\Linux\Doom\): doom (the launcher), doomgeneric, doom1.wad, licences.txt.
#   tools/lxbuild "ports/build-doom.sh"
# Then, for the .sis: core/tools/docker/psibuild "bash build.sh sis"
# (or makesis pkg/psilinux-doom.pkg in that folder by hand).
#
# Every download is pinned by sha256 and cached in $LXSRC. The WAD is never
# in the repository (.gitignore): it is id Software's shareware episode,
# which may be passed on only unmodified, and is fetched here.
#
# Running it: in PsiLinux, sh /d/Linux/Doom/doom (or /c/...). Options besides
# Doom's own: -letterbox, -tone N (0-4, the grey curve, default 2), -fps,
# -frames N, -dumptic N FILE, -fbfile F (see ports/doom/i_psilinux.c).
# Under qemu-arm, with no Psion (-cpu arm926, not ti925t: qemu's own vDSO is
# Thumb code that cannot return to ARMv4T; the program itself is ARMv4T):
#   qemu-arm -cpu arm926 build/psilinux-root/doom/doomgeneric -iwad .../doom1.wad -fbfile fb.raw -timedemo demo1
# or through the host harness, which writes what the Psion would show:
#   LX_DRIVE_D=DIR PSIHOST_FB=shot.pgm qemu-arm -cpu ti925t build/linux/psihost -r ROOT /bin/sh /d/Linux/Doom/doom -warp 1 1
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
WORK=${PSION_WORK:-$(cd "$REPO/.." && pwd)}
LXTC=${LXTC:-$WORK/psilinux-toolchain}
LXSRC=${LXSRC:-$WORK/psilinux-src}
export PATH=$LXTC/bin:$PATH
T=arm-linux-musleabi
OUT=$REPO/build/psilinux-root/doom      # what the package holds
B=$LXSRC/doom-build
mkdir -p "$OUT" "$B"

DG_COMMIT=e257a09527062bfb986b6e5e0780839b6cbd5b71     # ozkl/doomgeneric master, 11 Oct 2026
DG_SHA=26f8f60daa6c6e4d3c01e5eac9fb700d592dede803d13ac848154ff356b2a2e7
# doom1.wad, the shareware v1.9 (4,196,020 bytes, md5 f0cefca49926d00903cf57551d901abe)
WAD_SHA=1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771
WAD_URLS="https://raw.githubusercontent.com/Akbar30Bill/DOOM_wads/master/doom1.wad
https://github.com/nneonneo/universal-doom/raw/main/DOOM1.WAD"

fetch() {    # name sha256 url... -> the file's path (the first URL that gives the right file)
	local f=$LXSRC/$1 sum=$2 u; shift 2
	for u in "$@"; do
		echo "$sum  $f" | sha256sum -c --quiet > /dev/null 2>&1 && break
		curl -sSfL --retry 3 -o "$f.part" "$u" && mv "$f.part" "$f" || rm -f "$f.part"
	done
	echo "$sum  $f" | sha256sum -c --quiet >&2
	echo "$f"
}
log() { echo "== $*"; }

log "doomgeneric ${DG_COMMIT:0:7}"
tgz=$(fetch doomgeneric-$DG_COMMIT.tar.gz $DG_SHA https://github.com/ozkl/doomgeneric/archive/$DG_COMMIT.tar.gz)
S=$B/doomgeneric-$DG_COMMIT
rm -rf "$S"
tar xf "$tgz" -C "$B"
patch -d "$S" -p1 < "$HERE/patches/doomgeneric-e257a09-psilinux.diff" > "$B/patch.log"

log "doom1.wad (shareware 1.9)"
wad=$(fetch doom1.wad $WAD_SHA $WAD_URLS)

# Doom's sources: the Makefile's list, less i_video.c and the X11 backend
# (i_psilinux.c is both), and no sound (no FEATURE_SOUND: i_sound.c's stubs)
SRCS="dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net
	f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale i_sound
	i_system i_timer memio m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc
	m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj p_plats
	p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw
	r_main r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound tables
	v_video wi_stuff w_checksum w_file w_main w_wad z_zone w_file_stdc i_input doomgeneric"
# -O2 in ARM mode (lx-cc: -march=armv4t -marm, soft float); Doom is fixed
# point throughout, so no floating point in the frame (checked below)
CFLAGS="-O2 -fomit-frame-pointer -ffunction-sections -fdata-sections -ffile-prefix-map=$B=/src
	-DPSILINUX -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE -w"
O=$B/obj
rm -rf "$O"; mkdir -p "$O"
log "compiling"
pids=
for s in $SRCS; do
	lx-cc $CFLAGS -I"$S/doomgeneric" -c "$S/doomgeneric/$s.c" -o "$O/$s.o" 2>> "$B/cc.log" &
	pids="$pids $!"
	[ $(jobs -r | wc -l) -ge $(nproc) ] && wait -n
done
lx-cc $CFLAGS -Wall -I"$S/doomgeneric" -c "$HERE/doom/i_psilinux.c" -o "$O/i_psilinux.o"
lx-cc -c "$HERE/doom/psi_blit.S" -o "$O/psi_blit.o"
for p in $pids; do wait $p || { tail -20 "$B/cc.log"; exit 1; }; done
lx-cc -Wl,--gc-sections -o "$B/doomgeneric" "$O"/*.o -Wl,-Map,"$B/doomgeneric.map"

# no soft-float calls in the renderer or the game's tic (V_DrawMouseSpeedBox,
# the mouse test, is never called: there is no mouse)
bad=$($T-objdump -d "$B/doomgeneric" | awk '/^[0-9a-f]+ <.*>:$/ { f = $2 }
	/bl.*<__aeabi_[fd](add|sub|mul|div|cmp)|bl.*<__(add|sub|mul|div)[sd]f3>/ { print f }' | sort -u \
	| grep -E '<(R_|P_|G_Ticker|D_Display|V_|I_FinishUpdate|FixedMul|FixedDiv)' | grep -v V_DrawMouseSpeedBox || true)
[ -z "$bad" ] || { echo "floating point in the frame: $bad"; exit 1; }

$T-strip -o "$OUT/doomgeneric" "$B/doomgeneric"
install -m 755 "$REPO/pkg/doom/doom" "$OUT/doom"
cp "$wad" "$OUT/doom1.wad"
{
	printf '==== doomgeneric (https://github.com/ozkl/doomgeneric, %s)\n\n' "$DG_COMMIT"
	printf 'Doom (C) 1993-1996 id Software, Inc.; Chocolate Doom (C) 2005-2014 Simon Howard;\n'
	printf 'doomgeneric by ozkl; the PsiLinux backend, ports/doom/i_psilinux.c. GNU GPL version 2:\n\n'
	cat "$S/LICENSE"
	printf '\n==== doom1.wad\n\n'
	printf 'DOOM shareware episode 1, version 1.9, (C) 1993 id Software, Inc. The shareware\n'
	printf 'episode may be copied and given away only unmodified, and not sold. The full game\n'
	printf 'is sold by id Software (Bethesda).\n'
} > "$OUT/licences.txt"

$T-size "$B/doomgeneric" | tail -1 | awk '{ printf "   text %d, data %d, bss %d bytes\n", $1, $2, $3 }'
ls -la "$OUT" | awk 'NR > 1 && $9 !~ /^\.\.?$/ { print "   " $5 " bytes " $9 }'
