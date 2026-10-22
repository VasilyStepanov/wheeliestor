noinst_PROGRAMS = test_all


AM_CXXFLAGS = $(GENERIC_CXXFLAGS) \
              $(ASAN_CFLAGS) \
              $(GTEST_CFLAGS) \
              -I$(top_srcdir)/server/include \
              -I$(top_srcdir)/wheelie/include \
              -I$(top_srcdir)/wheelio/include

AM_LDFLAGS = $(ASAN_LDFLAGS) \
             $(GTEST_LIBS)


test_all_SOURCES = main.cpp \
                   test_parser.cpp \
                   test_server.cpp
test_all_LDADD = $(top_builddir)/server/src/libserver.la


test: all
	./test_all


.PHONY: test
