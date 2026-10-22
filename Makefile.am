SUBDIRS = wheelio server client fio bench

ACLOCAL_AMFLAGS = -I m4


test: all
	$(MAKE) -C wheelio test
	$(MAKE) -C server test
	$(MAKE) -C client test
	$(MAKE) -C fio test
	$(MAKE) -C bench test


.PHONY: test
