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
    # A plain fio loads an engine built with the sanitizer only with its runtime
    # loaded first, and with the C++ runtime loaded along with it: the
    # runtime of the sanitizer has to find the real __cxa_throw to intercept it,
    # and the first exception the engine throws brings the process down if it
    # cannot.
    AC_SUBST([ASAN_PRELOAD], ["`$CC -print-file-name=libasan.so` `$CXX -print-file-name=libstdc++.so`"])
], [
    AC_SUBST([ASAN_PRELOAD], [""])
    AC_SUBST([ASAN_CFLAGS], [""])
    AC_SUBST([ASAN_LDFLAGS], [""])
])


AC_SUBST([LIBURING_MIN_VERSION], ["2.3"])
PKG_CHECK_MODULES([LIBURING], [liburing >= $LIBURING_MIN_VERSION])


# The fio engine is an external engine of fio, built against the source of fio:
# its headers, and the configuration that fio itself is built with.
AC_ARG_WITH([fio], [AS_HELP_STRING([--with-fio=DIR],
    [build the fio engine against the source of fio in DIR; by default the one
     in 3rdparty/fio, if it is there, and --without-fio builds no engine])],
    [with_fio=$withval],
    [with_fio="check"])
fio_srcdir=""
AS_CASE([$with_fio],
    [no], [],
    [check], [
        AS_IF([test -f "$srcdir/3rdparty/fio/fio.h"], [
            fio_srcdir=`cd "$srcdir/3rdparty/fio" && pwd`
        ], [
            AC_MSG_NOTICE([no source of fio in 3rdparty/fio: the fio engine is not built])
        ])
    ],
    [yes], [AC_MSG_ERROR([--with-fio needs the directory with the source of fio])],
    [
        AS_IF([test -f "$with_fio/fio.h"], [
            fio_srcdir=`cd "$with_fio" && pwd`
        ], [
            AC_MSG_ERROR([no source of fio in $with_fio])
        ])
    ])
AM_CONDITIONAL([WITH_FIO], [test -n "$fio_srcdir"])
AC_SUBST([FIO_SRCDIR], [$fio_srcdir])
AS_IF([test -n "$fio_srcdir"], [
    # The version the engine is built for: the engine can only be loaded by
    # that fio.
    (cd "$fio_srcdir" && ./FIO-VERSION-GEN >/dev/null 2>&1)
    fio_version=`sed -n 's/^FIO_VERSION = //p' "$fio_srcdir/FIO-VERSION-FILE" 2>/dev/null`
])
AC_SUBST([FIO_VERSION], [$fio_version])


AC_CONFIG_FILES([
    Makefile
    wheelio/Makefile
    wheelio/src/Makefile
    wheelio/tests/Makefile
    server/Makefile
    server/src/Makefile
    server/tests/Makefile
    client/Makefile
    client/src/Makefile
    client/tests/Makefile
    fio/Makefile
])
AC_OUTPUT
