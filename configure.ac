# the autoconf initilization.
AC_INIT(vdi-stream-client, 0.3.0, [mbroemme@libmpq.org], [vdi-stream-client])

# detect the canonical target environment.
AC_CANONICAL_TARGET

# initialize autoconf and automake system.
AM_INIT_AUTOMAKE([no-dependencies])
AC_CONFIG_HEADERS([config.h:config.h.in])

# create define for the package author.
AC_DEFINE([AUTHOR], ["Maik Broemme <mbroemme@libmpq.org>"], [Define to the author of this package.])

# checking for programs.
AC_PROG_CC

# checking for pkg-config application.
PKG_PROG_PKG_CONFIG

# checking for libdl library.
AC_CHECK_HEADER([dlfcn.h], [], [AC_MSG_ERROR([*** dlfcn.h is required, install glibc header files])])
AC_CHECK_LIB([dl], [dlopen], [], [AC_MSG_ERROR([*** dlopen is required, install glibc library files])])

# checking for libx11 library.
PKG_CHECK_MODULES([X11], [x11])

# checking for libglvnd library.
PKG_CHECK_MODULES([GL], [gl])

# checking for sdl2 library.
PKG_CHECK_MODULES([SDL2], [sdl2 >= 2.0.5])

# checking for sdl2_ttf library.
PKG_CHECK_MODULES([SDL2_TTF], [SDL2_ttf >= 2.0.5])

# checking for libusb library.
PKG_CHECK_MODULES([USB], [libusb-1.0 >= 1.0.9])

# checking for usbredir library.
PKG_CHECK_MODULES([USBREDIRHOST], [libusbredirhost >= 0.7.1])
PKG_CHECK_MODULES([USBREDIRPARSER], [libusbredirparser-0.5 >= 0.7.1])

# checking for internal parsec sdk.
AC_CHECK_FILE([parsec-sdk/sdk/parsec.h], [AC_CHECK_FILE([parsec-sdk/sdk/linux/libparsec.so], [enable_internal_parsec_sdk="yes"])])

# checking for external parsec sdk usage.
if test "x$enable_internal_parsec_sdk" != "xyes"; then

	# checking for external parsec sdk.
	AC_CHECK_HEADER([parsec/parsec.h], [], [AC_MSG_ERROR([*** parsec/parsec.h is required, install parsec-sdk header files])])
	AC_CHECK_LIB([parsec], [ParsecInit], [], [AC_MSG_ERROR([*** ParsecInit is required, install parsec-sdk library files])])
	AC_CHECK_MEMBER([struct ParsecCursor.hidden], [], [AC_MSG_ERROR([*** struct ParsecCursor.hidden is required, install parsec-sdk >= 6.0])], [[#include <parsec/parsec.h>]])
fi

# condition for parsec sdk to usage.
AM_CONDITIONAL([INTERNAL_PARSEC_SDK], [test "x$enable_internal_parsec_sdk" = "xyes"])

# creating files.
AC_CONFIG_FILES([
Makefile
src/Makefile
docs/man1/Makefile
])

# perform configuration actions.
AC_OUTPUT
