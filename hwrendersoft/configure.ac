#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.
AC_PREREQ([2.69])

m4_define([hwrenderlib_version], [0.1.0.0])

AC_INIT([syndwars-hardware-render-library], [hwrenderlib_version], [mefistotelis@gmail.com], [hwrenderlib])

AC_CONFIG_SRCDIR([src/hwr_init.c])
AC_CONFIG_HEADERS([include/hwrendprivconf.h])
AX_PREFIX_CONFIG_H([include/hwrenderconf.h], [HWR])
AC_CONFIG_MACRO_DIRS([m4])

AM_INIT_AUTOMAKE([foreign subdir-objects])

# Checks for programs.

AC_PROG_CC
AC_PROG_RANLIB

# Checks for libraries.

# The hardware renderer needs SDL2 for its GL context and entry-point loader.
AM_PATH_SDL2([2.0.0], [
  have_sdl2=yes], [
  have_sdl2=no])
AS_IF([test "x$have_sdl2" != "xyes"], [
  AC_MSG_ERROR([the hardware renderer (--hwrender) requires SDL2])
])
AC_DEFINE(ENABLE_SDL2, [1], [Define for SDL2 support])

# Do not allow SDL to control machine emulation
AS_IF([test "$have_windows" = "yes"], [
  SDL_LIBS=`echo $SDL_LIBS | sed -e 's/\(^\|\s\)-mwindows\>//g'`
])

# Checks for header files.
AC_CHECK_HEADERS([stdint.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_INLINE
AC_TYPE_INT16_T
AC_TYPE_INT32_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT8_T

# Flags

CFLAGS="$CFLAGS -Wall -Wextra -Wno-unused-parameter"
CFLAGS="$CFLAGS -include \"\$(top_builddir)/include/hwrenderconf.h\""

CFLAGS="$CFLAGS $SDL_CFLAGS"
LIBS="$LIBS $SDL_LIBS"

# Final commands

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
