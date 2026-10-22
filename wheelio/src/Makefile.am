noinst_LTLIBRARIES = libwheelio.la


AM_CFLAGS = $(GENERIC_CFLAGS) \
            $(ASAN_CFLAGS) \
            $(LIBURING_CFLAGS) \
            -I$(top_srcdir)/wheelio/include

AM_LDFLAGS = $(ASAN_LDFLAGS)


libwheelio_la_SOURCES = $(top_srcdir)/wheelio/include/wheelio/wheelio.h \
                        backend.c \
                        reactor.c \
                        reactor.h \
                        libc.c \
                        pool.c \
                        pool.h \
                        uring.c

libwheelio_la_LIBADD = $(LIBURING_LIBS)
