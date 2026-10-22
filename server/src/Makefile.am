noinst_LTLIBRARIES = libserver.la

bin_PROGRAMS = wheelie-server


AM_CXXFLAGS = $(GENERIC_CXXFLAGS) \
              $(ASAN_CFLAGS) \
              -I$(top_srcdir)/server/include \
              -I$(top_srcdir)/wheelie/include \
              -I$(top_srcdir)/wheelio/include

AM_LDFLAGS = $(ASAN_LDFLAGS)


libserver_la_SOURCES = $(top_srcdir)/server/include/server/aligned_buffer.hpp \
                       $(top_srcdir)/server/include/server/request_parser.hpp \
                       $(top_srcdir)/server/include/server/server.hpp \
                       server.cpp \
                       session.cpp \
                       session.hpp

libserver_la_LIBADD = $(top_builddir)/wheelio/src/libwheelio.la


wheelie_server_SOURCES = main.cpp
wheelie_server_LDADD = libserver.la
