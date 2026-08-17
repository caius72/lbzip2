# lbzip2, parallel bzip2 compression utility

> **Fork notice.** This is a fork of [kjn/lbzip2](https://github.com/kjn/lbzip2),
> created on 2026-08-17 from upstream commit `724352c`, because upstream appears
> unmaintained. Changes made in this fork are recorded in the git history and in
> the ChangeLog. It remains free software under the GNU General Public License,
> version 3 or later; see COPYING.

Copyright (C) 2011, 2012, 2013, 2014, 2015 Mikolaj Izdebski  
Copyright (C) 2008, 2009, 2010 Laszlo Ersek

This README file is part of lbzip2 version 2.5.

lbzip2 is a parallel, SMP-based, bzip2-compatible compression utility.

lbzip2 compresses and decompresses files using a variation of BWT compression
stack. More information on this topic can be found in the ALGORITHM file.

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
