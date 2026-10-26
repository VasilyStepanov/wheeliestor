EXTRA_DIST = ceiling.sh \
             layers.py \
             matrix.sh \
             report.py \
             run.sh \
             submit.py \
             tests/test_layers.py \
             tests/test_report.py \
             tests/test_submit.py


# The scripts need python3 for their report, and so do their tests.
test:
	@if command -v python3 > /dev/null 2>&1; then \
	    python3 -B -m unittest discover -s $(srcdir)/tests -p 'test_*.py'; \
	else \
	    echo "SKIP: no python3"; \
	fi


.PHONY: test
