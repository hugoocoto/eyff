#!/usr/bin/env bash
# The docs say what --help says ($TEST_BIN's), so a flag added, renamed or
# removed in src/main.c fails `make test` until they follow it:
# - README.md's Usage is --help itself,
# - the man page's OPTIONS (docs/*.1) have every flag, and no other,
# - the Typst documents (docs/*.typ), if any, mention every flag.
set -euo pipefail

help=$("$TEST_BIN" --help | sed '/./,$!d')
failed=0
fail() {
        echo "$*"
        failed=1
}

# --help's flags, one per line: "--config", "-c"
flags() { sed -n '/^options:$/,/^$/s/^ \(.*\)/\1/p' | sed 's/  .*//' | tr ',' ' ' | tr -s ' ' '\n' | grep -- '^-' | sort -u; }
want=$(printf '%s\n' "$help" | flags)

# The first code block under "## Usage"
usage=$(awk '/^## / { here = ($0 == "## Usage") } here && /^```/ { if (block) exit; block = 1; next } block' README.md)
if [ "$usage" != "$help" ]; then
        fail "README.md's Usage is not what --help prints (< README, > --help):"
        diff <(printf '%s\n' "$usage") <(printf '%s\n' "$help") || true
fi

for man in docs/*.1; do
        [ -f "$man" ] || continue
        # \-c and \-\-config, between .SH OPTIONS and the next .SH
        have=$(sed -n '/^\.SH OPTIONS/,/^\.SH/p' "$man" | grep -oE '\\-(\\-)?[A-Za-z0-9][A-Za-z0-9-]*' | tr -d '\134' | sort -u)
        if [ "$have" != "$want" ]; then
                fail "$man's OPTIONS don't have the flags --help has (< man page, > --help):"
                diff <(printf '%s\n' "$have") <(printf '%s\n' "$want") || true
        fi
done

for typ in docs/*.typ; do
        [ -f "$typ" ] || continue
        for flag in $want; do
                grep -qF -- "$flag" "$typ" || fail "$typ doesn't mention $flag"
        done
done

exit $failed
