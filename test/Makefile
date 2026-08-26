CC       ?= cc
CFLAGS   ?= -Wall -O2 -g
INCLUDES  = -I../src
LDLIBS_HISTORY_METRIC = -lpthread

.PHONY: test clean

test: test_crc32 test_history_metric test_ts test_reframer test_stats
	./test_crc32
	./test_history_metric
	./test_ts
	./test_reframer
	./test_stats

test_crc32: test_crc32.c ../src/crc32.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_crc32.c ../src/crc32.c

test_history_metric: test_history_metric.c ../src/history-metric.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_history_metric.c ../src/history-metric.c $(LDLIBS_HISTORY_METRIC)

test_ts: test_ts.c ../src/ts.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_ts.c ../src/ts.c

test_reframer: test_reframer.c ../src/reframer.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_reframer.c ../src/reframer.c

test_stats: test_stats.c ../src/stats.c ../src/clocks.c ../src/ts.c ../src/history-metric.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_stats.c ../src/stats.c ../src/clocks.c ../src/ts.c ../src/history-metric.c -lpthread

clean:
	rm -f test_crc32 test_history_metric test_ts test_reframer test_stats
