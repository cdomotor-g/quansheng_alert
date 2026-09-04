#!/bin/sh
# A short id for the *content* of everything the firmware is compiled from, so a
# rebuild that changes no source produces a byte-identical image. Deliberately
# excludes docs/, tools/, tests and CI config: editing those must not change the
# firmware.
find Makefile firmware.ld start.S printf_config.h \
     *.c *.h app driver ui helper bsp hardware external \
     -type f ! -name '*.o' ! -name '*.d' -print0 2>/dev/null \
  | LC_ALL=C sort -z | xargs -0 sha256sum | sha256sum | cut -c1-7
