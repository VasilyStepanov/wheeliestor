noinst_PROGRAMS = test_all


AM_CFLAGS = $(GENERIC_CFLAGS) \
            $(ASAN_CFLAGS) \
            -I$(top_srcdir)/client/include \
            -I$(top_srcdir)/wheelie/include \
            -I$(top_srcdir)/wheelio/include

AM_CXXFLAGS = $(GENERIC_CXXFLAGS) \
              $(ASAN_CFLAGS) \
              $(GTEST_CFLAGS) \
              -I$(top_srcdir)/client/include \
              -I$(top_srcdir)/server/include \
              -I$(top_srcdir)/wheelie/include \
              -I$(top_srcdir)/wheelio/include

AM_LDFLAGS = $(ASAN_LDFLAGS) \
             $(GTEST_LIBS)


test_all_SOURCES = main.cpp \
                   c_header.c \
                   test_client.cpp \
                   test_coro.cpp \
                   test_location.cpp \
                   test_parser.cpp
test_all_LDADD = $(top_builddir)/client/src/libclient.la \
                 $(top_builddir)/server/src/libserver.la


test: all
	./test_all


.PHONY: test
