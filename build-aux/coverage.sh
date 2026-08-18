#!/bin/sh
#-
# Merge the profiles left by a coverage build and report on them.
#
# Usage: coverage.sh BUILD-DIR BINARY
#
# Invoked by the "coverage" target; see LBZIP2_COVERAGE in CMakeLists.txt.

set -e

build=${1:?usage: coverage.sh BUILD-DIR BINARY}
binary=${2:?usage: coverage.sh BUILD-DIR BINARY}

# The Xcode toolchain does not put these on PATH.
if command -v llvm-profdata >/dev/null 2>&1; then
  profdata=llvm-profdata
  cov=llvm-cov
elif command -v xcrun >/dev/null 2>&1; then
  profdata="xcrun llvm-profdata"
  cov="xcrun llvm-cov"
else
  echo "coverage.sh: neither llvm-profdata nor xcrun found" >&2
  exit 1
fi

raw=$build/raw
out=$build/coverage
merged=$out/lbzip2.profdata

if [ ! -d "$raw" ] || [ -z "$(ls "$raw" 2>/dev/null)" ]; then
  echo "coverage.sh: no profiles in $raw -- was this configured with -DLBZIP2_COVERAGE=ON?" >&2
  exit 1
fi

mkdir -p "$out"

# A run of the full suite leaves thousands of files, too many for one command
# line, so feed them through a list instead.
ls -d "$raw"/*.profraw > "$out/profraw.list"
$profdata merge -sparse -f "$out/profraw.list" -o "$merged"

$cov report "$binary" -instr-profile="$merged" -ignore-filename-regex='tests/'
$cov show "$binary" -instr-profile="$merged" -ignore-filename-regex='tests/' \
    -format=html -output-dir="$out/html" \
    -show-branches=count -show-line-counts-or-regions > /dev/null

echo
echo "HTML report: $out/html/index.html"
