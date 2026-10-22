SUBDIRS = wheelio server client

ACLOCAL_AMFLAGS = -I m4


test: all
	$(MAKE) -C wheelio test
	$(MAKE) -C server test
	$(MAKE) -C client test


.PHONY: test
