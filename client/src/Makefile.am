noinst_LTLIBRARIES = libclient.la


AM_CXXFLAGS = $(GENERIC_CXXFLAGS) \
              $(ASAN_CFLAGS) \
              -I$(top_srcdir)/client/include \
              -I$(top_srcdir)/wheelie/include \
              -I$(top_srcdir)/wheelio/include

AM_LDFLAGS = $(ASAN_LDFLAGS)


libclient_la_SOURCES = $(top_srcdir)/client/include/client/client.hpp \
                       $(top_srcdir)/client/include/client/response_parser.hpp \
                       $(top_srcdir)/client/include/wheelie/client.h \
                       capi.cpp \
                       client.cpp

libclient_la_LIBADD = $(top_builddir)/wheelio/src/libwheelio.la
