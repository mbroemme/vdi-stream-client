# the autoconf initilization.
AC_INIT(vdi-stream-client, 0.1.0, [mbroemme@libmpq.org], [vdi-stream-client])

# detect the canonical target environment.
AC_CANONICAL_TARGET

# initialize autoconf and automake system.
AM_INIT_AUTOMAKE([no-dependencies])
AC_CONFIG_HEADERS([config.h:config.h.in])

# create define for the package author.
AC_DEFINE([AUTHOR], ["Maik Broemme <mbroemme@libmpq.org>"], [Define to the author of this package.])

# checking for programs.
AC_PROG_CC

# checking for libdl library.
AC_CHECK_HEADER([dlfcn.h], [], [AC_MSG_ERROR([*** dlfcn.h is required, install glibc header files])])
AC_CHECK_LIB([dl], [dlopen], [], [AC_MSG_ERROR([*** dlopen is required, install glibc library files])])

# checking for libx11 library.
AC_CHECK_HEADER([X11/Xlib.h], [], [AC_MSG_ERROR([*** X11/Xlib.h is required, install libx11 header files])])
AC_CHECK_LIB([X11], [XGrabKeyboard], [], [AC_MSG_ERROR([*** XGrabKeyboard is required, install libx11 library files])])

# checking for libglvnd library.
AC_CHECK_HEADER([GL/gl.h], [], [AC_MSG_ERROR([*** X11/Xlib.h is required, install libglvnd header files])])
AC_CHECK_LIB([GL], [glFinish], [], [AC_MSG_ERROR([*** glFinish is required, install libglvnd library files])])

# checking for libparsec library.
AC_CHECK_HEADER([parsec/parsec.h], [], [AC_MSG_ERROR([*** parsec/parsec.h is required, install parsec-sdk header files])])
AC_CHECK_LIB([parsec], [ParsecInit], [], [AC_MSG_ERROR([*** ParsecInit is required, install parsec-sdk library files])])

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
AC_CONFIG_FILES([
Makefile
src/Makefile
])

# perform configuration actions.
AC_OUTPUT