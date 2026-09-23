#!/usr/bin/env bash
# FUSE Relight RL-0.5 rl_hash_wine: the MinGW-w64 build of fuse_relight_hash_kat, run under Wine,
#  1. passes the same checks as rl_hash_kat (official xxHash vectors, the KAT table, properties,
#     library vs upstream oracle, golden digest), and
#  2. prints exactly the native build's per-function digests of the 10k-per-function random case
#     set (and both equal tests/hash/digest_golden.txt).
# Exit 77 (skip) without the MinGW build or Wine.
#
# Usage: rl_hash_wine.sh <native exe> <mingw exe or ""> <fuse-wine-run.sh> <wineprefix> <tests/hash dir>
set -euo pipefail
native="$1"
mingw="$2"
runner="$3"
prefix="$4"
testdir="$5"
golden="$testdir/digest_golden.txt"

if [ -z "$mingw" ] || [ ! -f "$mingw" ]; then
    echo "SKIP: no MinGW-w64 build of fuse_relight_hash_kat (x86_64-w64-mingw32-g++ not found)"
    exit 77
fi

set +e
bash "$runner" "$prefix" "$mingw" --vectors "$testdir/xxhash_vectors.txt" --kat "$testdir/kat_vectors.txt" --self \
    --digest-check "$golden" | tr -d '\r'
rc=${PIPESTATUS[0]}
set -e
if [ "$rc" -eq 77 ]; then
    echo "SKIP: wine not installed"
    exit 77
fi
if [ "$rc" -ne 0 ]; then
    echo "FAIL: the MinGW build's known-answer checks exited with $rc under Wine"
    exit 1
fi

native_out="$("$native" --digest)"
set +e
wine_out="$(bash "$runner" "$prefix" "$mingw" --digest)"
rc=$?
set -e
if [ "$rc" -eq 77 ]; then
    echo "SKIP: wine not installed"
    exit 77
fi
if [ "$rc" -ne 0 ]; then
    echo "FAIL: the MinGW build exited with $rc under Wine"
    printf '%s\n' "$wine_out"
    exit 1
fi
wine_out="$(printf '%s\n' "$wine_out" | tr -d '\r')"

status=0
if [ "$native_out" != "$wine_out" ]; then
    echo "FAIL: native and Wine digests differ"
    diff <(printf '%s\n' "$native_out") <(printf '%s\n' "$wine_out") || true
    status=1
fi
if [ "$(printf '%s\n' "$native_out" | grep '^digest')" != "$(grep '^digest' "$golden")" ]; then
    echo "FAIL: digests differ from $golden"
    diff <(printf '%s\n' "$native_out" | grep '^digest') <(grep '^digest' "$golden") || true
    status=1
fi
if [ "$status" -eq 0 ]; then
    printf '%s\n' "$wine_out"
    echo "rl_hash_wine: MinGW/Wine digests identical to native and golden ($(printf '%s\n' "$wine_out" | grep -c '^digest') digests)"
fi
exit "$status"
