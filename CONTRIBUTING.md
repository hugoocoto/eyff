# Contributing to eyff

Thanks for helping! Bug reports, ideas, docs and code are all welcome.

## Reporting bugs

[Open an issue](../../issues/new/choose). The more of this it has, the
faster it gets fixed:

- what you ran (the command, and the config if it matters),
- what happened (the full output) and what you expected,
- the output of `eyff --version`, and how you installed it,
- your distro and architecture.

Search the open issues first: someone may have reported it already.

A security problem goes privately instead, not in an issue: see
[SECURITY.md](.github/SECURITY.md).

## Suggesting features

Open an issue describing the problem you want to solve, not just the
solution. For anything that needs more than a few lines of code, agree on it
in the issue before writing it: it saves everyone's time.

## Setting up

You need a C compiler (gcc or clang), make and Lua 5.4 (or 5.1, or LuaJIT)
with its headers: see [Building](README.md#building).

```sh
git clone --recursive https://github.com/hugoocoto/eyff eyff
cd eyff
make hooks   # run the tests before every push
make debug   # build with the sanitizers while working
make test
```

Editors that use clangd get the right flags from `src/.clangd` and
`test/.clangd`, and the code style from `.clang-format` and `.editorconfig`.

### Project layout

```
src/                     the program, every .c is compiled to build/src/*.o
src/thirdparty/          git submodules: flag.h (flags), cum.h (macros), conf.h (Lua config)
build/tools/             what the build downloads and builds: raylib, the release tools
test/                    one self-contained test per .c (a program) or .sh (a bash script)
scripts/test.sh          runs the tests (make test)
scripts/github-setup.sh  sets up the GitHub repo's settings, with gh
docs/eyff.1          the man page
docs/*.typ               Typst documents, to PDF with make docs
assets/                  the AppImage and file-tree icons
scripts/install.sh       the install script, attached to every release
scripts/test-install.sh  tests it on a release (CI)
scripts/release.sh       tags a release (scripts/changelog.sh writes its notes)
scripts/hooks/           git hooks (make hooks)
```

## Making a change

1. Fork the repo and make a branch from main.
2. Make the change, with a test (see [Tests](#tests)).
3. Update the docs if it changes what users see (see [Documentation](#documentation)).
4. Check it: `make test`, `make test SANITIZE=thread` and `make check-format`.
5. Open a pull request, saying what it changes and why. Link the issue it
   fixes (`Fixes #123`).

CI runs the tests with gcc and clang, with every sanitizer. A pull request is
merged when CI is green and it has been reviewed. Small pull requests are
reviewed much faster than big ones: split them when you can.

## Code style

- C99 (`-std=c99`), with POSIX (`_DEFAULT_SOURCE`). No warnings with
  `-Wall -Wextra -Wformat=2 -Wshadow`: CI builds with `make WERROR=1`, which
  makes them errors (do the same to check). Warnings in `src/thirdparty/`
  don't count, its headers are included with `-isystem`.
- Format with `make format` (it leaves `src/thirdparty/` alone). CI checks it
  with the clang-format version in the Makefile (`CLANG_FORMAT_VERSION`): if
  yours formats differently, install that one with
  `pipx install clang-format==<version>` and run
  `make format CLANG_FORMAT=~/.local/bin/clang-format`.
- Functions that can fail return 0 on success and non-zero on error, and
  print the error themselves, starting with the program's name.
- Free what you allocate: the sanitizers fail the tests on any leak.
- Comments say why, not what. Keep them short.

## Tests

Every `test/*.c` is a self-contained program, and every `test/*.sh` a bash
script: it exits 0 if it passes and anything else if it fails. `make test`
builds the program and each C test with sanitizers, then `scripts/test.sh`
runs the tests in random order from the project root with a 10s timeout, and
fails if any of them does.

The sanitizers make a test fail on things that would otherwise pass silently:

- `make test` (address,undefined): out of bounds accesses, use after free,
  double free, memory leaks, signed overflow, misaligned or NULL pointers...
- `make test SANITIZE=thread`: data races and lock order inversions. It
  can't be combined with the address sanitizer, so it's a separate run.

Writing tests:

- Tests can include anything in `src/`, `src/thirdparty/` and raylib (but
  don't open a window: CI has no display), and link with pthreads, Lua and
  raylib.
- `$TEST_BIN` is the program, also built with the sanitizers, for tests that
  run it: a leak in the program fails those tests too.
- Shell tests are for what's easier to check from outside, like
  `test/docs.sh`, which checks the docs against `--help`.
- Tests don't depend on each other or on the order they run in.

## Documentation

A change users can see updates, in the same pull request:

- `--help` (in `src/main.c`) and Usage in the README, for a flag
- `config.lua` and Configuration in the README, for the config
- the man page, `docs/eyff.1` (check it with `man docs/eyff.1`)
- the manual, `docs/*.typ`

`make test` checks them against `--help` (`test/docs.sh`): the README's
Usage must be exactly what it prints, and the other docs must have every
flag.
The shell completions come from flag.h (`eyff --completions SHELL`), so they
always have every flag.

## Commit messages

One change per commit. The first line says what it does, in under 72
characters (`Add --quiet flag`, `Fix crash on empty config`). If it isn't
obvious, the body says why.

The first lines are the changelog and the release notes
(`scripts/changelog.sh`, `make changelog` to see it), grouped by their first
word:

| Starts with                                  | Goes in  |
|----------------------------------------------|----------|
| Add, New, Implement, Introduce, Support      | Added    |
| Fix, Correct, Repair                         | Fixed    |
| Remove, Delete, Drop                         | Removed  |
| anything else (Change, Update, Make...)      | Changed  |

So write them for users: `Fix crash on empty config`, not `fix bug`.

## Third-party code

`src/thirdparty/` holds git submodules. Fix bugs in them upstream; to update
one by hand:

```sh
git -C src/thirdparty/flag pull origin main
git add src/thirdparty/flag
```

Dependabot also opens a pull request every week when they have new commits,
and CI tests it.

raylib is not a submodule: the Makefile downloads its source from GitHub,
checks it against a SHA-256, and builds it with the GLFW backends `BACKEND`
asks for (`x11` by default, `wayland`, or `both`). To update it, change
`RAYLIB_VERSION` and `RAYLIB_SHA256` in the Makefile. To add a backend, add
its libraries to `RAYLIB_LIBS_<name>`.

## Releases

Maintainers release from main with `scripts/release.sh`, which asks for the
version (semantic versioning: X.Y.Z, bigger than the last one), suggesting the
next one from the commits' first words (Remove... makes it major, Add...
minor, the rest a patch; before 1.0.0, removing is minor too), and pushes its
tag. CI then builds and publishes the release. Every push to main updates the
nightly release.

## License

By contributing you agree that your work is licensed under GPL-3.0-or-later,
like the rest of the project.
