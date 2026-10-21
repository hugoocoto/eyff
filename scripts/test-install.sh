#!/usr/bin/env bash
# Test scripts/install.sh on a release, the way users run it: install,
# reinstall, uninstall.
#
#   scripts/test-install.sh TAG [OPTION...]
#
# The release is $REPO's TAG on $GITHUB, like for install.sh, and the OPTIONs
# go to every install.sh run. CI runs it before publishing anything, on what
# it built, served from a dir (GITHUB=file://...), and on every release once
# it's published. It installs into a temp dir, and needs no root.
set -euo pipefail
cd "$(dirname "$0")/.."

[ $# -ge 1 ] || { echo "usage: $0 TAG [OPTION...]" >&2; exit 2; }
tag=$1
shift
name=$(sed -n 's/^NAME := //p' Makefile)
version=$(make -s version)
install=$PWD/scripts/install.sh

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
trap 'echo "test-install: failed: $BASH_COMMAND (line $LINENO)" >&2' ERR
cd "$tmp"
export PREFIX=prefix               # relative on purpose
export APPIMAGE_EXTRACT_AND_RUN=1  # AppImages without FUSE

run() { bash "$install" --yes "$@"; }
installed() { [[ $("prefix/bin/$name" --version) == "$name $version" ]]; }

run "$@" "$tag"
installed
test -f "prefix/share/man/man1/$name.1"
test -f "prefix/share/bash-completion/completions/$name"
test -f "prefix/share/zsh/site-functions/_$name"
test -f "prefix/share/fish/vendor_completions.d/$name.fish"
test -f "prefix/share/applications/$name.desktop"
run "$@" --appimage "$tag"
installed
run "$@" "$tag"
installed
run uninstall
[ -z "$(find prefix -type f)" ]
echo "install.sh installs, reinstalls and uninstalls $name $version ($tag)"
