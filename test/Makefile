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

test: test_crc32 test_history_metric test_ts test_reframer test_stats test_descriptor test_pes test_pat test_sectionextractor test_ts_packetizer test_throughput_hires test_time test_throughput test_streammodel test_streammodel_extractors test_pes_extractor test_sei_timestamp test_smoother_pcr test_probes test_smoother_rtp test_segmentwriter test_klbitstream_readwriter test_tr101290 test_nal_bitreader test_udp_receiver test_xorg_list test_vbv
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
	./test_streammodel_extractors
	./test_pes_extractor
	./test_sei_timestamp
	./test_smoother_pcr
	./test_probes
	./test_smoother_rtp
	./test_segmentwriter
	./test_klbitstream_readwriter
	./test_tr101290
	./test_nal_bitreader
	./test_udp_receiver
	./test_xorg_list
	./test_vbv

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

test_streammodel_extractors: test_streammodel_extractors.c ../src/streammodel-extractors.c ../src/sectionextractor.c ../src/ts.c ../src/crc32.c
	$(CC) $(CFLAGS) $(INCLUDES) $(DVBPSI_CFLAGS) -o $@ test_streammodel_extractors.c ../src/streammodel-extractors.c ../src/sectionextractor.c ../src/ts.c ../src/crc32.c

test_pes_extractor: test_pes_extractor.c ../src/pes-extractor.c ../src/klringbuffer.c ../src/utils.c ../src/pes.c ../src/crc32.c ../src/stats.c ../src/clocks.c ../src/ts.c ../src/history-metric.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_pes_extractor.c ../src/pes-extractor.c ../src/klringbuffer.c ../src/utils.c ../src/pes.c ../src/crc32.c ../src/stats.c ../src/clocks.c ../src/ts.c ../src/history-metric.c -lpthread

test_sei_timestamp: test_sei_timestamp.c ../src/sei-timestamp.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_sei_timestamp.c ../src/sei-timestamp.c

test_smoother_pcr: test_smoother_pcr.c ../src/smoother-pcr.c ../src/ts.c ../src/utils.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_smoother_pcr.c ../src/smoother-pcr.c ../src/ts.c ../src/utils.c -lpthread

test_probes: test_probes.c ../src/probes.c ../src/sei-timestamp.c ../src/utils.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_probes.c ../src/probes.c ../src/sei-timestamp.c ../src/utils.c -lpthread

test_smoother_rtp: test_smoother_rtp.c ../src/smoother-rtp.c ../src/rtp-analyzer.c ../src/utils.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_smoother_rtp.c ../src/smoother-rtp.c ../src/rtp-analyzer.c ../src/utils.c -lpthread

test_segmentwriter: test_segmentwriter.c ../src/segmentwriter.c ../src/kl-queue.c ../src/time.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_segmentwriter.c ../src/segmentwriter.c ../src/kl-queue.c ../src/time.c -lpthread

test_klbitstream_readwriter: test_klbitstream_readwriter.c ../src/libltntstools/klbitstream_readwriter.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_klbitstream_readwriter.c

TR101290_SRCS := ../src/tr101290.c ../src/tr101290-events.c ../src/tr101290-alarms.c ../src/tr101290-timers.c ../src/tr101290-p1.c ../src/tr101290-p2.c ../src/tr101290-summary.c ../src/streammodel.c ../src/streammodel-extractors.c ../src/pat.c ../src/descriptor.c ../src/crc32.c ../src/ts.c ../src/sectionextractor.c ../src/stats.c ../src/clocks.c ../src/history-metric.c

test_tr101290: test_tr101290.c $(TR101290_SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) $(DVBPSI_CFLAGS) -o $@ test_tr101290.c $(TR101290_SRCS) $(DVBPSI_LIBS) -lpthread

test_nal_bitreader: test_nal_bitreader.c ../src/nal_bitreader.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_nal_bitreader.c ../src/nal_bitreader.c

test_udp_receiver: test_udp_receiver.c ../src/udp_receiver.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_udp_receiver.c ../src/udp_receiver.c -lpthread

test_xorg_list: test_xorg_list.c ../src/libltntstools/xorg-list.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_xorg_list.c

test_vbv: test_vbv.c ../src/vbv.c ../src/pes.c ../src/crc32.c ../src/time.c ../src/utils.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_vbv.c ../src/vbv.c ../src/pes.c ../src/crc32.c ../src/time.c ../src/utils.c -lpthread

clean:
	rm -f test_crc32 test_history_metric test_ts test_reframer test_stats test_descriptor test_pes test_pat test_sectionextractor test_ts_packetizer test_throughput_hires test_time test_throughput test_streammodel test_streammodel_extractors test_pes_extractor test_sei_timestamp test_smoother_pcr test_probes test_smoother_rtp test_segmentwriter test_klbitstream_readwriter test_tr101290 test_nal_bitreader test_udp_receiver test_xorg_list test_vbv
