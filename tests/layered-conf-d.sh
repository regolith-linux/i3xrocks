#!/bin/sh
# Test: with -u USERDIR -d PKGDIR both directories are read together, the
# user's files and sections taking precedence, and SIGHUP reloads them.
# Needs no X server: no "xresource:" value is used.

I3XROCKS=${I3XROCKS:-./i3xrocks}
case $I3XROCKS in /*) ;; *) I3XROCKS=$(pwd)/$I3XROCKS ;; esac

command -v timeout >/dev/null 2>&1 || { echo "SKIP: timeout not found"; exit 77; }

tmp=$(mktemp -d)
pid=
cleanup() {
	[ -n "$pid" ] && kill "$pid" 2>/dev/null
	rm -rf "$tmp"
}
trap cleanup EXIT

fail=0

# Each block prints its name, its "val" and "tag" properties and the base
# name of the directory it runs in.
cat >"$tmp/show" <<'EOF'
printf '%s:%s:%s:%s\n' "$BLOCK_NAME" "$val" "$tag" "$(basename "$(pwd -P)")"
EOF

# blocks FILE: the full_text values of the last status line, space separated
blocks() {
	grep '^,\[' "$1" | tail -n 1 |
		grep -o '"full_text":"[^"]*"' | sed 's/"full_text":"\(.*\)"/\1/' |
		tr '\n' ' ' | sed 's/ $//'
}

# run NAME EXPECTED ARGS...: start the bar, let it settle, compare
run() {
	name=$1 expected=$2
	shift 2
	(cd "$tmp" && timeout 2 "$I3XROCKS" "$@" </dev/null >"$tmp/out" 2>"$tmp/err")
	got=$(blocks "$tmp/out")
	if [ "$got" = "$expected" ]; then
		echo "ok - $name"
	else
		echo "not ok - $name"
		echo "  expected: $expected"
		echo "  got:      $got"
		fail=1
	fi
}

# fresh: empty pkg and user directories
fresh() {
	rm -rf "$tmp/pkg" "$tmp/user"
	mkdir "$tmp/pkg" "$tmp/user"
}

setup="command=/bin/sh $tmp/show
interval=once
val=pkg"

# Package tree used by most cases
pkgtree() {
	fresh
	printf '%s\n' "$setup" >"$tmp/pkg/01_setup"
	printf '[alpha]\n' >"$tmp/pkg/20_alpha"
	printf '[gamma]\ntag=g\n' >"$tmp/pkg/40_gamma"
	printf '[delta]\ninstance=one\ntag=d1\n\n[delta]\ninstance=two\ntag=d2\n' >"$tmp/pkg/60_delta"
}

pkgtree
run "-d alone" \
	"alpha:pkg::pkg gamma:pkg:g:pkg delta:pkg:d1:pkg delta:pkg:d2:pkg" \
	-d "$tmp/pkg"

pkgtree
printf '[beta]\n' >"$tmp/user/30_beta"
run "user and package files are merged in file name order" \
	"alpha:pkg::pkg beta:pkg::pkg gamma:pkg:g:pkg delta:pkg:d1:pkg delta:pkg:d2:pkg" \
	-u "$tmp/user" -d "$tmp/pkg"

pkgtree
printf '[gamma]\ntag=mine\n' >"$tmp/user/40_gamma"
: >"$tmp/user/60_delta"
run "a user file replaces the package file of the same name" \
	"alpha:pkg::pkg gamma:pkg:mine:pkg" \
	-u "$tmp/user" -d "$tmp/pkg"

pkgtree
printf '[broken\n' >"$tmp/pkg/50_broken"
: >"$tmp/user/50_broken"
run "a shadowed package file is not read" \
	"alpha:pkg::pkg gamma:pkg:g:pkg delta:pkg:d1:pkg delta:pkg:d2:pkg" \
	-u "$tmp/user" -d "$tmp/pkg"

pkgtree
printf '%s\nval=user\n' "$setup" >"$tmp/user/01_setup"
printf 'val=late\n' >"$tmp/user/50_late"
run "global properties carry across both directories" \
	"alpha:user::user gamma:user:g:user delta:late:d1:user delta:late:d2:user" \
	-u "$tmp/user" -d "$tmp/pkg"

pkgtree
printf '[gamma]\nval=over\n\n[delta]\ninstance=two\ntag=over\n' >"$tmp/user/10_mine"
run "a user section overrides one block, at its own position" \
	"gamma:over:g:pkg delta:pkg:over:pkg alpha:pkg::pkg delta:pkg:d1:pkg" \
	-u "$tmp/user" -d "$tmp/pkg"

pkgtree
printf '[epsilon]\ncommand=/bin/sh %s/show\n\n[alpha]\nval=moved\n' "$tmp" >"$tmp/user/70_mine"
run "commands run in the directory of the file that set them" \
	"gamma:pkg:g:pkg delta:pkg:d1:pkg delta:pkg:d2:pkg epsilon:pkg::user alpha:moved::pkg" \
	-u "$tmp/user" -d "$tmp/pkg"

pkgtree
run "an empty user directory shows the package blocks" \
	"alpha:pkg::pkg gamma:pkg:g:pkg delta:pkg:d1:pkg delta:pkg:d2:pkg" \
	-u "$tmp/user" -d "$tmp/pkg"

pkgtree
rmdir "$tmp/user"
run "a missing user directory shows the package blocks" \
	"alpha:pkg::pkg gamma:pkg:g:pkg delta:pkg:d1:pkg delta:pkg:d2:pkg" \
	-u "$tmp/user" -d "$tmp/pkg"

fresh
printf '%s\n[beta]\n' "$setup" >"$tmp/user/30_beta"
rmdir "$tmp/pkg"
run "a missing package directory shows the user blocks" \
	"beta:pkg::user" \
	-u "$tmp/user" -d "$tmp/pkg"

pkgtree
printf '[broken\n' >"$tmp/user/50_broken"
run "a user file that fails to parse shows the configuration error" \
	"Oops! Failed to load configuration.. Increase log level for details." \
	-u "$tmp/user" -d "$tmp/pkg"

# SIGHUP: a persistent block keeps running until a reload replaces it
pkgtree
cat >"$tmp/user/30_live" <<EOF
[live]
command=echo live; echo \$\$ >$tmp/live.pid; exec sleep 600
interval=persist
EOF
(cd "$tmp" && exec "$I3XROCKS" -u "$tmp/user" -d "$tmp/pkg" </dev/null >"$tmp/out" 2>"$tmp/err") &
pid=$!
sleep 1
child=$(cat "$tmp/live.pid" 2>/dev/null)
rm "$tmp/user/30_live"
printf '[beta]\n' >"$tmp/user/30_beta"
printf '[gamma]\ntag=reloaded\n' >"$tmp/user/40_gamma"
kill -HUP "$pid"
sleep 1
got=$(blocks "$tmp/out")
expected="alpha:pkg::pkg beta:pkg::pkg gamma:pkg:reloaded:pkg delta:pkg:d1:pkg delta:pkg:d2:pkg"
if [ "$got" = "$expected" ] && kill -0 "$pid" 2>/dev/null; then
	echo "ok - SIGHUP reloads both directories"
else
	echo "not ok - SIGHUP reloads both directories"
	echo "  expected: $expected"
	echo "  got:      $got"
	fail=1
fi
if [ -n "$child" ] && ! kill -0 "$child" 2>/dev/null; then
	echo "ok - SIGHUP stops the commands of the previous blocks"
else
	echo "not ok - SIGHUP stops the commands of the previous blocks"
	fail=1
fi
kill "$pid" 2>/dev/null
wait "$pid" 2>/dev/null
pid=

[ "$fail" -eq 0 ] && echo "PASS" || { cat "$tmp/err"; echo "FAIL"; }
exit "$fail"
