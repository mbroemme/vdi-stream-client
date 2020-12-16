# the autoconf initilization.
AC_INIT(vdi-stream-client, 0.1.0, [mbroemme@libmpq.org], [vdi-stream-client])

# detect the canonical host and target build environment.
AC_CANONICAL_SYSTEM

# initialize autoconf and automake system.
AM_INIT_AUTOMAKE([no-dependencies])
AC_CONFIG_HEADERS([config.h:config.h.in])

# create define for the package author.
AC_DEFINE([AUTHOR], ["Maik Broemme <mbroemme@libmpq.org>"], [Define to the author of this package.])

# checking for programs.
AC_PROG_CC

# checking for x11.
AC_SEARCH_LIBS([XGrabKeyboard], [X11], [], [AC_MSG_ERROR([*** libX11 is required])])

# checking for libsdl2 library.
AC_CHECK_HEADER([SDL2/SDL.h], [], [AC_MSG_ERROR([*** SDL2/SDL.h is required, install libsdl2 header files])])
AC_PATH_PROG([sdl2_config], [sdl2-config], [AC_MSG_ERROR([*** sdl2-config is required, install libsdl2 development files])])

# checking for libsdl2 environment.
if test -n "$sdl2_config"; then
	LIBSDL2_CFLAGS="$(sdl2-config --cflags)"
	LIBSDL2_LIBS="$(sdl2-config --libs)"
fi

# export library flags.
AC_SUBST(LIBSDL2_CFLAGS)
AC_SUBST(LIBSDL2_LIBS)

# creating files.
AC_OUTPUT([
Makefile
src/Makefile
])
