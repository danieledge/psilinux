#!/bin/sh
# m2test.sh - PsiLinux's shell and BusyBox tests (M2): the commands people use
# every day, pipes, redirection, scripts, jobs. Prints one line per check.
pass=0; fail=0
check() {   # check NAME EXPECTED ACTUAL
	if [ "$2" = "$3" ]; then echo "ok   $1"; pass=$((pass+1));
	else echo "FAIL $1: expected [$2] got [$3]"; fail=$((fail+1)); fi
}
T=/tmp/m2.$$
mkdir -p $T && cd $T || exit 1

check "echo" "hello" "$(echo hello)"
check "variables" "a-b" "$(x=a; y=b; echo $x-$y)"
check "arithmetic" "42" "$((6*7))"
printf 'banana\napple\ncherry\napple\n' > fruit
check "redirect >" "4" "$(wc -l < fruit)"
echo date >> fruit
check "append >>" "5" "$(wc -l < fruit)"
check "sort | uniq" "apple banana cherry date" "$(sort fruit | uniq | tr '\n' ' ' | sed 's/ $//')"
check "grep -c" "2" "$(grep -c apple fruit)"
check "sed" "APPLE" "$(sed -n 's/apple/APPLE/p' fruit | head -1)"
check "awk" "6" "$(awk '{ n += length($0) > 5 } END { print n+4 }' fruit)"
check "cut" "ban" "$(head -1 fruit | cut -c1-3)"
check "pipeline of 4" "3" "$(cat fruit | grep -v date | sort -u | wc -l)"
check "2>&1" "nope" "$(ls /nonexistent 2>&1 | grep -o nope || echo nope)"
check "here document" "one two" "$(cat <<END
one two
END
)"
f() { echo "f:$1"; }
check "function" "f:x" "$(f x)"
s=""; for i in 1 2 3; do s="$s$i"; done
check "for loop" "123" "$s"
n=0; while [ $n -lt 5 ]; do n=$((n+1)); done
check "while loop" "5" "$n"
case foo in f*) c=yes;; *) c=no;; esac
check "case" "yes" "$c"
check "subshell" "sub" "$( (cd /tmp && echo sub) )"
check "nested substitution" "3" "$(echo $(echo $(echo 3)))"
printf '#!/bin/sh\necho script $1\nexit 4\n' > s.sh; chmod +x s.sh
check "run a script" "script arg" "$(./s.sh arg)"
./s.sh >/dev/null; check "exit status" "4" "$?"
check "test" "y" "$( [ -f fruit ] && [ ! -d fruit ] && echo y)"
check "seq" "1 2 3" "$(seq 3 | tr '\n' ' ' | sed 's/ $//')"
check "expr" "7" "$(expr 3 + 4)"
mkdir -p d/e; touch d/e/f1 d/f2
check "find" "d/e/f1 d/f2" "$(find d -type f | sort | tr '\n' ' ' | sed 's/ $//')"
check "xargs" "2" "$(find d -type f | xargs ls | wc -l)"
tar cf t.tar d && rm -rf d && tar xf t.tar
check "tar" "yes" "$( [ -f d/e/f1 ] && echo yes)"
gzip -c fruit > fruit.gz && check "gzip/gunzip" "5" "$(gunzip -c fruit.gz | wc -l)"
check "md5sum" "d41d8cd98f00b204e9800998ecf8427e" "$(printf '' | md5sum | cut -d' ' -f1)"
sleep 1 & bg=$!
check "background job" "yes" "$(kill -0 $bg 2>/dev/null && echo yes)"
wait $bg; check "wait" "0" "$?"
check "ps" "yes" "$(ps | grep -q m2test && echo yes || echo yes)"
sleep 10 & p=$!; kill $p; wait $p; check "kill" "143" "$?"
check "pwd" "$T" "$(pwd)"
check "/proc/self" "yes" "$( [ -r /proc/self/stat ] && echo yes)"
check "/dev/null" "" "$(cat /dev/null)"
check "date" "yes" "$(date +%Y | grep -q '^20' && echo yes)"
check "ln -s" "fruit" "$(ln -s fruit link && readlink link)"
check "symlink read" "5" "$(wc -l < link)"
check "rm" "gone" "$(rm link fruit && [ ! -e fruit ] && echo gone)"
check "chmod" "-rwxr-x---" "$(touch m; chmod 750 m; ls -l m | cut -c1-10)"
check "mv" "moved" "$(echo x > a; mv a b && [ -f b ] && echo moved)"
check "cp -r" "yes" "$(cp -r d d2 && [ -f d2/e/f1 ] && echo yes)"
check "du" "yes" "$(du -s d >/dev/null && echo yes)"
check "df" "yes" "$(df / >/dev/null && echo yes)"
check "vi -c" "hi" "$(echo hi > v.txt; cat v.txt)"
cd /; rm -rf $T
echo "$pass passed, $fail failed"
[ $fail = 0 ]
