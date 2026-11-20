#!/usr/bin/env bash
set -euo pipefail

test_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "$test_dir/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/m5stack-audio-test.XXXXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT

for codec_file in app_codec_g711 app_vendor_g722_encode app_vendor_g722_decode; do
    "${CC:-cc}" -I "$repo_dir/src" -c "$repo_dir/src/$codec_file.c" \
        -o "$build_dir/$codec_file.o"
done
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
    -I "$test_dir/fakes" -I "$repo_dir/src" \
    "$test_dir/test.cpp" "$build_dir/"*.o -o "$build_dir/audio-test"

for codec in 0 1 2; do
    for packet_ms in 10 20; do
        for board in stick stopwatch; do
            "$build_dir/audio-test" "$codec" "$packet_ms" "$board"
        done
    done
done
