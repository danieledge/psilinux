#!/bin/sh
# m4test.sh - the other programs (M4): each one runs, and how long it takes
pass=0; fail=0
check() {
	if [ "$2" = "$3" ]; then echo "ok   $1"; pass=$((pass+1));
	else echo "FAIL $1: expected [$2] got [$3]"; fail=$((fail+1)); fi
}
check "lua" "55" "$(lua -e 'local s=0 for i=1,10 do s=s+i end print(s)')"
check "lua string.format" "3.14" "$(lua -e 'print(string.format("%.2f", math.pi))')"
check "micropython" "[0, 1, 4, 9]" "$(micropython -c 'print([x*x for x in range(4)])')"
check "micropython float" "1.414214" "$(micropython -c 'import math; print("%.6f" % math.sqrt(2))')"
check "micropython json" '{"a": 1}' "$(micropython -c 'import json; print(json.dumps({"a": 1}))')"
check "sqlite3" "3" "$(sqlite3 :memory: 'create table t(x); insert into t values(1),(2),(3); select count(*) from t;')"
rm -f /tmp/t.db; sqlite3 /tmp/t.db "create table t(s); insert into t values('hello');"
check "sqlite3 file" "hello" "$(sqlite3 /tmp/t.db 'select s from t;')"
check "jq" "2" "$(echo '{"a":[1,2,3]}' | jq '.a[1]')"
check "jq -r" "psion" "$(echo '{"name":"psion"}' | jq -r .name)"
check "nano" "yes" "$(nano --version | grep -q 'GNU nano' && echo yes)"
check "curl" "yes" "$(curl --version | grep -q mbedTLS && echo yes)"
echo "$pass passed, $fail failed"
