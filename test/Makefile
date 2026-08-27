CC       ?= cc
CFLAGS   ?= -Wall -O2 -g
INCLUDES  = -I../src
LDLIBS_HISTORY_METRIC = -lpthread

# pat.c compiles directly against <dvbpsi/*.h> and links -ldvbpsi. Prefer
# pkg-config (portable); fall back to this multi-repo checkout's shared
# dependency sysroot (../../target-root), which is what this project's own
# top-level ./configure already resolves to on this kind of checkout.
DVBPSI_CFLAGS := $(shell pkg-config --cflags libdvbpsi 2>/dev/null)
DVBPSI_LIBS   := $(shell pkg-config --libs libdvbpsi 2>/dev/null)
ifeq ($(strip $(DVBPSI_CFLAGS)$(DVBPSI_LIBS)),)
DVBPSI_CFLAGS := -I../../target-root/usr/include
DVBPSI_LIBS   := -L../../target-root/usr/lib -ldvbpsi
endif

.PHONY: test clean

test: test_crc32 test_history_metric test_ts test_reframer test_stats test_descriptor test_pes test_pat test_sectionextractor test_ts_packetizer test_throughput_hires test_time test_throughput test_streammodel
	./test_crc32
	./test_history_metric
	./test_ts
	./test_reframer
	./test_stats
	./test_descriptor
	./test_pes
	./test_pat
	./test_sectionextractor
	./test_ts_packetizer
	./test_throughput_hires
	./test_time
	./test_throughput
	./test_streammodel

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

test_descriptor: test_descriptor.c ../src/descriptor.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_descriptor.c ../src/descriptor.c

test_pes: test_pes.c ../src/pes.c ../src/crc32.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_pes.c ../src/pes.c ../src/crc32.c

test_pat: test_pat.c ../src/pat.c ../src/descriptor.c ../src/crc32.c ../src/ts.c
	$(CC) $(CFLAGS) $(INCLUDES) $(DVBPSI_CFLAGS) -o $@ test_pat.c ../src/pat.c ../src/descriptor.c ../src/crc32.c ../src/ts.c $(DVBPSI_LIBS)

test_sectionextractor: test_sectionextractor.c ../src/sectionextractor.c ../src/ts.c ../src/crc32.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_sectionextractor.c ../src/sectionextractor.c ../src/ts.c ../src/crc32.c

test_ts_packetizer: test_ts_packetizer.c ../src/ts_packetizer.c ../src/ts.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_ts_packetizer.c ../src/ts_packetizer.c ../src/ts.c

test_throughput_hires: test_throughput_hires.c ../src/throughput_hires.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_throughput_hires.c ../src/throughput_hires.c

test_time: test_time.c ../src/time.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_time.c ../src/time.c

test_throughput: test_throughput.c ../src/throughput.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_throughput.c ../src/throughput.c

test_streammodel: test_streammodel.c ../src/streammodel.c ../src/streammodel-extractors.c ../src/pat.c ../src/descriptor.c ../src/crc32.c ../src/ts.c ../src/sectionextractor.c ../src/stats.c ../src/clocks.c ../src/history-metric.c
	$(CC) $(CFLAGS) $(INCLUDES) $(DVBPSI_CFLAGS) -o $@ test_streammodel.c ../src/streammodel.c ../src/streammodel-extractors.c ../src/pat.c ../src/descriptor.c ../src/crc32.c ../src/ts.c ../src/sectionextractor.c ../src/stats.c ../src/clocks.c ../src/history-metric.c $(DVBPSI_LIBS) -lpthread

clean:
	rm -f test_crc32 test_history_metric test_ts test_reframer test_stats test_descriptor test_pes test_pat test_sectionextractor test_ts_packetizer test_throughput_hires test_time test_throughput test_streammodel
