AC_INIT([wheeliestor], [0.1.0])
AM_INIT_AUTOMAKE([1.9 tar-pax foreign subdir-objects])

AC_CONFIG_HEADERS([config.h])

AC_CANONICAL_HOST

AC_PROG_CC
AC_PROG_CXX

LT_INIT

AC_CONFIG_MACRO_DIRS([m4])

AC_SUBST([GENERIC_CFLAGS], ["-std=gnu99 -Wall -Wextra -Werror -fPIC -pthread"])
AC_SUBST([GENERIC_CXXFLAGS], ["-std=c++20 -Wall -Wextra -Werror -fPIC -pthread"])

# The alibc backend runs its disk I/O on pthreads.
AC_SEARCH_LIBS([pthread_create], [pthread])


PKG_CHECK_MODULES([GTEST], [gtest])


AC_ARG_ENABLE([asan], [AS_HELP_STRING([--enable-asan],
    [Enable address sanitizer])],
    [enable_asan=$enableval],
    [enable_asan="no"])
AS_IF([test "x$enable_asan" = "xyes"], [
    AC_SUBST([ASAN_CFLAGS], ["-fsanitize=address -g"])
    AC_SUBST([ASAN_LDFLAGS], ["-fsanitize=address"])
], [
    AC_SUBST([ASAN_CFLAGS], [""])
    AC_SUBST([ASAN_LDFLAGS], [""])
])


AC_SUBST([LIBURING_MIN_VERSION], ["2.3"])
PKG_CHECK_MODULES([LIBURING], [liburing >= $LIBURING_MIN_VERSION])


AC_CONFIG_FILES([
    Makefile
    wheelio/Makefile
    wheelio/src/Makefile
    wheelio/tests/Makefile
    server/Makefile
    server/src/Makefile
    server/tests/Makefile
])
AC_OUTPUT
