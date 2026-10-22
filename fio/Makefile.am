if WITH_FIO

pkglib_LTLIBRARIES = wheelie.la

# fio's own configuration, which the engine is compiled with. fio makes a
# Makefile of its own where it is configured, so it is configured in a
# directory of its own.
BUILT_SOURCES = fio-config/config-host.h

fio-config/config-host.h:
	$(MKDIR_P) fio-config
	cd fio-config && $(FIO_SRCDIR)/configure > configure.log 2>&1


wheelie_la_SOURCES = wheelie.c

wheelie_la_CPPFLAGS = -D_GNU_SOURCE \
                      -I$(FIO_SRCDIR) \
                      -I$(top_srcdir)/client/include \
                      -I$(top_srcdir)/wheelie/include \
                      -I$(top_srcdir)/wheelio/include

wheelie_la_CFLAGS = -std=gnu99 -Wall -Werror -fPIC \
                    $(ASAN_CFLAGS) \
                    -include fio-config/config-host.h

wheelie_la_LDFLAGS = -module -avoid-version -shared -rdynamic $(ASAN_LDFLAGS)

# The client is C++.
wheelie_la_LIBADD = $(top_builddir)/client/src/libclient.la -lstdc++


CLEANFILES = fio-config/configure.log
clean-local:
	rm -rf fio-config


EXTRA_DIST = tests/smoke.sh

test: all
	FIO_VERSION="$(FIO_VERSION)" \
	WHEELIE_FIO_ENGINE="$(builddir)/.libs/wheelie.so" \
	WHEELIE_SERVER="$(top_builddir)/server/src/wheelie-server" \
	WHEELIE_FIO_PRELOAD="$(ASAN_PRELOAD)" \
	$(srcdir)/tests/smoke.sh


.PHONY: test

else

test:


.PHONY: test

endif
