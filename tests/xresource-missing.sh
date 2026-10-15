#!/bin/sh
# Regression test: an "xresource:" value whose resource is undefined and that
# has no default must not crash i3xrocks; the property is ignored instead.
# Needs Xvfb and xrdb; skipped (exit 77) when they are not installed.

I3XROCKS=${I3XROCKS:-./i3xrocks}

for tool in Xvfb xrdb timeout; do
	command -v "$tool" >/dev/null 2>&1 || { echo "SKIP: $tool not found"; exit 77; }
done

tmp=$(mktemp -d)
xvfb_pid=
cleanup() {
	[ -n "$xvfb_pid" ] && kill "$xvfb_pid" 2>/dev/null
	rm -rf "$tmp"
}
trap cleanup EXIT

# -noreset keeps the (empty) resource database when xrdb disconnects.
Xvfb -displayfd 3 -noreset -nolisten tcp 3>"$tmp/display" 2>/dev/null &
xvfb_pid=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
	[ -s "$tmp/display" ] && break
	sleep 0.5
done
[ -s "$tmp/display" ] || { echo "FAIL: Xvfb did not start"; exit 1; }
DISPLAY=":$(cat "$tmp/display")"
export DISPLAY
xrdb -nocpp -load /dev/null

mkdir "$tmp/conf.d"
printf 'color=xresource:i3xrocks.value.color\n' >"$tmp/conf.d/01_setup"
printf '[hello]\nfull_text=hello\ninterval=once\n' >"$tmp/conf.d/10_hello"
printf '[fallback]\nfull_text=xresource:i3xrocks.label.missing fallback\ninterval=once\n' >"$tmp/conf.d/20_fallback"

timeout 2 "$I3XROCKS" -d "$tmp/conf.d" >"$tmp/out" 2>"$tmp/err"
rc=$?
cat "$tmp/out" "$tmp/err"

# 124: still running when timeout stopped it (139 would be the old segfault).
[ "$rc" -eq 124 ] || { echo "FAIL: i3xrocks exited with $rc"; exit 1; }
grep -q '"full_text":"hello"' "$tmp/out" || { echo "FAIL: block without the resource not rendered"; exit 1; }
grep -q '"full_text":"fallback"' "$tmp/out" || { echo "FAIL: default value not used"; exit 1; }
grep -q '"color"' "$tmp/out" && { echo "FAIL: undefined resource set a color"; exit 1; }
grep -q 'i3xrocks.value.color' "$tmp/err" || { echo "FAIL: no error logged for the undefined resource"; exit 1; }
echo "PASS"
