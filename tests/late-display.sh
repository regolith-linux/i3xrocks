#!/bin/sh
# Without a usable X display at startup, i3xrocks keeps looking for one and
# adopts its resources, without a restart, while the blocks keep updating.
# Needs Xvfb and xrdb; skipped (exit 77) when they are not installed.

I3XROCKS=${I3XROCKS:-./i3xrocks}
I3XROCKS=$(cd "$(dirname "$I3XROCKS")" && pwd)/$(basename "$I3XROCKS")

for tool in Xvfb xrdb timeout; do
	command -v "$tool" >/dev/null 2>&1 || { echo "SKIP: $tool not found"; exit 77; }
done

tmp=$(mktemp -d)
xvfb_pid=
cleanup() {
	[ -s "$tmp/bar.pid" ] && kill "$(cat "$tmp/bar.pid")" 2>/dev/null
	if [ -n "$xvfb_pid" ]; then
		kill -CONT "$xvfb_pid" 2>/dev/null
		kill "$xvfb_pid" 2>/dev/null
		wait "$xvfb_pid" 2>/dev/null
	fi
	rm -rf "$tmp"
}
trap cleanup EXIT
fails=0

fail() {
	echo "FAIL: $*"
	fails=$((fails + 1))
}

now() { date +%s.%N; }

# Find a free display number
Xvfb -displayfd 3 -nolisten tcp 3>"$tmp/display" 2>/dev/null &
pid=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
	[ -s "$tmp/display" ] && break
	sleep 0.5
done
kill "$pid"
wait "$pid" 2>/dev/null
display=":$(cat "$tmp/display")"
[ "$display" != : ] || { echo "FAIL: Xvfb did not start"; exit 1; }

start_xvfb() {
	Xvfb "$display" -noreset -nolisten tcp -audit 4 2>"$tmp/xvfb.log" &
	xvfb_pid=$!
	for _ in 1 2 3 4 5 6 7 8 9 10; do
		DISPLAY=$display xrdb -query >/dev/null 2>&1 && return
		sleep 0.2
	done
	echo "FAIL: Xvfb did not start"
	exit 1
}

load_resources() {
	DISPLAY=$display xrdb -nocpp -load - <<-EOF
	t.text: ADOPTED
	t.color: #ABCDEF
	t.label: NEW:
	t.empty:
	EOF
	loaded=$(now)
}

mkdir "$tmp/conf.d"
printf 'color=xresource:t.color #112233\nshort_text=xresource:t.absent KEEP\n' >"$tmp/conf.d/01_setup"
printf '[tick]\ncommand=date +%%s%%N\ninterval=1\n' >"$tmp/conf.d/05_tick"
printf '[a]\nfull_text=xresource:t.text FALLBACK\ninterval=once\n' >"$tmp/conf.d/10_a"
printf '[l]\nlabel=xresource:t.label OLD: \ncommand=echo BODY\ninterval=once\n' >"$tmp/conf.d/20_l"
printf '[e]\nfull_text=E\ninstance=xresource:t.empty KEPT\ninterval=once\n' >"$tmp/conf.d/30_e"

# stdin stays open, as with a real bar
mkfifo "$tmp/stdin"
exec 9<>"$tmp/stdin"

# start_bar <name>: i3xrocks in the background, each line prefixed by its time
start_bar() {
	started=$(now)
	: >"$tmp/bar.pid"
	DISPLAY=$display sh -c 'echo $$ >"$1"; exec "$2" -d "$3"' sh \
		"$tmp/bar.pid" "$I3XROCKS" "$tmp/conf.d" <&9 2>"$tmp/$1.err" |
		while IFS= read -r line; do
			echo "$(now) $line"
		done >"$tmp/$1.out" &
	reader=$!
	while [ ! -s "$tmp/bar.pid" ]; do sleep 0.1; done
}

stop_bar() {
	kill "$(cat "$tmp/bar.pid")"
	wait "$reader"
	: >"$tmp/bar.pid"
}

# first <run> <text>: time of the first status line containing <text>, or nothing
first() { grep -F -- "$2" "$tmp/$1.out" | head -n 1 | cut -d' ' -f1; }

# within <a> <b> <seconds>: b - a <= seconds
within() { awk -v a="$1" -v b="$2" -v s="$3" 'BEGIN { exit !(b - a <= s) }'; }

# check_ticks <run> <from> <to>: the tick block updated at least every 2.5 s
check_ticks() {
	grep -o '^[0-9.]* .*"name":"tick","full_text":"[0-9]*"' "$tmp/$1.out" |
		sed 's/ .*"full_text":"/ /; s/"$//' | uniq -f 1 |
		awk -v from="$2" -v to="$3" -v run="$1" '
			$1 >= from && $1 <= to { if (prev && $1 - prev > 2.5) gap = $1 - prev; prev = $1; n++ }
			END { if (gap || n < 3) { print "FAIL: " run ": tick gap " gap ", " n " updates"; exit 1 } }' ||
		fails=$((fails + 1))
}

# block_has <run> <block name> <text>: the block in the last status line has <text>
block_has() {
	grep '^[0-9.]* ,\[' "$tmp/$1.out" | tail -n 1 |
		grep -o "{[^{}]*\"name\":\"$2\"[^{}]*}" | grep -qF -- "$3"
}

check_adopted() {
	block_has "$1" a '"full_text":"ADOPTED"' || fail "$1: resource not adopted"
	block_has "$1" a '"color":"#ABCDEF"' || fail "$1: global resource not adopted"
	# undefined and empty resources keep their defaults
	block_has "$1" a '"short_text":"KEEP"' || fail "$1: default lost"
	block_has "$1" e '"instance":"KEPT"' || fail "$1: default lost"
	# the output of the command stays
	block_has "$1" l '"full_text":"NEW:BODY"' || fail "$1: label not adopted"
	block_has "$1" tick '"color":"#ABCDEF"' || fail "$1: global resource not adopted"
	grep -q Oops "$tmp/$1.out" && fail "$1: unexpected warning"
}

# 1. The display starts two seconds after the bar
start_bar late
sleep 1
if command -v pkill >/dev/null 2>&1; then
	# The helper looking for the display (a fork of this bar, so with the
	# same command line) is not taken for the bar...
	bars=0 helpers=0
	for p in $(pgrep -f -- "-d $tmp/conf.d"); do
		case $(cat "/proc/$p/comm" 2>/dev/null) in
		i3xrocks) bars=$((bars + 1)) ;;
		i3xrocks-xres) helpers=$((helpers + 1)) ;;
		esac
	done
	[ "$bars" -eq 1 ] && [ "$helpers" -ge 1 ] ||
		fail "late: $bars processes named i3xrocks, $helpers helpers"
	# ...and survives the signals of key bindings refreshing blocks
	pkill -RTMIN+1 -f -- "-d $tmp/conf.d"
fi
sleep 1
start_xvfb
load_resources
sleep 5
stop_bar
fallback=$(first late '"color":"#112233","short_text":"KEEP","name":"a","full_text":"FALLBACK"')
[ -n "$fallback" ] && within "$started" "$fallback" 3 || fail "late: defaults not shown within 3 s"
first late '"full_text":"OLD: BODY"' | grep -q . || fail "late: default label not shown"
adopted=$(first late '"full_text":"ADOPTED"')
[ -n "$adopted" ] && within "$loaded" "$adopted" 5 || fail "late: resources not adopted within 5 s"
check_adopted late
check_ticks late "$started" "$(now)"

# 2. A server holding the connections without answering, then answering
before=$(grep -c ' connected from ' "$tmp/xvfb.log")
kill -STOP "$xvfb_pid"
start_bar silent
sleep 6
loaded=$(now)
kill -CONT "$xvfb_pid"
sleep 4
stop_bar
fallback=$(first silent '"name":"a","full_text":"FALLBACK"')
[ -n "$fallback" ] && within "$started" "$fallback" 3 || fail "silent: defaults not shown within 3 s"
adopted=$(first silent '"full_text":"ADOPTED"')
[ -n "$adopted" ] && within "$loaded" "$adopted" 2 || fail "silent: slow answer not adopted"
check_adopted silent
check_ticks silent "$started" "$loaded"
attempts=$(($(grep -c ' connected from ' "$tmp/xvfb.log") - before))
# an attempt at most every two seconds
[ "$attempts" -ge 2 ] && [ "$attempts" -le 4 ] || fail "silent: $attempts connections in 6 s"

kill "$xvfb_pid"

if [ "$fails" -ne 0 ]; then
	for f in "$tmp"/*.out "$tmp"/*.err "$tmp/xvfb.log"; do
		echo "== $(basename "$f")"
		cat "$f"
	done
	exit 1
fi
echo "PASS"
