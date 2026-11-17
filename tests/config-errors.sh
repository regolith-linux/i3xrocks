#!/bin/sh
# Invalid lines and unreadable files are skipped one by one, the rest of the
# configuration still loads, and an urgent first block names the files.
# Needs no X server.

I3XROCKS=${I3XROCKS:-./i3xrocks}
I3XROCKS=$(cd "$(dirname "$I3XROCKS")" && pwd)/$(basename "$I3XROCKS")

command -v timeout >/dev/null 2>&1 || { echo "SKIP: timeout not found"; exit 77; }

tmp=$(mktemp -d)
trap 'chmod -R u+rwX "$tmp"; rm -rf "$tmp"' EXIT
fails=0

fail() {
	echo "FAIL: $*"
	fails=$((fails + 1))
}

# run <name> <seconds> <arguments>...: output in $tmp/<name>.out and .err
run() {
	name=$1 secs=$2
	shift 2
	# stdin stays open, as with a real bar
	sleep $((secs + 1)) | env -u DISPLAY timeout "$secs" "$I3XROCKS" "$@" \
		>"$tmp/$name.out" 2>"$tmp/$name.err"
	rc=$?
	[ "$rc" -eq 124 ] || fail "$name: i3xrocks exited with $rc"
	grep "^,\[" "$tmp/$name.out" | tail -n 1 >"$tmp/$name.last"
}

has() { grep -qF -- "$2" "$tmp/$1.last"; }

# block_has <run> <block name> <text>: the block in the last status line has <text>
block_has() {
	grep -o "{[^{}]*\"name\":\"$2\"[^{}]*}" "$tmp/$1.last" | grep -qF -- "$3"
}

mkdir "$tmp/conf.d"
cd "$tmp/conf.d" || exit 1
printf 'separator=false\n-bad global line\nalign=center\n' >01_global
printf '[tick]\ncommand=echo tick\ninterval=1\n' >05_tick
printf '[a]\nfull_text=A\nthis line has no equals sign\ncolor=#00FF00\ninterval=once\n' >10_a
printf '[b]\nfull_text=B\ninterval=once\n[broken\nfull_text=LEAK\ncolor=#FF0000\n' >20_bad
printf '[c] trailing\nfull_text=LEAK\n[d]\nfull_text=D\ninterval=once\n' >30_trailing
mkdir 40_dir
ln -s missing 50_dangling
names="01_global, 10_a, 20_bad, 30_trailing, 40_dir, 50_dangling"
# root reads any file
if [ "$(id -u)" -ne 0 ]; then
	printf '[u]\nfull_text=LEAK\n' >55_unreadable
	chmod 000 55_unreadable
	names="$names, 55_unreadable"
fi
printf '[e]\nfull_text=E\ninterval=once\n' >60_last
cd - >/dev/null || exit 1

run dir 4 -d "$tmp/conf.d"
want="\"full_text\":\"Oops! Configuration errors in $names. Increase log level for details.\",\"urgent\":true"
has dir "[{$want" || fail "dir: no persistent warning first in the last status line"
block_has dir tick '"full_text":"tick"' || fail "dir: block tick missing"
block_has dir a '"full_text":"A"' || fail "dir: block a missing"
block_has dir a '"color":"#00FF00"' || fail "dir: line after an invalid one not applied"
for b in b d e; do
	block_has dir $b "\"full_text\":\"$(echo $b | tr a-z A-Z)\"" || fail "dir: block $b missing"
done
grep -q LEAK "$tmp/dir.out" && fail "dir: properties of an invalid section were used"
grep -q '"color":"#FF0000"' "$tmp/dir.out" && fail "dir: properties of an invalid section were used"
block_has dir b '"separator":false' || fail "dir: global before an invalid line not applied"
block_has dir e '"align":"center"' || fail "dir: global after an invalid line not applied"
grep -q 'line 2: invalid INI syntax' "$tmp/dir.err" || fail "dir: invalid line not logged"
grep -q 'failed to load config file 50_dangling, skipping it' "$tmp/dir.err" || fail "dir: dangling link not logged"

# A single file with an invalid line names that file
run single 2 -c "$tmp/conf.d/10_a"
has single "\"full_text\":\"Oops! Configuration errors in 10_a." || fail "single: no warning naming the file"
block_has single a '"color":"#00FF00"' || fail "single: block not loaded"

# A missing single file loads nothing, the failure stays for the whole run
run missing 3 -c "$tmp/missing.conf"
has missing '[{"full_text":"Oops! Failed to load configuration. Increase log level for details.","urgent":true' ||
	fail "missing: no persistent failure message"

# A clean configuration shows no warning
mkdir "$tmp/clean.d"
cp "$tmp/conf.d/05_tick" "$tmp/conf.d/60_last" "$tmp/clean.d"
run clean 2 -d "$tmp/clean.d"
grep -q Oops "$tmp/clean.out" && fail "clean: unexpected warning"
block_has clean e '"full_text":"E"' || fail "clean: block not loaded"

if [ "$fails" -ne 0 ]; then
	for f in "$tmp"/*.out "$tmp"/*.err; do
		echo "== $(basename "$f")"
		cat "$f"
	done
	exit 1
fi
echo "PASS"
