#!/bin/bash
# bashtest.sh - Alpine's bash under PsiLinux's translator (Alpine mode):
# what bash has beyond sh. Prints one line per check. (The interactive side,
# job control, Ctrl+C, Tab and history, is checked by run-alpine.sh.)
pass=0; fail=0
check() {   # check NAME EXPECTED ACTUAL
	if [ "$2" = "$3" ]; then echo "ok   $1"; pass=$((pass+1));
	else echo "FAIL $1: expected [$2] got [$3]"; fail=$((fail+1)); fi
}
T=/tmp/bt.$$
mkdir -p $T && cd $T || exit 1

check "version" "5" "${BASH_VERSINFO[0]}"
a=(one two three)
check "arrays" "3 two" "${#a[@]} ${a[1]}"
a+=(four)
check "array append" "one two three four" "${a[*]}"
declare -A h=([psion]=5mx [cpu]=arm)
check "associative arrays" "5mx arm" "${h[psion]} ${h[cpu]}"
check "[[ pattern ]]" "yes" "$([[ psion5mx == psion* ]] && echo yes)"
check "[[ =~ ]]" "5mx" "$([[ 'psion 5mx' =~ ([0-9]+mx) ]] && echo ${BASH_REMATCH[1]})"
check "here-string" "HELLO" "$(tr a-z A-Z <<< hello)"
x="a.b.c"
check "\${x//}" "a-b-c" "${x//./-}"
check "\${x^^}" "A.B.C" "${x^^}"
check "substring" "b.c" "${x:2}"
check "brace expansion" "f1 f2 f3" "$(echo f{1..3})"
check "printf -v" "007" "$(printf -v n '%03d' 7; echo $n)"
check "read" "psion 5mx" "$(echo 'psion 5mx' | { read a b; echo "$a $b"; })"
check "read -a" "3" "$(echo 'x y z' | { read -a w; echo ${#w[@]}; })"
printf 'l1\nl2\nl3\n' > lines
check "mapfile" "3 l2" "$(mapfile -t L < lines; echo ${#L[@]} ${L[1]})"
f() { local v=inner; echo $v; }
v=outer
check "local" "inner outer" "$(f) $v"
check "\$(( ))" "1024" "$((2**10))"
check "let" "6" "$(let y=2*3; echo $y)"
check "PIPESTATUS" "0 1 0" "$(true | false | true; echo ${PIPESTATUS[*]})"
check "pipefail" "1" "$(set -o pipefail; true | false | true; echo $?)"
check "process substitution" "3" "$(wc -l < <(printf 'a\nb\nc\n'))"
check "trap EXIT" "bye" "$(bash -c 'trap "echo bye" EXIT; true')"
check "case" "fruit" "$(case apple in a*) echo fruit;; *) echo other;; esac)"
check "for (( ))" "0 1 2" "$(for ((i=0; i<3; i++)); do printf '%d ' $i; done | sed 's/ $//')"
check "while read" "l1l2l3" "$(while read l; do printf %s $l; done < lines)"
check "nested \$( )" "deep" "$(echo $(echo $(echo deep)))"
check "subshell" "sub top" "$(x=top; (x=sub; echo -n "$x "); echo $x)"
check "set -e in subshell" "1" "$( (set -e; false; echo no) ; echo $?)"
check "functions" "3" "$(sq() { echo $(( $1 * $1 )); }; s=$(sq 1); s2=$(sq 1); echo $((s+s2+1)))"
check "source" "sourced" "$(echo 'echo sourced' > s.sh; . ./s.sh)"
check "glob" "lines s.sh" "$(echo *)"
shopt -s nullglob
check "nullglob" "" "$(echo *.none)"
check "type" "builtin" "$(type -t echo)"
check "command -v" "/bin/ls" "$(command -v ls)"
check "\$RANDOM" "yes" "$([ $RANDOM -ge 0 ] && echo yes)"
check "background & wait" "done 0" "$(sleep 0 & wait $!; echo done $?)"
check "coreutils pipeline" "3" "$(ls /etc | head -3 | wc -l)"
check "bash -c" "42" "$(bash -c 'echo $((6*7))')"
check "script file" "ok" "$(printf '#!/bin/bash\necho ok\n' > t.sh; chmod +x t.sh; ./t.sh)"
cd /; rm -rf $T
echo "$pass passed, $fail failed"
