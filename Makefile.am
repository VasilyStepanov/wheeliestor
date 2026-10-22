SUBDIRS = wheelio server

ACLOCAL_AMFLAGS = -I m4


test: all
	$(MAKE) -C wheelio test
	$(MAKE) -C server test


.PHONY: test
