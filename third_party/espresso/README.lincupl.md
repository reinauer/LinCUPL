# Berkeley Espresso in lincupl

`cupl -m4` minimizes with the `espresso()` function of the sources in this
directory (see `src/cupl/minimize_espresso.c`).

## Origin

`espresso/` holds the library sources of

- https://github.com/chipsalliance/espresso, tag `v2.5.1`,
  commit `d80d7bb96051b81d343bd66761f2768307b35a4b`,

with two pull requests applied that are open upstream at the time of writing:

- **#34 "contain: Initialize the containment scan boundary"**
  (`espresso/contain.c`). `rm_contain()` and `rm_rev_contain()` leave
  `pcheck` uninitialized until the first size group ends; GCC 15 reports
  `-Wmaybe-uninitialized`. The pointer now starts at the beginning of the
  array, which does not change the traversal.
- **#35 "espresso: Refresh cover costs before convergence checks"**
  (`espresso/espresso.c`, `espresso/sparse.c`). The main loop of
  `espresso()` and `make_sparse()` compare costs that 2.5.1 no longer
  recomputes after changing the cover, so they stop before further
  improvements are found. The cost is recomputed before each convergence
  decision. Only the library part of the pull request is taken; its test
  program and CMake changes are not.

Not taken from upstream: `main.c` (the command line program) and
`cvrin.c` / `cvrout.c` (reading and writing PLA files), which nothing in the
library refers to, and everything outside `espresso/` (build files, examples,
documentation).

## Licence

The upstream repository at that commit contains no licence or copyright file
and no notices in its sources. Espresso is software of the University of
California; `COPYRIGHT` is the notice of the Berkeley distribution the code
descends from, taken from `port/copyright.h` of
https://ptolemy.berkeley.edu/projects/embedded/pubs/downloads/espresso/espresso.tar.gz
(SHA-256 `757256c24d33f343068a67f51a57f8dfbe8af01d122a8434ef99572544cb4802`).
It asks that distributions include the notice.

## Changes made for embedding

To the upstream files, apart from the two pull requests:

1. `espresso/espresso.h`: include guard (`ESPRESSO_H`) added.
2. `espresso/mincov_int.h`: include guard (`MINCOV_INT_H`) added.

Both are needed because lincupl compiles the library as one translation
unit. Nothing else in `espresso/` differs from upstream.

Files added by lincupl (not part of Espresso):

- `lincupl_embed.c` includes the upstream `.c` files as a single translation
  unit and adds what embedding in a long-running program needs:
  - a tracking allocator behind `malloc`/`realloc`/`free`, so that everything
    the library allocated can be released after a call, also when it stopped
    in the middle;
  - replacements for `exit()` and `abort()`, which the library calls on
    internal errors (`fatal()`, `fail()`, `assert`): they return to
    `lincupl_esp_run()` instead of ending the process;
  - `lincupl_esp_teardown()`, which also resets the library's global and
    static variables (`cube`, `cdata`, the set family free list in `set.c`,
    the ordering switch `toggle` in `reduce.c`, `Rp_current` in `irred.c`).
    The static ones are only reachable from the same translation unit, which
    is the reason for the single unit. Without the reset the result of a
    minimization would depend on the calls before it.
- `lincupl_embed.h` declares that interface and redirects the five C library
  names for the library and its caller.
- `lincupl_rename.h` gives each of the 145 external symbols of the library
  the prefix `lincupl_esp_` (`cube`, `expand`, `verify`, `fatal`, ... would
  otherwise be global names of the program). It was generated from `nm -g`
  of the separately compiled upstream files. New upstream symbols must
  be added to this list so they are not exported without the prefix.

Build: `lincupl_embed.c` is compiled into `cupl_phases` with
`-fno-strict-aliasing -fwrapv` and without the warning flags of `src/`.
`-fwrapv` is there because the set macros of Espresso (`set_insert`,
`is_in_set`, ...) shift a signed `1` by up to 31 bits.

## Updating

Replace the files under `espresso/` with the new upstream ones, re-apply the
two include guards (and the pull requests, if they are still open), build,
and check exported symbols with `nm`. Add any new upstream symbols to
`lincupl_rename.h`. Reset any new static variables in
`lincupl_esp_teardown()`.
