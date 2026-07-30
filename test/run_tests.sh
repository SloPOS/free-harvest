#!/usr/bin/env bash
# Build and run the host-side unit tests for the portable protocol core.
# Requires only a C11 compiler - no ESP-IDF, no hardware.
set -u

cd "$(dirname "$0")/.."
CC=${CC:-gcc}
CFLAGS="-std=c11 -Wall -Wextra -Werror -g -Icomponents/hr_protocol/include -Itest"
SRC=$(ls components/hr_protocol/*.c)
OUT_DIR="${TMPDIR:-/tmp}/hr_tests"
mkdir -p "$OUT_DIR"

status=0
for t in test/test_*.c; do
    name=$(basename "$t" .c)
    echo "=== $name ==="
    if ! $CC $CFLAGS -o "$OUT_DIR/$name.exe" $SRC "$t"; then
        echo "  BUILD FAILED"
        status=1
        continue
    fi
    "$OUT_DIR/$name.exe" || status=1
    echo
done

if [ $status -eq 0 ]; then
    echo "ALL TESTS PASSED"
else
    echo "TESTS FAILED"
fi
exit $status
