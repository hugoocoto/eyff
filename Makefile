# GNU make 4.2 or newer: macOS's own make is 3.81 (brew install make, and run
# gmake)
ifeq ($(filter-out 3.% 4.0 4.0.% 4.1 4.1.%,$(MAKE_VERSION)),)
$(error GNU make 4.2 or newer is needed, this is $(MAKE_VERSION) (on macOS: brew install make, and run gmake))
endif

# Project name: the binary, ~/.config/$(NAME)/ and everything named after it
NAME := eyff
# For the AppImage's menu entry
DESCRIPTION := Explore Your F*cking Files

# vX.Y.Z[-N-gHASH][-dirty] from git, else the VERSION file that `make dist`
# puts in tarballs, else nothing (and the code falls back to "unknown")
VERSION := $(shell git describe --tags --match 'v*' --always --dirty 2>/dev/null)
ifeq ($(VERSION),)
VERSION := $(shell cat VERSION 2>/dev/null)
endif
DATE := $(shell git log -1 --format=%cs 2>/dev/null || date +%F)
# Every date the build writes (tarballs, packages) is the last commit's,
# so building a commit again gives the same files
SOURCE_DATE_EPOCH ?= $(shell git log -1 --format=%ct 2>/dev/null || date +%s)
export SOURCE_DATE_EPOCH
# Tarballs with only what's in them: no dates, owners or umask (every file
# rw-r--r--, or rwxr-xr-x if it's executable or a dir)
TAR := tar --sort=name --mtime=@$(SOURCE_DATE_EPOCH) --owner=0 --group=0 --numeric-owner --mode=a=rX,u+w --format=gnu

PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man/man1
BASHCOMPDIR ?= $(PREFIX)/share/bash-completion/completions
ZSHCOMPDIR ?= $(PREFIX)/share/zsh/site-functions
# fish only looks for a user's completions in its own config dir
ifeq ($(PREFIX),$(HOME)/.local)
FISHCOMPDIR ?= $(HOME)/.config/fish/completions
else
FISHCOMPDIR ?= $(PREFIX)/share/fish/vendor_completions.d
endif

BUILD ?= build
BIN ?= $(NAME)
THIRDPARTY := src/thirdparty

# What the build downloads: kept between builds, and it is not in `make dist`
ARCH := $(shell uname -m)
TOOLS := $(BUILD)/tools
# Downloaded once into $(DL), which CI caches, and checked before every use
DL := $(TOOLS)/downloads

# $(call check,FILE,SHA256): stop, and remove FILE, unless it has that checksum
check = @echo '$(2)  $(1)' | sha256sum -c --quiet || { rm -f $(1); echo "$(1): wrong checksum, removed it" >&2; exit 1; }

ifeq ($(wildcard $(THIRDPARTY)/conf/CONF_FLAGS),)
$(error submodules missing, run: git submodule update --init)
endif
# Lua 5.4 (pkg-config's lua5.4). `make LUA=lua5.1` and `make LUA=luajit` also
# work: conf.h is written for 5.1, and src/lua_compat.h bridges it to 5.4.
LUA ?= lua5.4
# Sets LUA_CFLAGS and LUA_LIBS for $(LUA)
include $(THIRDPARTY)/conf/CONF_FLAGS

# Hardened by default: buffer overflow checks (_FORTIFY_SOURCE, which needs
# optimization, so `make debug` and `make test` leave it out), stack canaries,
# and a position independent binary, plus, on Linux and the other ELF
# systems, stack clash protection and read-only relocations, which macOS's
# compiler and linker don't have
HARDEN_CFLAGS := -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fPIE
HARDEN_LDFLAGS := -pie
ifneq ($(shell uname -s),Darwin)
HARDEN_CFLAGS += -fstack-clash-protection
HARDEN_LDFLAGS += -Wl,-z,relro,-z,now
endif
CFLAGS ?= -O2 $(HARDEN_CFLAGS)
LDFLAGS ?= $(HARDEN_LDFLAGS)
# `make WERROR=1` (CI) makes every warning an error
WARN = -Wall -Wextra -Wformat=2 -Wshadow $(if $(WERROR),-Werror)
# raylib: built from source, because its prebuilt Linux binaries are X11 only.
# BACKEND picks the GLFW backends compiled in; with both, GLFW follows the
# session (XDG_SESSION_TYPE): Wayland on Wayland, X11 on X11.
BACKEND ?= x11
ifneq ($(filter x11 wayland both,$(BACKEND)),$(BACKEND))
$(error BACKEND must be x11, wayland or both, this is $(BACKEND))
endif
RAYLIB_VERSION := 6.2
RAYLIB_SHA256 := df7a13129b1bec9e73a6962efff170beba310ee6eab8f94cd000b7316b4db5ee
RAYLIB_TARBALL := $(DL)/raylib-$(RAYLIB_VERSION).tar.gz
RAYLIB_DIR := $(TOOLS)/raylib-$(RAYLIB_VERSION)-$(BACKEND)
RAYLIB_LIB := $(RAYLIB_DIR)/src/libraylib.a
RAYLIB_CFLAGS := -isystem $(RAYLIB_DIR)/src
# OpenGL it loads at runtime; these are the windowing libraries of each backend
RAYLIB_LIBS_wayland := -lwayland-client -lwayland-cursor -lwayland-egl -lxkbcommon
RAYLIB_LIBS_x11 := -lX11
RAYLIB_LIBS_both := $(RAYLIB_LIBS_x11) $(RAYLIB_LIBS_wayland)
RAYLIB_LIBS := $(RAYLIB_LIB) $(RAYLIB_LIBS_$(BACKEND)) -lm -ldl -lrt
# The libraries are -isystem: their warnings are theirs, not this project's
CPPFLAGS += -D_DEFAULT_SOURCE -Isrc $(addprefix -isystem $(THIRDPARTY)/,flag cum conf) $(RAYLIB_CFLAGS) $(LUA_CFLAGS)
LDLIBS += $(LUA_LIBS) $(RAYLIB_LIBS) -pthread
ALL_CFLAGS = -std=c99 $(WARN) $(CFLAGS)
DEFINES = -DNAME='"$(NAME)"' $(if $(VERSION),-DVERSION='"$(VERSION)"')

# Runtime checks for `make debug` and `make test`. address finds memory errors
# and leaks, undefined finds undefined behavior, and thread finds data races
# and deadlocks. thread can't be combined with address, so CI runs the tests
# once with each: `make test SANITIZE=thread`.
SANITIZE ?= address,undefined
SAN_FLAGS = -fsanitize=$(SANITIZE) -fno-sanitize-recover=all -fno-omit-frame-pointer
DEBUG_FLAGS = -O0 -ggdb $(SAN_FLAGS)

SRC := $(shell find src -name '*.c' -not -path '$(THIRDPARTY)/*')
OBJ := $(SRC:%.c=$(BUILD)/%.o)
# -MMD leaves out the -isystem headers, so a submodule update wouldn't rebuild
LIB_HEADERS := $(wildcard $(THIRDPARTY)/*/*.h)

# Rebuild everything when the flags or the version change (e.g. after `make debug`)
FLAGS := $(CC) $(ALL_CFLAGS) $(CPPFLAGS) $(DEFINES) $(LDFLAGS) $(LDLIBS)
ifneq ($(FLAGS),$(file < $(BUILD)/.flags))
$(shell mkdir -p $(BUILD))
$(file > $(BUILD)/.flags,$(FLAGS))
endif

.PHONY: all debug test analyze format check-format install uninstall dist version clean distclean
.PHONY: release-arch release-common

all: $(BIN)

$(BIN): $(OBJ) $(BUILD)/.flags
	$(CC) $(LDFLAGS) $(OBJ) $(LDLIBS) -o $@

$(BUILD)/%.o: %.c $(BUILD)/.flags $(LIB_HEADERS) $(RAYLIB_LIB)
	@mkdir -p $(@D)
	$(CC) $(ALL_CFLAGS) $(CPPFLAGS) $(DEFINES) -MMD -MP -c $< -o $@

-include $(OBJ:.o=.d)

# raylib's source tarball, built where $(RAYLIB_CFLAGS) and $(RAYLIB_LIBS)
# point at it: an object file may include raylib.h, so every one waits for it
$(RAYLIB_TARBALL):
	@mkdir -p $(@D)
	curl -fsSL -o $@ https://github.com/raysan5/raylib/archive/refs/tags/$(RAYLIB_VERSION).tar.gz

# raylib's own Makefile takes no CFLAGS from here (make test and make debug
# pass theirs on the command line, and it must not build raylib with them), and
# a clean MAKEFLAGS drops the jobserver, so -j again
$(RAYLIB_LIB): $(RAYLIB_TARBALL)
	@test "$(shell uname -s)" = Linux || { echo "raylib: its X11 and Wayland backends are built on Linux only" >&2; exit 1; }
	$(call check,$(RAYLIB_TARBALL),$(RAYLIB_SHA256))
	rm -rf $(RAYLIB_DIR)
	mkdir -p $(RAYLIB_DIR)
	tar -xzf $(RAYLIB_TARBALL) -C $(RAYLIB_DIR) --strip-components=1
	env -u MAKEFLAGS -u MFLAGS $(MAKE) -C $(RAYLIB_DIR)/src -j$(shell nproc 2>/dev/null || echo 1) \
		PLATFORM=PLATFORM_DESKTOP RAYLIB_LIBTYPE=STATIC CC='$(CC)' \
		GLFW_LINUX_ENABLE_X11=$(if $(filter x11 both,$(BACKEND)),TRUE,FALSE) \
		GLFW_LINUX_ENABLE_WAYLAND=$(if $(filter wayland both,$(BACKEND)),TRUE,FALSE)
	touch $@ # the archive's own timestamps are older than the tarball

debug:
	$(MAKE) CFLAGS='$(DEBUG_FLAGS)' LDFLAGS='$(SAN_FLAGS)'
# The program and every test/*.c are built with the sanitizers, in their own
# build dir so the normal build is left alone. scripts/test.sh runs the tests.
comma := ,
TEST_BUILD := $(BUILD)/test/$(subst $(comma),-,$(SANITIZE))
test: export TEST_CC = $(CC)
test: export TEST_CFLAGS = -std=c99 $(WARN) -O1 -ggdb $(SAN_FLAGS) $(CPPFLAGS)
test: export TEST_LDLIBS = $(SAN_FLAGS) $(LDLIBS)
test: export TEST_BIN = $(TEST_BUILD)/$(NAME)
test: export TEST_OUT = $(TEST_BUILD)/tests
test:
	@$(MAKE) --no-print-directory BUILD=$(TEST_BUILD) BIN=$(TEST_BUILD)/$(NAME) TOOLS=$(abspath $(TOOLS)) \
		CFLAGS='-O1 -ggdb $(SAN_FLAGS)' LDFLAGS='$(SAN_FLAGS)' $(TEST_BUILD)/$(NAME)
	@scripts/test.sh

# clang's static analyzer: it follows every path through the code, also the
# ones no test runs, looking for leaks, NULL dereferences, use after free...
# Any finding fails. Each function is analyzed on its own (ipa=none): by
# default only main is, following its calls, and the header libraries' code
# uses up its budget before it gets far (a leak in find_config went unseen,
# and gcc -fanalyzer misses it too).
ANALYZE_CC ?= clang
ANALYZE_FLAGS = --analyze -Xanalyzer -analyzer-werror -Xanalyzer -analyzer-config -Xanalyzer ipa=none
analyze:
	@for f in $(SRC) $(wildcard test/*.c); do \
		echo "analyze $$f"; \
		$(ANALYZE_CC) $(ANALYZE_FLAGS) -std=c99 $(CPPFLAGS) $(DEFINES) $$f -o /dev/null || exit 1; \
	done

# clang-format with .clang-format. CI checks with clang-format $(CLANG_FORMAT_VERSION)
# (pipx install clang-format==$(CLANG_FORMAT_VERSION)): other versions may format differently.
CLANG_FORMAT ?= clang-format
CLANG_FORMAT_VERSION := 22.1.8
FORMAT_SRC = $(shell find src test -name '*.[ch]' -not -path '$(THIRDPARTY)/*')

format:
	$(CLANG_FORMAT) -i $(FORMAT_SRC)

check-format:
	$(CLANG_FORMAT) --dry-run -Werror $(FORMAT_SRC)

.PHONY: docs
docs:
	@find docs -name '*.typ' | while read -r f; do echo "typst compile $$f"; typst compile "$$f" || exit 1; done

COMPLETIONS := $(BUILD)/$(NAME)-completions
$(COMPLETIONS): $(BIN)
	rm -rf $@ && mkdir -p $@
	$(abspath $(BIN)) --completions bash >$@/$(NAME).bash
	$(abspath $(BIN)) --completions zsh >$@/_$(NAME)
	$(abspath $(BIN)) --completions fish >$@/$(NAME).fish

# The man page with its version and date, $(NAME).1 (attached to releases)
.PHONY: man
man: $(BUILD)/$(NAME).1
	cp $< $(NAME).1

$(BUILD)/$(NAME).1: docs/$(NAME).1 $(BUILD)/.flags
	sed 's/@VERSION@/$(or $(VERSION),unknown)/; s/@DATE@/$(DATE)/' $< >$@

install: $(BUILD)/$(NAME).1
install: $(COMPLETIONS)

# install -d, then install: macOS's install has no -D
install: $(NAME)
	install -d $(DESTDIR)$(BINDIR)
	install -m755 $(NAME) $(DESTDIR)$(BINDIR)/$(NAME)
	install -d $(DESTDIR)$(MANDIR)
	install -m644 $(BUILD)/$(NAME).1 $(DESTDIR)$(MANDIR)/$(NAME).1
	install -d $(DESTDIR)$(BASHCOMPDIR) $(DESTDIR)$(ZSHCOMPDIR) $(DESTDIR)$(FISHCOMPDIR)
	install -m644 $(COMPLETIONS)/$(NAME).bash $(DESTDIR)$(BASHCOMPDIR)/$(NAME)
	install -m644 $(COMPLETIONS)/_$(NAME) $(DESTDIR)$(ZSHCOMPDIR)/_$(NAME)
	install -m644 $(COMPLETIONS)/$(NAME).fish $(DESTDIR)$(FISHCOMPDIR)/$(NAME).fish

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(NAME)
	rm -f $(DESTDIR)$(MANDIR)/$(NAME).1
	rm -f $(DESTDIR)$(BASHCOMPDIR)/$(NAME) $(DESTDIR)$(ZSHCOMPDIR)/_$(NAME) $(DESTDIR)$(FISHCOMPDIR)/$(NAME).fish

# What CI packages and releases: release-arch on each arch, release-common
# (what's the same on every arch) once
release-common: dist
release-common: man
release-common: completions
release-arch: appimage

# Bundles every shared library the binary needs (except glibc and friends, see
# linuxdeploy's excludelist) into $(NAME)-$(ARCH).AppImage
.PHONY: appimage
APPDIR := $(BUILD)/AppDir
APPIMAGE := $(NAME)-$(ARCH).AppImage
# The tools are AppImages too: run them without FUSE (CI has none)
export APPIMAGE_EXTRACT_AND_RUN = 1

# The tools, pinned to a release and checked against its checksums (from
# GitHub's asset digests) before every use: they end up in what's released.
# The AppImage runtime is pinned too, or appimagetool would download its
# latest one.
LINUXDEPLOY_VERSION := 1-alpha-20251107-1
LINUXDEPLOY_SHA256_x86_64 := c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
LINUXDEPLOY_SHA256_aarch64 := 620095110d693282b8ebeb244a95b5e911cf8f65f76c88b4b47d16ae6346fcff
LINUXDEPLOY := $(DL)/linuxdeploy-$(LINUXDEPLOY_VERSION)-$(ARCH).AppImage
APPIMAGETOOL_VERSION := 1.9.1
APPIMAGETOOL_SHA256_x86_64 := ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
APPIMAGETOOL_SHA256_aarch64 := f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158
APPIMAGETOOL := $(DL)/appimagetool-$(APPIMAGETOOL_VERSION)-$(ARCH).AppImage
RUNTIME_VERSION := 20251108
RUNTIME_SHA256_x86_64 := 2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
RUNTIME_SHA256_aarch64 := 00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444
RUNTIME := $(DL)/runtime-$(RUNTIME_VERSION)-$(ARCH)

# linuxdeploy's own strip is too old for the libraries of newer distros
# (.relr.dyn sections), so the system's strips what it bundles
STRIP ?= strip

appimage: $(APPIMAGE)

$(APPIMAGE): $(NAME) assets/icon.svg $(LINUXDEPLOY) $(APPIMAGETOOL) $(RUNTIME)
	$(call check,$(LINUXDEPLOY),$(LINUXDEPLOY_SHA256_$(ARCH)))
	$(call check,$(APPIMAGETOOL),$(APPIMAGETOOL_SHA256_$(ARCH)))
	$(call check,$(RUNTIME),$(RUNTIME_SHA256_$(ARCH)))
	chmod +x $(LINUXDEPLOY) $(APPIMAGETOOL)
	rm -rf $(APPDIR)
	mkdir -p $(APPDIR)
	printf '[Desktop Entry]\nType=Application\nName=%s\nComment=%s\nExec=%s\nIcon=%s\nTerminal=true\nCategories=Utility;\n' \
		'$(NAME)' '$(subst ','\'',$(DESCRIPTION))' '$(NAME)' '$(NAME)' >$(BUILD)/$(NAME).desktop
	cp assets/icon.svg $(BUILD)/$(NAME).svg
	NO_STRIP=1 $(LINUXDEPLOY) --appdir $(APPDIR) --executable $(NAME) \
		--desktop-file $(BUILD)/$(NAME).desktop --icon-file $(BUILD)/$(NAME).svg
	find $(APPDIR)/usr/bin $(APPDIR)/usr/lib -type f -exec $(STRIP) --strip-unneeded {} +
	ARCH=$(ARCH) $(APPIMAGETOOL) --no-appstream --runtime-file $(RUNTIME) $(APPDIR) $@

$(LINUXDEPLOY):
	@mkdir -p $(@D)
	curl -fsSL -o $@ https://github.com/linuxdeploy/linuxdeploy/releases/download/$(LINUXDEPLOY_VERSION)/linuxdeploy-$(ARCH).AppImage

$(APPIMAGETOOL):
	@mkdir -p $(@D)
	curl -fsSL -o $@ https://github.com/AppImage/appimagetool/releases/download/$(APPIMAGETOOL_VERSION)/appimagetool-$(ARCH).AppImage

$(RUNTIME):
	@mkdir -p $(@D)
	curl -fsSL -o $@ https://github.com/AppImage/type2-runtime/releases/download/$(RUNTIME_VERSION)/runtime-$(ARCH)

# Source tarball with the submodules and a VERSION file (GitHub's own
# tarballs have neither): what git tracks, but what's deleted already
dist:
	@test -n "$(VERSION)" || { echo "dist: no version, not a git checkout?"; exit 1; }
	rm -rf $(BUILD)/dist && mkdir -p $(BUILD)/dist/$(NAME)-$(VERSION)
	git ls-files --recurse-submodules | while IFS= read -r f; do [ ! -e "$$f" ] || echo "$$f"; done | \
		tar -cf - -T - | tar -xf - -C $(BUILD)/dist/$(NAME)-$(VERSION)
	echo $(VERSION) >$(BUILD)/dist/$(NAME)-$(VERSION)/VERSION
	$(TAR) -cf - -C $(BUILD)/dist $(NAME)-$(VERSION) | gzip -9n >$(NAME)-$(VERSION).tar.gz

# The bash, zsh and fish completions, printed by the program itself
# (`--completions SHELL`, from flag.h), so they have every flag, and released
# beside the binaries as $(NAME)-completions.tar.gz
.PHONY: completions
completions: $(NAME)-completions.tar.gz

$(NAME)-completions.tar.gz: $(COMPLETIONS)
	$(TAR) -cf - -C $(BUILD) $(NAME)-completions | gzip -9n >$@

# Grouped commit messages since the first release, like each release has
.PHONY: changelog
changelog:
	@mkdir -p $(BUILD)
	scripts/changelog.sh >$(BUILD)/CHANGELOG.md
	mv $(BUILD)/CHANGELOG.md CHANGELOG.md

version:
	@echo $(or $(VERSION),unknown)

# Run the tests before every `git push`
.PHONY: hooks
hooks:
	git config core.hooksPath scripts/hooks

# What the build makes. The downloaded tools and libraries are kept.
clean:
	rm -rf $(NAME) $(NAME).1 CHANGELOG.md $(BUILD)/.flags $(BUILD)/src $(BUILD)/test $(BUILD)/AppDir $(BUILD)/dist $(BUILD)/$(NAME).* $(BUILD)/$(NAME)-completions

# Back to a fresh clone: also the downloads and every package
distclean: clean
	rm -rf $(BUILD) $(NAME)-*.tar.gz
	rm -f $(NAME)-*.AppImage
	find docs -name '*.pdf' -delete
