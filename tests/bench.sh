#!/bin/sh
# bench.sh - how long things take (elapsed seconds, from the clock)
t() {   # t NAME COMMAND...
	n=$1; shift
	s=$(date +%s)
	"$@" > /dev/null 2>&1
	e=$(date +%s)
	echo "$n: $((e - s)) s"
}
t "true (start a program)" /bin/true
t "ls /bin" ls /bin
t "lua fib(20)" lua -e 'local function f(n) if n<2 then return n end return f(n-1)+f(n-2) end print(f(20))'
t "micropython 100k loop" micropython -c 's=0
for i in range(100000): s+=i
print(s)'
t "sqlite3 1000 rows" sqlite3 :memory: 'create table t(x); with recursive c(i) as (select 1 union all select i+1 from c where i<1000) insert into t select i from c; select sum(x) from t;'
t "jq" jq -n '[range(1000)] | add'
t "gzip 100 KB" sh -c 'dd if=/dev/zero bs=1024 count=100 2>/dev/null | gzip | wc -c'
