#!/usr/bin/env bash
set -euo pipefail
test_source_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
test_build_dir="$(mktemp -d /tmp/m5stack-transport-test.XXXXXX)"
trap 'rm -rf -- "$test_build_dir"' EXIT
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -pthread \
    "$test_source_dir/test_time_compat.c" \
    "$test_source_dir/../../main/voice_rtsa_time_compat.c" \
    -o "$test_build_dir/test_time_compat"
"$test_build_dir/test_time_compat"
