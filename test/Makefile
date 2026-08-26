CC       ?= cc
CFLAGS   ?= -Wall -O2 -g
INCLUDES  = -I../src
LDLIBS_HISTORY_METRIC = -lpthread

.PHONY: test clean

test: test_crc32 test_history_metric
	./test_crc32
	./test_history_metric

test_crc32: test_crc32.c ../src/crc32.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_crc32.c ../src/crc32.c

test_history_metric: test_history_metric.c ../src/history-metric.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_history_metric.c ../src/history-metric.c $(LDLIBS_HISTORY_METRIC)

clean:
	rm -f test_crc32 test_history_metric
