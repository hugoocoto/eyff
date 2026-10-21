#!/usr/bin/env bash
# Install eyff from its GitHub releases, nothing to build:
#
#   curl -fsSL https://github.com/hugoocoto/eyff/releases/latest/download/install.sh | bash
#
# It asks what to install and where, suggesting what it finds: the latest
# release, ~/.local, the kind already installed. Then it installs the binary
# for this machine (from `uname -m`), the man page and the bash, zsh and fish
# completions, checking them against the release's SHA256SUMS, and SHA256SUMS
# against the release's build attestation when gh is logged in. Nothing is
# replaced until everything is downloaded and checked. Without a terminal, or
# with --yes, it takes the suggestions without asking. See usage() for the
# arguments, which go after `bash -s --`.
set -euo pipefail

NAME=eyff
REPO=${REPO:-hugoocoto/eyff}
GITHUB=${GITHUB:-https://github.com}

usage() {
        cat <<EOF
Usage: ... | bash -s -- [OPTIONS] [VERSION | nightly | uninstall]

Installs $NAME from $GITHUB/$REPO/releases. It asks what to install and
where; the arguments change what it suggests.

  VERSION              that release (v1.2.3 or 1.2.3) instead of the latest one
  nightly              the build of the last commit on main
  uninstall            remove everything this script installs
  --appimage           the AppImage instead of the static binary (it needs
                       FUSE), with its applications menu entry and icon
  --static             the static binary (the default, unless the AppImage is
                       what's installed)
  -y, --yes            don't ask, take the suggestions (also without a terminal)
  --strict             fail if the release's attestation can't be checked
                       (needs gh, logged in)
  --skip-attestation   don't check the attestation, for releases made
                       before there were any
  -h, --help           this help

PREFIX is where it goes: ~/.local by default, /usr/local as root
(... | sudo bash). A relative PREFIX is taken from the current directory.
EOF
}

tmp=''
staged=()
interactive=''

say() { printf '%s\n' "$*"; }
complain() { printf 'install.sh: %s\n' "$*" >&2; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die() { printf 'install.sh: %s\n' "$*" >&2; exit 1; }
fetch() { curl -fsSL --retry 3 -o "$2" "$1"; }

# ask VAR QUESTION SUGGESTION: the answer, typed on the terminal with the
# suggestion already filled in (Enter takes it). It reads /dev/tty because
# under `curl | bash` stdin is this script. Without a terminal, the suggestion.
ask() {
        local -n answer=$1
        if [ -z "$interactive" ]; then
                answer=$3
                return
        fi
        # shellcheck disable=SC2034 # answer is the caller's variable
        read -rep "$2: " -i "$3" answer </dev/tty || die "cancelled"
}

# choose VAR QUESTION SUGGESTION CHOICE...: ask until it's one of the choices
choose() {
        local var=$1 question=$2 suggestion=$3 choice choices
        shift 3
        choices="$*"
        while :; do
                ask "$var" "$question (${choices// /, })" "$suggestion"
                for choice; do [ "${!var}" = "$choice" ] && return; done
                [ -n "$interactive" ] || die "'${!var}' is not one of: $*"
                say "It's one of: $*"
        done
}

# confirm QUESTION: yes unless answered no; always yes without a terminal
confirm() {
        local ok
        [ -n "$interactive" ] || return 0
        read -rp "$1 [Y/n] " ok </dev/tty || die "cancelled"
        case $ok in
        '' | [yY]*) return 0 ;;
        *) return 1 ;;
        esac
}

# The default PREFIX: what's set, else ~/.local, or /usr/local as root (not
# root's ~/.local, nor, under a sudo that keeps HOME, root-owned files in the
# user's)
default_prefix() {
        if [ -n "${PREFIX:-}" ]; then
                say "$PREFIX"
        elif [ "$(id -u)" = 0 ]; then
                say /usr/local
        else
                say "$HOME/.local"
        fi
}

# Where everything goes, from PREFIX $1: ~ expanded (it's typed), relative
# to the current dir (the install runs from a temp dir)
set_prefix() {
        PREFIX=$1
        # shellcheck disable=SC2088 # the ~ typed, not expanded yet
        case $PREFIX in
        '~') PREFIX=$HOME ;;
        '~/'*) PREFIX=$HOME/${PREFIX#'~/'} ;;
        esac
        [ -n "$PREFIX" ] || die "the install dir can't be empty"
        case $PREFIX in
        /*) ;;
        *) PREFIX=$PWD/$PREFIX ;;
        esac
        [ "$PREFIX" = / ] || PREFIX=${PREFIX%/}
        BINDIR=$PREFIX/bin
        MANDIR=$PREFIX/share/man/man1
        BASHDIR=$PREFIX/share/bash-completion/completions
        ZSHDIR=$PREFIX/share/zsh/site-functions
        # fish only looks for a user's completions in its own config dir
        if [ "$PREFIX" = "$HOME/.local" ]; then
                FISHDIR=$HOME/.config/fish/completions
        else
                FISHDIR=$PREFIX/share/fish/vendor_completions.d
        fi
        # The AppImage's menu entry and icon (only with --appimage)
        APPSDIR=$PREFIX/share/applications
        ICONSDIR=$PREFIX/share/icons
        DESKTOP_FILES=("$APPSDIR/$NAME.desktop" "$ICONSDIR"/hicolor/*/apps/"$NAME".{svg,png})
        FILES=("$BINDIR/$NAME" "$MANDIR/$NAME.1" "$BASHDIR/$NAME" "$ZSHDIR/_$NAME" "$FISHDIR/$NAME.fish"
                "${DESKTOP_FILES[@]}")
}

# What's installed in PREFIX: "VERSION KIND", or nothing
installed() {
        local bin=$BINDIR/$NAME kind=static version
        [ -x "$bin" ] || return 0
        # AppImages have "AI" and their type (2) at offset 8
        [ "$(od -An -tx1 -j8 -N3 "$bin" 2>/dev/null | tr -d ' \n')" != 414902 ] || kind=appimage
        version=$("$bin" --version 2>/dev/null) && version=${version##* } || version=unknown
        say "$version $kind"
}

# The latest release's tag: releases/latest redirects to releases/tag/<tag>,
# or to releases if there are none. Nothing if it can't tell: the download
# says why, if it fails too.
latest() {
        local tag
        tag=$(curl -fsSLI -o /dev/null -w '%{url_effective}' "$GITHUB/$REPO/releases/latest" 2>/dev/null) || return 0
        tag=${tag##*/}
        [ "$tag" = releases ] || [ "$tag" = latest ] || say "$tag"
}

cleanup() {
        local f
        for f in "${staged[@]}"; do rm -f "$f.$NAME-new"; done
        [ -z "$tmp" ] || rm -rf "$tmp"
}

# Check FILE (in the current dir) against its line in SHA256SUMS
verify() {
        local line
        line=$(awk -v f="$1" '$2 == f || $2 == "*" f' SHA256SUMS)
        [ -n "$line" ] || { complain "$1 is not in the release's SHA256SUMS"; return 1; }
        printf '%s\n' "$line" | sha256sum -c --quiet - >/dev/null 2>&1 ||
                { complain "$1 doesn't match the release's SHA256SUMS"; return 1; }
}

# Why the release's attestation can't be checked, if it can't
no_attest() {
        if [ "$GITHUB" != https://github.com ]; then
                say "$GITHUB is not GitHub"
        elif ! command -v gh >/dev/null; then
                say "gh is not installed"
        elif ! gh auth status >/dev/null 2>&1; then
                say "gh is not logged in (gh auth login)"
        fi
}

# Check that SHA256SUMS was made by this repo's CI
attest() {
        gh attestation verify SHA256SUMS --repo "$REPO" \
                --signer-workflow "$REPO/.github/workflows/ci.yml" >/dev/null 2>&1 ||
                { complain "SHA256SUMS has no valid attestation from $REPO's CI (if the release is older than its attestations: --skip-attestation)"; return 1; }
        say "Checked the release's attestation"
}

# Every file into the current dir, checked: 1 if one doesn't check out. It
# runs in main's scope (url, bin, why, skip_attest).
download() {
        local f
        fetch "$url/SHA256SUMS" SHA256SUMS ||
                die "$REPO has no release $tag, or it has no SHA256SUMS (releases: $GITHUB/$REPO/releases)"
        if [ -z "$skip_attest" ] && [ -z "$why" ]; then attest || return 1; fi
        for f in "$bin" $extras; do
                fetch "$url/$f" "$f" || die "release $tag has no $f, though its SHA256SUMS has it"
                verify "$f" || return 1
        done
}

# Whether the release (its SHA256SUMS, in the current dir) has FILE. Every
# file but the binary is optional: a project may not make a man page,
# completions, an AppImage or a static binary.
has() {
        awk -v f="$1" '$2 == f || $2 == "*" f { found = 1 } END { exit !found }' SHA256SUMS
}

# Install SRC as DEST, but beside it, until commit puts everything in place
stage() {
        install -Dm"$1" "$2" "$3.$NAME-new"
        staged+=("$3")
}

commit() {
        local f
        for f in "${staged[@]}"; do mv -f "$f.$NAME-new" "$f"; done
        staged=()
}

uninstall() {
        local f found=()
        # Whatever is there: `make install` puts it in the same places
        for f in "${FILES[@]}"; do
                if [ -e "$f" ] || [ -L "$f" ]; then found+=("$f"); fi
        done
        if [ ${#found[@]} = 0 ]; then
                say "$NAME is not installed in $PREFIX"
                return
        fi
        say "Installed in $PREFIX:"
        printf '  %s\n' "${found[@]}"
        confirm "Remove them?" || die "cancelled"
        rm -f "${found[@]}"
        refresh_desktop
        say "Removed $NAME from $PREFIX"
}

# Update the menu and icon caches, but only the ones that already exist:
# making an icon cache would hide the icons of later apps that don't update it
refresh_desktop() {
        if [ -f "$APPSDIR/mimeinfo.cache" ] && command -v update-desktop-database >/dev/null; then
                update-desktop-database -q "$APPSDIR" 2>/dev/null || true
        fi
        if [ -f "$ICONSDIR/hicolor/icon-theme.cache" ] && command -v gtk-update-icon-cache >/dev/null; then
                gtk-update-icon-cache -q -t "$ICONSDIR/hicolor" 2>/dev/null || true
        fi
}

# Put the AppImage $1 in the applications menu: its .desktop file, run from
# where it was installed, and its icon. Extracting doesn't need FUSE.
integrate_desktop() {
        local root=squashfs-root/usr/share icon
        chmod +x "$1"
        "./$1" --appimage-extract >/dev/null 2>&1 && [ -f "$root/applications/$NAME.desktop" ] || {
                warn "couldn't get the menu entry out of the AppImage"
                return 0
        }
        mkdir -p "$APPSDIR"
        sed -e "s|^Exec=.*|Exec=\"$BINDIR/$NAME\"|" -e '/^TryExec=/d' \
                -e "/^Exec=/a TryExec=$BINDIR/$NAME" "$root/applications/$NAME.desktop" >"$APPSDIR/$NAME.desktop"
        for icon in "$root"/icons/hicolor/*/apps/"$NAME".*; do
                [ -f "$icon" ] && install -Dm644 "$icon" "$ICONSDIR/${icon#"$root"/icons/}"
        done
        refresh_desktop
        say "Added $NAME to the applications menu"
}

# The menu entry of an earlier --appimage install, now that it's the static binary
remove_desktop() {
        local f removed=''
        for f in "${DESKTOP_FILES[@]}"; do
                [ -e "$f" ] && rm -f "$f" && removed=1
        done
        [ -z "$removed" ] || { refresh_desktop; say "Removed the AppImage's applications menu entry"; }
}

# The whole script runs from here, at its last line: if the download is cut,
# bash runs nothing instead of half of it
main() {
        local tag='' kind='' yes='' strict='' skip_attest='' action=install
        local arch url bin version found arg prefix current newest why checks try extras
        local -a kinds

        for arg; do
                case $arg in
                -h | --help) usage; return ;;
                --appimage) kind=appimage ;;
                --static) kind=static ;;
                -y | --yes) yes=1 ;;
                --strict) strict=1 ;;
                --skip-attestation) skip_attest=1 ;;
                -*) die "unknown option $arg (see --help)" ;;
                uninstall) action=uninstall ;;
                *)
                        [ -z "$tag" ] || die "both $tag and $arg given, pick one"
                        tag=$arg
                        ;;
                esac
        done
        [ -z "$strict" ] || [ -z "$skip_attest" ] || die "--strict and --skip-attestation don't go together"
        if [ -z "$yes" ] && { : </dev/tty; } 2>/dev/null; then interactive=1; fi

        [ "$(uname -s)" = Linux ] || die "there are only Linux builds"

        [ -z "$interactive" ] || say "Installs $NAME from $GITHUB/$REPO (Enter takes the suggestion, Ctrl-C quits)"
        choose action "Install or uninstall" "$action" install uninstall
        prefix=$(default_prefix)
        # shellcheck disable=SC2088 # shown as typed, set_prefix expands it
        [ "$prefix" != "$HOME/.local" ] || prefix='~/.local'
        if [ "$action" = uninstall ]; then
                ask prefix "Uninstall from" "$prefix"
                set_prefix "$prefix"
                uninstall
                return
        fi
        ask prefix "Install to" "$prefix"
        set_prefix "$prefix"

        for arg in curl sha256sum awk tar gzip install mktemp od; do
                command -v "$arg" >/dev/null || die "needs $arg"
        done
        case $(uname -m) in
        x86_64 | amd64) arch=x86_64 ;;
        aarch64 | arm64) arch=aarch64 ;;
        *) die "there is no build for $(uname -m)" ;;
        esac

        current=$(installed)
        newest=$(latest)
        if [ -n "$current" ]; then
                say "Installed now: $NAME ${current% *} (${current#* }), the latest release is ${newest:-none}"
        else
                say "The latest release is ${newest:-none}"
        fi
        [ -n "$tag" ] || tag=${newest:-nightly}
        ask tag "Version (vX.Y.Z or nightly)" "$tag"
        case $tag in
        [0-9]*) tag=v$tag ;;
        '') die "the version can't be empty" ;;
        esac

        # What the release has, from its SHA256SUMS
        tmp=$(mktemp -d)
        trap cleanup EXIT
        cd "$tmp"
        url=$GITHUB/$REPO/releases/download/$tag
        fetch "$url/SHA256SUMS" SHA256SUMS ||
                die "$REPO has no release $tag, or it has no SHA256SUMS (releases: $GITHUB/$REPO/releases)"
        kinds=()
        if has "$NAME-$arch-static"; then kinds+=(static); fi
        if has "$NAME-$arch.AppImage"; then kinds+=(appimage); fi
        [ ${#kinds[@]} != 0 ] || die "release $tag has nothing for $arch"
        extras=''
        if has "$NAME.1"; then extras+=" $NAME.1"; fi
        if has "$NAME-completions.tar.gz"; then extras+=" $NAME-completions.tar.gz"; fi

        # The kind asked for, else the one installed now, else the static
        # binary (it runs anywhere), of the ones the release has
        if [ -n "$kind" ]; then
                [[ " ${kinds[*]} " == *" $kind "* ]] || die "release $tag has no $kind for $arch"
        else
                kind=${current#* }
                [[ " ${kinds[*]} " == *" $kind "* ]] || kind=${kinds[0]}
        fi
        if [ ${#kinds[@]} = 1 ]; then
                say "Kind: $kind, the only one release $tag has"
        else
                choose kind "Static binary or AppImage" "$kind" "${kinds[@]}"
        fi
        if [ "$kind" = appimage ]; then
                bin=$NAME-$arch.AppImage
        else
                bin=$NAME-$arch-static
        fi

        why=''
        [ -n "$skip_attest" ] || why=$(no_attest)
        if [ -n "$skip_attest" ]; then
                checks="SHA256SUMS only (--skip-attestation)"
        elif [ -n "$why" ]; then
                [ -z "$strict" ] || die "can't check the release's attestation: $why"
                checks="SHA256SUMS only: $why. It catches broken downloads, not a release whose files were all replaced"
        else
                checks="SHA256SUMS and the release's attestation"
        fi
        say "Checks: $checks"
        confirm "Install $NAME $tag ($kind, $arch) to $PREFIX?" || die "cancelled"

        say "Downloading $NAME $tag ($arch)"
        # CI replaces nightly's files one by one, so for a moment they may
        # not match its checksums yet: then it tries again
        for try in 1 2 3; do
                download && break
                [ "$tag" = nightly ] && [ "$try" -lt 3 ] || die "the download doesn't check out, nothing was installed"
                say "nightly may be being updated right now, trying again in 20 seconds"
                sleep 20
        done
        # Everything goes beside its place first, so a failure (a full disk,
        # a dir it can't write to) leaves the old install as it was
        stage 755 "$bin" "$BINDIR/$NAME"
        if [ -f "$NAME-completions.tar.gz" ]; then
                tar -xzf "$NAME-completions.tar.gz"
                stage 644 "$NAME-completions/$NAME.bash" "$BASHDIR/$NAME"
                stage 644 "$NAME-completions/_$NAME" "$ZSHDIR/_$NAME"
                stage 644 "$NAME-completions/$NAME.fish" "$FISHDIR/$NAME.fish"
        fi
        if [ -f "$NAME.1" ]; then stage 644 "$NAME.1" "$MANDIR/$NAME.1"; fi
        commit
        if [ "$kind" = appimage ]; then
                integrate_desktop "$bin"
        else
                remove_desktop
        fi

        if version=$("$BINDIR/$NAME" --version 2>/dev/null); then
                say "Installed $version to $BINDIR/$NAME"
        elif [ "$kind" = appimage ]; then
                say "Installed $bin to $BINDIR/$NAME, but it can't run: AppImages need FUSE"
                say "(the fuse or fuse3 package). Or install the static binary."
        else
                die "installed $BINDIR/$NAME, but it doesn't run"
        fi
        case ":$PATH:" in
        *":$BINDIR:"*) ;;
        *) say "$BINDIR is not in your PATH, add it to your shell's config" ;;
        esac
        found=$(type -P "$NAME" || true)
        if [ -n "$found" ] && ! [ "$found" -ef "$BINDIR/$NAME" ]; then
                warn "\`$NAME\` runs $found instead, it comes first in your PATH"
        fi
        case ${SHELL:-} in
        */zsh) [ ! -f "$NAME-completions.tar.gz" ] || say "For the zsh completions, add to ~/.zshrc before compinit: fpath=($ZSHDIR \$fpath)" ;;
        esac
}

main "$@"
