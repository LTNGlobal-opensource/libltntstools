CC       ?= cc
CFLAGS   ?= -Wall -O2 -g
INCLUDES  = -I../src

.PHONY: test clean

test: test_crc32
	./test_crc32

test_crc32: test_crc32.c ../src/crc32.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_crc32.c ../src/crc32.c

clean:
	rm -f test_crc32
