# lbzip2, parallel bzip2 compression utility

> **Fork notice.** This is a fork of [kjn/lbzip2](https://github.com/kjn/lbzip2),
> created on 2026-08-17 from upstream commit `724352c`, because upstream is
> unmaintained -- Homebrew disabled its lbzip2 formula on 2025-07-07 for that
> reason and removed it on 2026-07-08. Changes made in this fork are described in [CHANGES.md](CHANGES.md)
> and recorded in the git history. It remains free software under the GNU General
> Public License, version 3 or later; see COPYING.

Copyright (C) 2011, 2012, 2013, 2014, 2015 Mikolaj Izdebski  
Copyright (C) 2008, 2009, 2010 Laszlo Ersek

This README file is part of lbzip2 version 2.6.2.

lbzip2 is a parallel, SMP-based, bzip2-compatible compression utility.

lbzip2 compresses and decompresses files using a variation of BWT compression
stack. More information on this topic can be found in the ALGORITHM file.

## How this fork differs from upstream

The compressed format is untouched: this produces and reads exactly the same
bytes as upstream and as `bzip2`.

- **Faster decompression — 7 to 10%.** The inverse MTF transform uses a single
  vector shuffle (`vqtbl1q_u8` on AArch64, `pshufb` on SSSE3) instead of a
  branch the predictor cannot learn. No compiler flags change, so a default
  build stays as portable as it was. ([kjn/lbzip2#31](https://github.com/kjn/lbzip2/issues/31))
- **A default build is optimised again.** A plain `cmake ..` selected no build
  type and therefore no optimisation at all, making the binary roughly twice as
  slow as the autotools build it replaced. ([kjn/lbzip2#39](https://github.com/kjn/lbzip2/issues/39))
- **Builds and passes its tests on Windows,** under the MSYS2 runtime — open
  upstream since 2016. Native Win32 is still unported.
  ([kjn/lbzip2#16](https://github.com/kjn/lbzip2/issues/16))
- **Builds and passes its tests on macOS,** which strict POSIX feature-test
  macros had broken outright.
- **Tested on four platforms** — glibc Linux, musl Linux, macOS on Apple
  Silicon, and Windows — on every push, all 1111 cases. Upstream CI covered
  Linux alone.
- **`make install` installs something:** the binary and all three manual pages,
  honouring `--prefix` and `DESTDIR`. It previously installed nothing.
- **Installable with Homebrew** from a tap, and reports its own version and
  project page correctly.

[CHANGES.md](CHANGES.md) describes each of these in detail.

## Installation

With [Homebrew](https://brew.sh):

```sh
brew trust --formula caius72/lbzip2/lbzip2
brew install caius72/lbzip2/lbzip2
```

Homebrew refuses to load formulae from third-party taps until they are trusted.
Installing by the fully qualified name above trusts this one implicitly, so the
first command is only strictly needed if you tap first and then install
`lbzip2` by its short name; running it up front does no harm either way.

Homebrew core dropped its own `lbzip2` formula: disabled on 2025-07-07 as
unmaintained, removed on 2026-07-08. This tap is a replacement for it. If a
copy from before the removal is still installed, `brew uninstall lbzip2` first,
since the two cannot occupy the name at once.

From source, with CMake 3.15 or newer and a C99 compiler:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build      # optional, 1111 tests
cmake --install build       # honours --prefix and DESTDIR
```

Installing places `lbzip2` in `bin` and the three manual pages in `share/man`.
The `lbunzip2` and `lbzcat` modes are selected by the name lbzip2 is invoked
under, so create them as symlinks to the installed binary:

```sh
ln -s lbzip2 <prefix>/bin/lbunzip2
ln -s lbzip2 <prefix>/bin/lbzcat
```

lbzip2 is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.

lbzip2 is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

A copy of the GNU General Public License is contained in the COPYING file.

The following is a list of subdirectories with explanation of their purpose:

```
build-aux - different auxiliary build scripts
man       - manual pages
src       - C source code of higher-level part of lbzip2
tests     - regression tests
```
