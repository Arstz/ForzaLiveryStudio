# Vendored potrace (potracelib core)

Source: potrace 1.16 (https://potrace.sourceforge.net/), by Peter Selinger.

Only the `potracelib` library core is vendored — not the command-line tool or
its file-format backends. Files taken from `src/` of the upstream tarball:

    potracelib.c/.h  curve.c/.h  trace.c/.h  decompose.c/.h
    auxiliary.h  bitmap.h  bitops.h  lists.h  progress.h

Upstream `COPYING`, `README`, and `AUTHORS` are kept alongside.

Build (see the `potrace` target in ../../CMakeLists.txt): compiled as a static
C library. `config.h` is not used; the only autotools symbols needed are
supplied as compile definitions — `VERSION`, `HAVE_INTTYPES_H`, `HAVE_STDINT_H`.

## Licensing

potrace is GPL "version 2, or (at your option) any later version," which is
compatible with this project's AGPLv3 (it may be taken as GPLv3; AGPLv3 permits
combining with GPLv3 code). No source changes were made. See COPYING.
