SUBDIRS = wheelio server client fio

ACLOCAL_AMFLAGS = -I m4


test: all
	$(MAKE) -C wheelio test
	$(MAKE) -C server test
	$(MAKE) -C client test
	$(MAKE) -C fio test


.PHONY: test
