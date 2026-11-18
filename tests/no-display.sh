#!/bin/sh
# Without a usable X display, "xresource:" properties take their default (or
# stay unset) and the blocks are shown; with one, the resources are used.
# Needs Xvfb and xrdb; skipped (exit 77) when they are not installed.

I3XROCKS=${I3XROCKS:-./i3xrocks}
I3XROCKS=$(cd "$(dirname "$I3XROCKS")" && pwd)/$(basename "$I3XROCKS")

for tool in Xvfb xrdb timeout; do
	command -v "$tool" >/dev/null 2>&1 || { echo "SKIP: $tool not found"; exit 77; }
done

tmp=$(mktemp -d)
xvfb_pid=
cleanup() {
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

# start_xvfb [xrdb input]: start a server, with these resources or none
start_xvfb() {
	stop_xvfb
	: >"$tmp/display"
	# -noreset keeps the resources when xrdb disconnects
	Xvfb -displayfd 3 -noreset -nolisten tcp -audit 4 3>"$tmp/display" 2>"$tmp/xvfb.log" &
	xvfb_pid=$!
	for _ in 1 2 3 4 5 6 7 8 9 10; do
		[ -s "$tmp/display" ] && break
		sleep 0.5
	done
	[ -s "$tmp/display" ] || { echo "FAIL: Xvfb did not start"; exit 1; }
	display=":$(cat "$tmp/display")"
	if [ $# -gt 0 ]; then
		printf '%s' "$1" | DISPLAY=$display xrdb -nocpp -load -
	else
		DISPLAY=$display xrdb -remove
	fi
}

stop_xvfb() {
	[ -n "$xvfb_pid" ] || return
	kill -CONT "$xvfb_pid" 2>/dev/null
	kill "$xvfb_pid" 2>/dev/null
	wait "$xvfb_pid" 2>/dev/null
	xvfb_pid=
}

connections() { grep -c ' connected from ' "$tmp/xvfb.log"; }

# run <name> <seconds> <DISPLAY or -> [env assignments]: output in $tmp/<name>.*
run() {
	name=$1 secs=$2 disp=$3
	shift 3
	if [ "$disp" = - ]; then
		set -- env -u DISPLAY HOME="$tmp" "$@"
	else
		set -- env DISPLAY="$disp" HOME="$tmp" "$@"
	fi
	# stdin stays open, as with a real bar
	sleep $((secs + 1)) | timeout "$secs" "$@" "$I3XROCKS" -d "$tmp/conf.d" \
		>"$tmp/$name.out" 2>"$tmp/$name.err"
	rc=$?
	[ "$rc" -eq 124 ] || fail "$name: i3xrocks exited with $rc"
	grep '^,\[' "$tmp/$name.out" | tail -n 1 >"$tmp/$name.last"
}

# block_has <run> <block name> <text>: the block in the last status line has <text>
block_has() {
	grep -o "{[^{}]*\"name\":\"$2\"[^{}]*}" "$tmp/$1.last" | grep -qF -- "$3"
}

clean() { ! grep -q Oops "$tmp/$1.out" || fail "$1: unexpected warning"; }

# Only used when the server holds no resources
printf 't.text: FROM FILE\n' >"$tmp/.Xresources"

mkdir "$tmp/conf.d"
printf 'color=xresource:t.color #112233\n' >"$tmp/conf.d/01_setup"
printf '[a]\nfull_text=xresource:t.text FALLBACK\ninterval=once\n' >"$tmp/conf.d/10_a"
printf '[b]\nfull_text=B\nshort_text=xresource:t.empty SHORT\ninterval=once\n' >"$tmp/conf.d/20_b"
printf '[c]\nfull_text=C\ncolor=xresource:t.undefined\ninterval=once\n' >"$tmp/conf.d/30_c"

# check_fallbacks <run>: every block shown with its defaults
check_fallbacks() {
	block_has "$1" a '"full_text":"FALLBACK"' || fail "$1: default not used"
	block_has "$1" a '"color":"#112233"' || fail "$1: global default not used"
	block_has "$1" b '"short_text":"SHORT"' || fail "$1: default not used"
	# without a default, the global value stays
	block_has "$1" c '"color":"#112233"' || fail "$1: global value lost"
	clean "$1"
}

run unset 2 -
check_fallbacks unset

run nosocket 2 :99 XAUTHORITY=/dev/null
check_fallbacks nosocket

# An X server holding the connection without answering
start_xvfb 't.text: FROM STORE
'
kill -STOP "$xvfb_pid"
run silent 3 "$display"
check_fallbacks silent
first=$(grep -n '"name":"a"' "$tmp/silent.out" | head -n 1 | cut -d: -f1)
[ -n "$first" ] || fail "silent: blocks never shown"
kill -CONT "$xvfb_pid"

# With resources: values (spaces kept), empty and undefined ones
start_xvfb 't.text: FROM  STORE
t.color: #ABCDEF
t.empty:
'
before=$(connections)
run store 2 "$display"
block_has store a '"full_text":"FROM  STORE"' || fail "store: resource not used"
block_has store a '"color":"#ABCDEF"' || fail "store: global resource not used"
block_has store b '"short_text":"SHORT"' || fail "store: empty resource not replaced by its default"
block_has store c '"color":"#ABCDEF"' || fail "store: global value lost"
clean store
# one connection however many files use resources
n=$(($(connections) - before))
[ "$n" -eq 1 ] || fail "store: $n connections to the X server"

# A server without resources: the resource files are read
start_xvfb
run files 2 "$display"
block_has files a '"full_text":"FROM FILE"' || fail "files: ~/.Xresources not used"
block_has files b '"short_text":"SHORT"' || fail "files: default not used"

stop_xvfb

if [ "$fails" -ne 0 ]; then
	for f in "$tmp"/*.out "$tmp"/*.err; do
		echo "== $(basename "$f")"
		cat "$f"
	done
	exit 1
fi
echo "PASS"
