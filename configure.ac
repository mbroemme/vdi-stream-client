# the autoconf initilization.
AC_INIT(vdi-stream-client, 0.4.0, [mbroemme@libmpq.org], [vdi-stream-client])

# detect the canonical target environment.
AC_CANONICAL_TARGET

# initialize autoconf and automake system.
AM_INIT_AUTOMAKE([no-dependencies])
AC_CONFIG_HEADERS([config.h:config.h.in])

# create define for the package author.
AC_DEFINE([AUTHOR], ["Maik Broemme <mbroemme@libmpq.org>"], [Define to the author of this package.])

# checking for programs.
CFLAGS="$CFLAGS -std=c17"
AC_USE_SYSTEM_EXTENSIONS
AC_PROG_CC

# require C17 capable compiler and use it for all project checks/builds.
AC_MSG_CHECKING([whether $CC supports C17])
AC_COMPILE_IFELSE(
	[AC_LANG_PROGRAM([[#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201710L
#error "C17 support is required"
#endif]])],
	[AC_MSG_RESULT([yes])],
	[AC_MSG_RESULT([no])
	 AC_MSG_ERROR([*** a C17 capable compiler is required])]
)

# checking for pkg-config application.
PKG_PROG_PKG_CONFIG

# checking for libdl library.
AC_CHECK_HEADER([dlfcn.h], [], [AC_MSG_ERROR([*** dlfcn.h is required, install glibc header files])])
AC_CHECK_LIB([dl], [dlopen], [], [AC_MSG_ERROR([*** dlopen is required, install glibc library files])])

# checking for sdl3 library.
PKG_CHECK_MODULES([SDL3], [sdl3 >= 3.2.0])

# checking for sdl3_ttf library.
PKG_CHECK_MODULES([SDL3_TTF], [sdl3-ttf >= 3.0.0])

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
