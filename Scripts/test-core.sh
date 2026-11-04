#!/bin/sh
# Build and run the C core unit tests with AddressSanitizer and UBSan.
# Needs only the Xcode command line tools.
#   Scripts/test-core.sh                   build and run every test
#   Scripts/test-core.sh --update-configs  rewrite configs/*.conf from the schema
#                                          (after changing settings/config_schema.c)
set -eu
cd "$(dirname "$0")/.."
OUT=build/test-core
mkdir -p "$OUT"
xcrun clang -std=c17 -g -O1 -fno-omit-frame-pointer \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
    -ISources/Core/include -ISources/Core/internal -ITests/CoreTests \
    $(find Sources/Core -name "*.c" | LC_ALL=C sort) Tests/CoreTests/*.c \
    -framework IOKit -framework CoreFoundation \
    -o "$OUT/core-tests"
if [ "${1:-}" = "--update-configs" ]; then
    exec "$OUT/core-tests" --write-configs configs
fi
"$OUT/core-tests"
