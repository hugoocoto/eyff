#!/usr/bin/env bash
# Build and run every test/*.c, and run every test/*.sh, in random order. Each
# one is a self-contained program (or bash script) that exits 0 if it passes
# and non-zero if it fails. This script exits non-zero if any test fails to
# build or run.
#
# Run it as `make test` (or directly: it calls make itself), which builds the
# program and the tests with sanitizers and sets TEST_CC, TEST_CFLAGS,
# TEST_LDLIBS, TEST_OUT (where the test binaries go) and TEST_BIN (the program,
# for tests that run it). TEST_TIMEOUT (seconds, default 10) limits each test.
#
# Plain bash 3.2 (macOS's): no mapfile.
set -u
cd "$(dirname "$0")/.." || exit 1
[ -n "${TEST_CC:-}" ] || exec make --no-print-directory test

# Random order, so no test depends on another running first, and a time
# limit for each. macOS has neither shuf nor timeout: there they run in order,
# with no limit.
shuffle() { if command -v shuf >/dev/null; then shuf; else cat; fi; }
limit=()
if command -v timeout >/dev/null; then limit=(timeout "${TEST_TIMEOUT:-10}"); fi

mkdir -p "$TEST_OUT"
tests=()
while IFS= read -r t; do tests+=("$t"); done < <(find test -maxdepth 1 \( -name '*.c' -o -name '*.sh' \) | shuffle)
failed=()

for t in "${tests[@]}"; do
        name=$(basename "$t" .c)
        bin=$TEST_OUT/$name
        case $t in
        *.sh)
                run=(bash "$t")
                ;;
        *)
                run=("$bin")
                # The flags are word-split on purpose
                # shellcheck disable=SC2086
                if ! $TEST_CC $TEST_CFLAGS "$t" -o "$bin" $TEST_LDLIBS 2>"$bin.log"; then
                        echo "FAIL $name (build)"
                        sed 's/^/    /' "$bin.log"
                        failed+=("$name")
                        continue
                fi
                ;;
        esac
        if ${limit[@]+"${limit[@]}"} "${run[@]}" >"$bin.log" 2>&1; then
                echo "ok   $name"
        else
                status=$?
                [ $status -eq 124 ] && status="timeout" || status="exit $status"
                echo "FAIL $name ($status)"
                sed 's/^/    /' "$bin.log"
                failed+=("$name")
        fi
done

echo "${#tests[@]} tests, ${#failed[@]} failed${failed[*]:+: ${failed[*]}}"
[ ${#failed[@]} -eq 0 ]
