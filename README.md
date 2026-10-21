# eyff

[![CI](https://github.com/hugoocoto/eyff/actions/workflows/ci.yml/badge.svg)](https://github.com/hugoocoto/eyff/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/hugoocoto/eyff)](https://github.com/hugoocoto/eyff/releases/latest)
[![License](https://img.shields.io/github/license/hugoocoto/eyff)](LICENSE)

Explore Your F*cking Files.

## Install

```sh
curl -fsSL https://github.com/hugoocoto/eyff/releases/latest/download/install.sh | bash
```

It asks what to install and where, with its suggestion filled in (Enter
takes it): the latest release, in `~/.local` (`/usr/local` as root), the
binary for your machine (x86_64 or aarch64 Linux) with what comes with it.
Everything is checked against the release's `SHA256SUMS`, and if
[`gh`](https://cli.github.com) is installed and logged in, `SHA256SUMS` is
checked against the release's attestation, proof that CI built it from this
repo. Nothing is replaced until it all checks out. Arguments, after
`bash -s --`, change the suggestions, and `--yes` takes them without asking
(`--help` lists them all):

```sh
curl -fsSL https://github.com/hugoocoto/eyff/releases/latest/download/install.sh | bash -s -- v1.2.3     # that release
curl -fsSL https://github.com/hugoocoto/eyff/releases/latest/download/install.sh | bash -s -- nightly    # the last commit on main
curl -fsSL https://github.com/hugoocoto/eyff/releases/latest/download/install.sh | bash -s -- --strict   # fail without the attestation check
curl -fsSL https://github.com/hugoocoto/eyff/releases/latest/download/install.sh | bash -s -- uninstall
curl -fsSL https://github.com/hugoocoto/eyff/releases/latest/download/install.sh | bash -s -- --yes      # no questions
curl -fsSL https://github.com/hugoocoto/eyff/releases/latest/download/install.sh | PREFIX=~/opt bash     # somewhere else
```

It installs the AppImage as `eyff`, added to your applications menu, with
its icon. It needs FUSE (the `fuse` or `fuse3` package).

`uninstall` removes it all, also what `make install` put in the same places.
Releases made before attestations need `--skip-attestation`.

The script comes from the latest release, attested like everything in it. To
check it before running it:

```sh
curl -fsSLO https://github.com/hugoocoto/eyff/releases/latest/download/install.sh
gh attestation verify install.sh --repo hugoocoto/eyff && bash install.sh
```

The same checks any file you downloaded yourself: `gh attestation verify <file> --repo hugoocoto/eyff`.

If `~/.local/bin` is not in your `PATH` yet, add it to your shell's config.
For the zsh completions, add this to `~/.zshrc`, before `compinit`:

```zsh
fpath=(~/.local/share/zsh/site-functions $fpath)
```

### By hand

Every [release](../../releases/latest) has, for x86_64 and aarch64:

| File                      | What                                           |
|---------------------------|------------------------------------------------|
| `eyff-<arch>.AppImage`    | the program, as an AppImage                    |
| `eyff-completions.tar.gz` | bash, zsh and fish completions                 |
| `eyff.1`                  | the man page                                   |
| `eyff-<version>.tar.gz`   | the source code, with the submodules           |
| `*.pdf`                   | the documentation                              |
| `install.sh`              | the install script                             |
| `CHANGELOG.md`            | what changed in every release                  |
| `SHA256SUMS`              | checksums of all of the above                  |

The [nightly](../../releases/tag/nightly) release has the same files, built
from the last commit on main. Check what you downloaded first:

```sh
sha256sum -c --ignore-missing SHA256SUMS
```

```sh
install -Dm755 eyff-x86_64.AppImage ~/.local/bin/eyff    # or eyff-aarch64.AppImage
```

```sh
install -Dm644 eyff.1 ~/.local/share/man/man1/eyff.1
```

Or have `eyff` print them, e.g. `source <(eyff --completions bash)` in
`~/.bashrc` (also `zsh` and `fish`):

```sh
tar -xzf eyff-completions.tar.gz && cd eyff-completions
install -Dm644 eyff.bash ~/.local/share/bash-completion/completions/eyff
install -Dm644 _eyff ~/.local/share/zsh/site-functions/_eyff
install -Dm644 eyff.fish ~/.config/fish/completions/eyff.fish
```

### From source

See [Building](#building). `make install` installs it in `~/.local` (or
`PREFIX`).

## Usage

<!-- Your program's --help, exactly: `make test` fails until it is (see
Documentation in CONTRIBUTING.md). -->

```
usage: eyff [-h] [--completions C] [-v] [-c C]

Explore Your F*cking Files

options:
 --help, -h       Show this help
 --completions C  Print completions for bash, zsh or fish
 --version, -v    Show version and exit
 --config, -c C   Use this config file
```

`man eyff` has the full manual.

## Configuration

<!-- What the config can have. Where it's read from is the same in every
project. -->

The config is a Lua 5.4 file: `$XDG_CONFIG_HOME/eyff/config.lua`
(usually `~/.config/eyff/config.lua`), or the one given with `-c`. Without
one, the defaults are used. A `config.lua` in the current directory is never
read on its own: it can run any command, so running `eyff` inside a
downloaded directory would run that directory's code. Use `-c config.lua` for
that one.

```lua
Config = {
}
```

It's plain Lua, so it can compute values, read environment variables, etc.

## Building

Needs a C compiler, make and Lua 5.4 with its headers (or Lua 5.1 or
LuaJIT, with `make LUA=lua5.1` or `make LUA=luajit`):

| Distro        | Packages                                                       |
|---------------|----------------------------------------------------------------|
| Arch          | `lua54 libx11 wayland libxkbcommon mesa`                       |
| Debian/Ubuntu | `liblua5.4-dev libx11-dev libwayland-dev libxkbcommon-dev libgl1-mesa-dev` |

`BACKEND` picks raylib's windowing: `x11` (the default), `wayland`, or `both`
(one binary, which follows the session: Wayland on Wayland, X11 on X11). The
X11 build also runs on Wayland desktops, through XWayland. The first build
downloads raylib's source, pinned in the Makefile and checked against its
tarball's checksum, and builds it, so it needs the network once.

```sh
git clone --recursive https://github.com/hugoocoto/eyff eyff
cd eyff
make
./eyff --version
```

| Command                     | What it does                                            |
|-----------------------------|---------------------------------------------------------|
| `make`                      | build `./eyff` (objects in `build/`), hardened          |
| `make WERROR=1`             | the same, failing on any warning, like CI               |
| `make LUA=luajit`           | the same, with LuaJIT (or `LUA=lua5.1`)                 |
| `make BACKEND=wayland`      | the same, native Wayland (or `BACKEND=both`)            |
| `make debug`                | `-O0 -ggdb` with the address and undefined sanitizers   |
| `make test`                 | build and run the tests, with the same sanitizers       |
| `make test SANITIZE=thread` | the tests again, looking for data races                 |
| `make format`               | format the code with clang-format                       |
| `make check-format`         | check it's formatted, like CI does                      |
| `make install`              | install it to `~/.local` (`PREFIX`, `DESTDIR`)          |
| `make uninstall`            | remove them                                             |
| `make appimage`             | `eyff-<arch>.AppImage` (downloads its pinned tools)     |
| `make completions`          | `eyff-completions.tar.gz`, from `eyff --completions`    |
| `make dist`                 | `eyff-<version>.tar.gz`, the source with the submodules |
| `make man`                  | `eyff.1`, the man page with its version                 |
| `make docs`                 | `docs/*.typ` to PDF (needs typst)                       |
| `make changelog`            | `CHANGELOG.md`, from the commit messages                |
| `make clean`                | remove what the build made                              |
| `make distclean`            | also the downloads and everything packaged              |

If you cloned without `--recursive`: `git submodule update --init`.

## Versions

`eyff --version` shows the version, from `git describe`: `v1.2.3` on a
release, `v1.2.3-4-gabcdef0` four commits after it, `-dirty` if built with
uncommitted changes, and `unknown` outside git.

- Every push to main that passes CI updates the
  [nightly](../../releases/tag/nightly) release.
- Releases are made with `scripts/release.sh`: it asks for the new version
  (bigger than the last one), suggesting the next one from the commits since
  the last release (major if they remove things, minor if they add things,
  else patch), tags main with it and pushes the tag, and CI
  builds and publishes the release.
- Each release's notes list its commits, grouped in Added, Changed, Fixed and
  Removed by their first word, and `CHANGELOG.md` in every release has them
  for all releases.

## CI

CI builds and tests every push to main and every pull request with gcc and
clang, each with AddressSanitizer, LeakSanitizer and
UndefinedBehaviorSanitizer, and again with ThreadSanitizer: a memory error,
leak, undefined behavior or data race fails it, and so does any compiler
warning or anything clang's static analyzer finds. It also checks the code
is formatted and the docs match `--help`, runs every file it packages, and
builds the source tarball on its own.
It runs the install script on what it packaged before anything is
published, and again on each release once it is.

- The build is hardened by default: `_FORTIFY_SOURCE=3`, stack protector
  and PIE, and on Linux, stack clash protection and full RELRO.
- Everything the build downloads is pinned to a version and checked against
  its sha256 before every use, and the GitHub Actions are pinned to commits.
- The tarballs are reproducible: built again from the same commit, they're
  the same bytes. CI builds them twice to check.
- Every release file is attested (signed as built by this workflow, from its
  commit), so it can be checked to be what CI built from this repo.

Dependabot keeps the GitHub Actions (their pinned commits) and the
submodules up to date with weekly pull requests.
The tools in the Makefile are updated by hand: their versions and checksums
are together there.

## Contributing

Bug reports, ideas and pull requests are welcome.

- **Found a bug?** [Open an issue](../../issues/new/choose) with what you
  ran, what happened, what you expected, and the output of
  `eyff --version`.
- **Want a feature?** Open an issue first, so we can agree on it before you
  write the code.
- **Sending code?** Read [CONTRIBUTING.md](CONTRIBUTING.md): it has the
  setup, the tests and what a pull request needs.

## License

Copyright (C) 2026 Hugo Coto

GPL-3.0-or-later, see [LICENSE](LICENSE).
The libraries in `src/thirdparty/` keep their own licenses, and so does
raylib (zlib), which the build downloads.
