# Builds everything, and refuses to call it done if any part failed.
all:
	$(MAKE) -C src/finder all
	$(MAKE) -C src/write all
	$(MAKE) -C src/code all

test: all
	bash tests/test_write.sh
	bash tests/test_finder.sh
	bash tests/test_code.sh

.PHONY: all test
