AUTOMAKE_OPTIONS = foreign
SUBDIRS = src
EXTRA_DIST = doxygen/libltntstools.doxyconf doxygen/include

docs:
	cd doxygen && doxygen libltntstools.doxyconf

test:
	cd test && make test
