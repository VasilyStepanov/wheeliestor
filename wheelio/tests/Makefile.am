noinst_PROGRAMS = test_all


AM_CXXFLAGS = $(GENERIC_CXXFLAGS) \
              $(ASAN_CFLAGS) \
              $(GTEST_CFLAGS) \
              -I$(top_srcdir)/wheelio/include

AM_LDFLAGS = $(ASAN_LDFLAGS) \
             $(GTEST_LIBS)


test_all_SOURCES = main.cpp \
                   test_api.cpp \
                   test_backend.cpp \
                   test_reactor.cpp
test_all_LDADD = $(top_builddir)/wheelio/src/libwheelio.la


test: all
	./test_all


.PHONY: test
