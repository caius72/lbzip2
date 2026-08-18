#!/bin/sh
#-
# Command line behaviour tests.
#
# Usage: cli.sh CASE LBZIP2
#
# The suite driven by driver.c covers the compressed format.  These cover what
# lbzip2 does with files, operands and exit codes -- including the default of
# removing its input, which every other test avoids with -k or -c.

set -e

case_name=${1:?usage: cli.sh CASE LBZIP2}
lbzip2=${2:?usage: cli.sh CASE LBZIP2}

work=$(mktemp -d "${TMPDIR:-/tmp}/lbzip2-cli.XXXXXX")
trap 'rm -rf "$work"' EXIT
cd "$work"

fail() { echo "cli.sh: $case_name: $*" >&2; exit 1; }

# Run lbzip2, capturing status and stderr rather than dying on failure.
run() {
  set +e
  "$lbzip2" "$@" > stdout.txt 2> stderr.txt
  status=$?
  set -e
}

expect_status() {
  [ "$status" -eq "$1" ] || fail "expected exit $1, got $status: $(cat stderr.txt)"
}

seed() { printf 'lbzip2 command line test payload %s\n' "$(seq 1 400 2>/dev/null || echo padding)" > "$1"; }

case $case_name in
  # The default is destructive: the input is removed once it is compressed.
  removes_input)
    seed f
    run f
    expect_status 0
    [ -f f.bz2 ] || fail "no f.bz2"
    [ -e f ] && fail "input still present"
    ;;
  keeps_input_with_k)
    seed f
    run -k f
    expect_status 0
    [ -f f.bz2 ] || fail "no f.bz2"
    [ -f f ] || fail "input removed despite -k"
    ;;
  # ... and the same on the way back.
  removes_input_on_decompress)
    seed f
    run f                       # not -k: f must be gone, or -d refuses to
    run -d f.bz2                # overwrite it and the test proves nothing
    expect_status 0
    [ -f f ] || fail "no f"
    [ -e f.bz2 ] && fail "f.bz2 still present"
    ;;
  roundtrip_preserves_content)
    seed f
    cp f expected
    run f
    run -d f.bz2
    expect_status 0
    [ "$(cat f)" = "$(cat expected)" ] || fail "content changed across a roundtrip"
    ;;
  # An existing target is a warning, not an overwrite.
  existing_target_skips)
    seed f
    : > f.bz2
    run -k f
    expect_status 4
    [ -s f.bz2 ] && fail "target was overwritten"
    grep -q "File exists" stderr.txt || fail "no File exists warning"
    ;;
  force_overwrites_target)
    seed f
    : > f.bz2
    run -f -k f
    expect_status 0
    [ -s f.bz2 ] || fail "target not written"
    ;;
  # Operands lbzip2 should refuse.
  missing_operand_warns)
    run -k no-such-file
    expect_status 4
    grep -q "no-such-file" stderr.txt || fail "file not named in the warning"
    ;;
  directory_operand_warns)
    mkdir d
    run -k d
    expect_status 4
    grep -q "not a regular file" stderr.txt || fail "no not-a-regular-file warning"
    ;;
  compressed_suffix_skips)
    seed f.bz2
    run -k f.bz2
    expect_status 4
    grep -q "compressed suffix" stderr.txt || fail "no compressed-suffix warning"
    ;;
  # Decompression of things that are not bzip2 data.
  not_bzip2_fails)
    seed f
    run -d -c f
    [ "$status" -eq 0 ] && fail "garbage accepted as bzip2 data"
    grep -q "not a valid bzip2 file" stderr.txt || fail "no not-a-valid-file error"
    ;;
  force_copies_non_bzip2)
    seed f
    run -d -c -f f
    expect_status 0
    [ "$(cat stdout.txt)" = "$(cat f)" ] || fail "-dcf did not pass the stream through"
    ;;
  # Options.
  rejects_unknown_option)
    run -Q
    [ "$status" -eq 0 ] && fail "unknown option accepted"
    ;;
  rejects_excessive_threads)
    seed f
    run -n 99999999 -k f
    [ "$status" -eq 0 ] && fail "absurd -n accepted"
    grep -q "as an integer in" stderr.txt || fail "no range message"
    ;;
  accepts_ignored_options)
    seed f
    run -S -q --repetitive-fast --exponential -k f
    expect_status 0
    ;;
  help_and_version)
    run -h
    expect_status 0
    grep -q "Usage" stdout.txt || fail "no usage on stdout"
    run -V
    expect_status 0
    grep -q "lbzip2 version" stdout.txt || fail "no version on stdout"
    ;;
  test_mode_detects_corruption)
    seed f
    run -k f
    dd if=/dev/urandom of=f.bz2 bs=1 seek=20 count=8 conv=notrunc 2>/dev/null
    run -t f.bz2
    [ "$status" -eq 0 ] && fail "corrupt file passed -t"
    ;;
  *)
    fail "unknown case"
    ;;
esac

exit 0
