# Changes in this fork

This file records how [caius72/lbzip2](https://github.com/caius72/lbzip2)
differs from [kjn/lbzip2](https://github.com/kjn/lbzip2), from which it was
forked on 2026-08-17 at commit `724352c`.  Upstream's own history is in
`ChangeLog` and `NEWS`; only the fork's changes are described here.

The fork exists because upstream stopped moving: Homebrew disabled its lbzip2
formula on 2025-07-07 as unmaintained and removed it a year later, on
2026-07-08, leaving macOS users without a package.  Several of the changes
below are the reasons it had become hard to package -- it did not build on
macOS at all, and `make install` installed nothing.

Nothing here alters the bzip2 format.  Every version below produces and
consumes exactly the same bytes as upstream, and the 1111-case test suite
passes on every supported platform.

## Unreleased

`src/common.h` no longer redefines `_Noreturn` when the C library already
defines it -- MSYS2 makes it `__dead2` in `<sys/cdefs.h>`, and redefining it
differently warned once per translation unit.

## 2.6.3

### Performance

**The inverse MTF transform uses a vector shuffle where the instruction set
provides one.** Its fast path moves one row of sixteen bytes to the front of a
sliding list, which upstream does with an unrolled `switch` — a branch on an
index the predictor cannot learn.  Both AArch64 and SSSE3 do the whole thing in
a single table lookup.  Measured with `-t -n1`, medians of repeated alternating
runs:

| Machine | Instruction | Before | After | |
|---|---|---|---|---|
| Apple M5 Max | `vqtbl1q_u8` | 1.81s | 1.66s | −8.3% (200MB mixed) |
| Apple M5 Max | `vqtbl1q_u8` | 3.46s | 3.13s | −9.5% (439MB text) |
| AMD EPYC 9V74 | `pshufb` | 4.65s | 4.32s | −7.1% (200MB mixed) |

Compression is unchanged, as expected for a decode-only path.  Decompressed
output is byte-identical to the branchy version, to `bzip2`, and to the
original data.

No compiler flags changed.  NEON is baseline on AArch64, so it is always
available there.  SSSE3 is *above* the baseline that x86-64 implies — AMD
shipped it only with Bulldozer — so that path compiles in exactly when the ISA
is already enabled, as `-march=native` or an `x86-64-v2` baseline does.  A
default build stays as portable as it was.

This is the optimisation proposed in [kjn/lbzip2#31][31].  The measurement
reported there found it slower; on current hardware it is not.

**A default build is optimised.** A plain `cmake ..` selects no build type, and
CMake then passes no optimisation flag at all.  The autotools build that CMake
replaced upstream defaulted `CFLAGS` to `-g -O2`, so anyone following the build
instructions after that switch got a binary roughly twice as slow: 10.85s
against 4.88s to compress 200MB, 2.84s against 1.75s to decompress it.
`CMAKE_BUILD_TYPE` now defaults to `Release` when the user names none; an
explicitly requested type is left alone.  Reported upstream as
[kjn/lbzip2#39][39].

**An absurd `-n` is rejected instead of attempted.** `_SC_THREAD_THREADS_MAX`
is an optional limit that most systems leave undefined, in which case
`sysconf()` returns -1 and the bound derived from it becomes `UINT_MAX`.  A
mistyped `-n 300000` was therefore accepted, and lbzip2 spent minutes creating
threads it could not use -- appearing to hang -- before running out and
failing.  The maximum is now 1024, or four times the number of online
processors where that is larger, and an over-large value is refused
immediately, naming the accepted range.  Reported upstream as
[kjn/lbzip2#24][24].

### Portability

**Windows builds and passes the test suite, under MSYS2.** Native Win32 has no
`fork()`, `sigaction()`, `futimens()` or `sysconf()`, all of which lbzip2 and
its test driver use, which is why [kjn/lbzip2#16][16] has stood open since
2016.  The MSYS2 runtime provides all of them and needs no source changes to
lbzip2 itself.  This is verified continuously in CI rather than claimed.  Note
the boundary: the result is a POSIX-runtime binary that needs `msys-2.0.dll`,
not the native port that issue asks for.

**The test driver no longer assumes libc is on the default search path.** It
handed each child an empty environment, which is fine where the C library is
found through the standard search path, but on Cygwin and MSYS2 the runtime is
a DLL that only `PATH` locates.  Every one of the 1111 tests failed there with
exit code 127 — the loader's way of saying it never reached `main()`.  `PATH`
alone is now passed through, so the tested program still cannot pick up
settings from whoever ran the suite.

**musl is tested.** Alpine joins the CI matrix, so the code is exercised
against a second C library rather than glibc alone.

### Testing

The CI matrix covers four platforms — glibc Linux, musl Linux, macOS on
Apple Silicon, and Windows via MSYS2 — building, running all 1111 tests, and
checking that the install rules produce a working binary and manual page.
Upstream CI built on Linux only, which is how two macOS breakages reached the
tree unnoticed.

`t_error()` in the test driver is declared `_Noreturn`.  It ends in `_exit()`,
but nothing said so, and every build warned that `test_handler` was used
uninitialised along a path that cannot be reached.

`actions/checkout` moved to v7; v4 targets Node.js 20, which the runners now
force onto Node.js 24 and warn about on every job.

### Build and packaging

`make install` works: the binary and all three manual pages are installed
through `GNUInstallDirs`, so `--prefix` and `DESTDIR` are honoured.  Upstream's
CMake build produced binaries but installed nothing.

The version is declared once, in `project()`, and `PACKAGE_VERSION` derives
from it.  An installed build previously reported itself as `devel`.

`lbzip2 --version` and the manual page name this fork as the project page,
rather than sending anyone reporting a bug to a repository that does not
contain the code they are running.  Upstream is still listed under SEE ALSO.
The URL comes from the build system, alongside `PACKAGE_NAME` and
`PACKAGE_VERSION`, so a downstream rebuild can point it elsewhere without
patching the source.

### Documentation

The README carries a fork notice, installation instructions for both Homebrew
and CMake, and this file.  A Homebrew tap lives at
[caius72/homebrew-lbzip2](https://github.com/caius72/homebrew-lbzip2).

## 2.6.2

Version bump only, so that a release tarball would carry the installation
instructions added just after 2.6.1 was tagged.

## 2.6.1

`lbzip2 --version` and `man lbzip2` pointed at the upstream repository.

## 2.6

First release from the fork.

**The build works on macOS.** Defining `_XOPEN_SOURCE=700` puts the macOS SDK
headers into strict POSIX mode, which hides both the `struct stat` timespec
members and the `_SC_NPROCESSORS_ONLN` sysconf key.  The former broke the build
outright; the latter made every invocation fail with `WORKER-THREADS not set`,
because the autodetection is compiled out when the key is missing.
`_DARWIN_C_SOURCE` is scoped to the `lbzip2` target rather than set globally,
since the test driver refers to `SIGPOLL`, which macOS declares only in strict
POSIX mode.

Install rules and a real version, as described above.

## Upstream issues

| Issue | State in this fork |
|---|---|
| [#16][16] porting to windows | Builds and tests under MSYS2, verified in CI.  Native Win32 remains unported. |
| [#24][24] limit thread upper bound | Implemented: `-n` is bounded, and an over-large value fails at once. |
| [#22][22] does not build on Fedora Rawhide | Cannot occur: the failure was in gnulib, which the CMake build does not use. |
| [#31][31] speedup opportunity via vector shuffle | Implemented for both NEON and SSSE3, with fresh measurements. |
| [#39][39] lbzip2 is 2x slower since last commits | Root cause was the missing default build type.  Fixed. |

[16]: https://github.com/kjn/lbzip2/issues/16
[22]: https://github.com/kjn/lbzip2/issues/22
[24]: https://github.com/kjn/lbzip2/issues/24
[31]: https://github.com/kjn/lbzip2/issues/31
[39]: https://github.com/kjn/lbzip2/issues/39
