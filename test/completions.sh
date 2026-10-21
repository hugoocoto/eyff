#!/usr/bin/env bash
# The completions the program ($TEST_BIN) prints with --completions (flag.h):
# bash's offer every flag in --help, and leave a flag's value to bash's own
# file completion; fish's offer every flag too, and zsh reads its own (where
# they're installed). An unknown shell is an error.
set -euo pipefail

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
help=$("$TEST_BIN" --help)
name=$(printf '%s\n' "$help" | sed -n 's/^usage: \([^ ]*\).*/\1/p')
name=${name##*/}
for shell in bash zsh fish; do
        "$TEST_BIN" --completions "$shell" >"$dir/$shell"
done
if "$TEST_BIN" --completions nosuchshell >/dev/null 2>&1; then
        echo "--completions nosuchshell didn't fail"
        exit 1
fi

# --help's flags, one per line, and the ones that take a value ("-c C")
specs=$(printf '%s\n' "$help" | sed -n '/^options:$/,/^$/s/^ \(.*\)/\1/p' | sed 's/  .*//')
want=$(printf '%s\n' "$specs" | tr ',' ' ' | tr -s ' ' '\n' | grep -- '^-' | sort -u)
valued=$(printf '%s\n' "$specs" | grep -E ' [^-]' | tr ',' ' ' | tr -s ' ' '\n' | grep -- '^-' || true)
[ -n "$want" ] || { echo "no flags in --help"; exit 1; }

# bash, as if Tab was pressed after the words given
cat >"$dir/try.bash" <<EOT
source "$dir/bash"
try() {
        COMP_WORDS=("\$@")
        COMP_CWORD=\$((\$# - 1))
        COMPREPLY=()
        "\$(complete -p $name | sed 's/.*-F \([^ ]*\).*/\1/')"
        printf '%s\n' "\${COMPREPLY[@]}"
}
EOT
got=$(bash -c "source '$dir/try.bash'; try $name -" | sed '/^$/d' | sort -u)
[ "$got" = "$want" ] || { echo "bash completes '$name -' with: $got"; exit 1; }
# A value is bash's default completion (files): no flags there
complete=$(bash -c "source '$dir/bash'; complete -p $name")
[[ $complete == *"-o default"* ]] || { echo "bash's completion has no -o default: $complete"; exit 1; }
for flag in $valued; do
        got=$(bash -c "source '$dir/try.bash'; try $name $flag -" | sed '/^$/d')
        [ -z "$got" ] || { echo "bash completes flags after $name $flag: $got"; exit 1; }
done

# fish 3.6 or newer: older ones have no --no-config
if command -v fish >/dev/null && fish --no-config -c true 2>/dev/null; then
        got=$(fish --no-config -c "source '$dir/fish'; complete -C '$name -'" | cut -f1 | sort -u)
        [ "$got" = "$want" ] || { echo "fish completes '$name -' with: $got"; exit 1; }
fi
if command -v zsh >/dev/null; then
        zsh -n "$dir/zsh" || { echo "zsh can't read its completion"; exit 1; }
fi
