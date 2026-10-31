#!/bin/bash
# All of psiwm's screenshots (build/psiwm-shots/): tools/lxbuild ports/psiwm/test/shots.sh [NAME...]
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
T=$REPO/build/psiwm/test
S=$HERE/shot.sh
D="shot $T/done"
declare -A SC
SC[desk]="key Right;key Right;key Left;wait 300;$D-desk"
SC[files]="run 0;wait 400;key Down;key Return;wait 400;key Down;key Down;key Down;wait 300;$D-files"
SC[filesmenu]="run 0;wait 400;key Down;key Return;wait 400;key Down;key Down;key c c;wait 200;menu 0;wait 400;$D-filesmenu"
SC[notes]="open /c/Documents/Plans.txt;wait 600;key Down;key Down;key Down;key Down;key Down;key Down;key End;type _(and_Wi-Fi_drivers);wait 300;$D-notes"
SC[notesmenu]="open /c/Documents/Plans.txt;wait 600;key Down;key Down;key End;key Up s;wait 200;menu 1;key Down;wait 300;$D-notesmenu"
SC[calc]="run 0;wait 400;key Down;key Return;wait 300;run 2;wait 300;type 1234.5;key asterisk s;type 12;key Return;wait 300;$D-calc"
SC[clock]="open /c/Documents/Plans.txt;wait 500;run 3;wait 500;$D-clock"
SC[system]="run 4;wait 600;key Down;key Down;wait 300;$D-system"
SC[delete]="run 0;wait 400;key Down;key Return;wait 400;key Down;key Down;key Down;key Down;key Down;key d c;wait 400;$D-delete"
SC[about]="run 4;wait 300;key a cs;wait 400;$D-about"
SC[x11]="wait 4500;focus XEyes;move 30 112;wait 300;run 3;move 400 100;wait 2500;$D-x11"
SC[tasks]="run 0;wait 300;run 1;wait 300;run 2;wait 300;tap 600 12;wait 300;$D-tasks"
SC[mem]="wait 300;$D-mem0;wait 1500;run 0;wait 400;open /c/Documents/Plans.txt;wait 400;run 2;wait 300;run 3;wait 300;run 4;wait 600;menu 0;wait 300;key Escape;$D-mem"
SC[rename]="run 0;wait 400;key Down;key Return;wait 400;key Down;key Down;key r c;wait 300;type Plans;wait 300;$D-rename"
SC[switch]="run 0;wait 300;open /c/Documents/Plans.txt;wait 300;run 2;wait 300;run 4;wait 400;key Tab c;key Tab c;wait 400;$D-switch"
SC[help]="run 0;wait 300;key h cs;wait 300;key Down;key Down;key Down;wait 300;$D-help"
SC[doom]="key Right;wait 200;tap 280 140;wait 200;tap 280 140;wait 500;$D-doom"
SC[progmenu]="key Right;key Right;menu 2;wait 400;$D-progmenu"
names=${@:-${!SC[@]} splash}
for n in $names; do
	case $n in
	splash)
		"$REPO/build/psiwm/psisplash" -o "$T/splash.raw" "Starting the X server…" 45 2>/dev/null \
			|| qemu-arm "$REPO/build/psiwm/psisplash" -o "$T/splash.raw" "Starting the X server…" 45
		python3 "$HERE/fb2psion.py" "$T/splash.raw" "$REPO/build/psiwm-shots/splash.png"
		python3 "$HERE/fb2psion.py" "$T/splash.raw" "$REPO/build/psiwm-shots/splash-2x.png" 2 ;;
	x11) "$S" "$n" "${SC[$n]}" xterm xeyes ;;
	*) "$S" "$n" "${SC[$n]}" ;;
	esac
done
