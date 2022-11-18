
AUTOMAKE_OPTIONS = foreign

lib_LTLIBRARIES = libltntstools.la

libltntstools_la_SOURCES  = klbitstream_readwriter.h
libltntstools_la_SOURCES += xorg-list.h
libltntstools_la_SOURCES += ts_packetizer.c
libltntstools_la_SOURCES += libltntstools/ts_packetizer.h
libltntstools_la_SOURCES += klringbuffer.h
libltntstools_la_SOURCES += klringbuffer.c
libltntstools_la_SOURCES += udp_receiver.c
libltntstools_la_SOURCES += libltntstools/udp_receiver.h
libltntstools_la_SOURCES += ts.c
libltntstools_la_SOURCES += libltntstools/pat.h
libltntstools_la_SOURCES += pat.c
libltntstools_la_SOURCES += nal_h264.c
libltntstools_la_SOURCES += nal_h265.c
libltntstools_la_SOURCES += libltntstools/ts.h
libltntstools_la_SOURCES += stats.c
libltntstools_la_SOURCES += libltntstools/stats.h
libltntstools_la_SOURCES += pes.c
libltntstools_la_SOURCES += libltntstools/pes.h
libltntstools_la_SOURCES += libltntstools/histogram.h
libltntstools_la_SOURCES += throughput.c
libltntstools_la_SOURCES += throughput_hires.c
libltntstools_la_SOURCES += clocks.c
libltntstools_la_SOURCES += time.c
libltntstools_la_SOURCES += libltntstools/time.h
libltntstools_la_SOURCES += segmentwriter.c
libltntstools_la_SOURCES += libltntstools/segmentwriter.h
libltntstools_la_SOURCES += tr101290-types.h
libltntstools_la_SOURCES += tr101290.c
libltntstools_la_SOURCES += tr101290-events.h
libltntstools_la_SOURCES += tr101290-events.c
libltntstools_la_SOURCES += tr101290-alarms.h
libltntstools_la_SOURCES += tr101290-alarms.c
libltntstools_la_SOURCES += tr101290-timers.h
libltntstools_la_SOURCES += tr101290-timers.c
libltntstools_la_SOURCES += tr101290-p1.h
libltntstools_la_SOURCES += tr101290-p1.c
libltntstools_la_SOURCES += tr101290-p2.h
libltntstools_la_SOURCES += tr101290-p2.c
libltntstools_la_SOURCES += tr101290-summary.c
libltntstools_la_SOURCES += libltntstools/tr101290.h
libltntstools_la_SOURCES += libltntstools/streammodel.h
libltntstools_la_SOURCES += streammodel-types.h
libltntstools_la_SOURCES += streammodel.c
libltntstools_la_SOURCES += streammodel-extractors.c
libltntstools_la_SOURCES += kl-queue.c
libltntstools_la_SOURCES += descriptor.c
libltntstools_la_SOURCES += sectionextractor.c
libltntstools_la_SOURCES += probes.c
libltntstools_la_SOURCES += sei-timestamp.c
libltntstools_la_SOURCES += sei-timestamp.h
libltntstools_la_SOURCES += crc32.c
libltntstools_la_SOURCES += pes-extractor.c
libltntstools_la_SOURCES += smoother-pcr.c
libltntstools_la_SOURCES += proc-net-udp.c
libltntstools_la_SOURCES += audioanalyzer.c
libltntstools_la_SOURCES += nielsen-bindings.cpp
libltntstools_la_SOURCES += nielsen-bindings.h
libltntstools_la_SOURCES += h264_codec_metadata.h
libltntstools_la_SOURCES += h264_codec_metadata.c
libltntstools_la_SOURCES += h265_codec_metadata.h
libltntstools_la_SOURCES += h265_codec_metadata.c
libltntstools_la_SOURCES += kafka-platform.c
libltntstools_la_SOURCES += source-pcap.c
libltntstools_la_SOURCES += source-rcts.c
libltntstools_la_SOURCES += utils.h
libltntstools_la_SOURCES += utils.c
libltntstools_la_SOURCES += reframer.c
libltntstools_la_SOURCES += igmp.c
libltntstools_la_SOURCES += rtp-analyzer.c
libltntstools_la_SOURCES += libltntstools/rtp-analyzer.h

libltntstools_la_CFLAGS = -Wall -DVERSION=\"$(VERSION)\" -DPROG="\"$(PACKAGE)\"" \
	-D_FILE_OFFSET_BITS=64 -O3 -D_BSD_SOURCE -I$(top_srcdir)/include

if DEBUG
  libltntstools_la_CFLAGS += -g
endif

libltntstools_la_CFLAGS += -I../../ffmpeg

libltntstools_la_CXXFLAGS  = $(libltntstools_la_CFLAGS)
libltntstools_la_CXXFLAGS += -std=c++11

libltntstools_includedir = $(includedir)/libltntstools

libltntstools_include_HEADERS  = libltntstools/ltntstools.h
libltntstools_include_HEADERS += libltntstools/ts.h
libltntstools_include_HEADERS += libltntstools/nal_h264.h
libltntstools_include_HEADERS += libltntstools/nal_h265.h
libltntstools_include_HEADERS += libltntstools/ts_packetizer.h
libltntstools_include_HEADERS += libltntstools/stats.h
libltntstools_include_HEADERS += libltntstools/pes.h
libltntstools_include_HEADERS += libltntstools/udp_receiver.h
libltntstools_include_HEADERS += libltntstools/histogram.h
libltntstools_include_HEADERS += libltntstools/hexdump.h
libltntstools_include_HEADERS += libltntstools/throughput.h
libltntstools_include_HEADERS += libltntstools/throughput_hires.h
libltntstools_include_HEADERS += libltntstools/clocks.h
libltntstools_include_HEADERS += libltntstools/time.h
libltntstools_include_HEADERS += libltntstools/segmentwriter.h
libltntstools_include_HEADERS += libltntstools/tr101290.h
libltntstools_include_HEADERS += libltntstools/pat.h
libltntstools_include_HEADERS += libltntstools/streammodel.h
libltntstools_include_HEADERS += libltntstools/kl-queue.h
libltntstools_include_HEADERS += libltntstools/xorg-list.h
libltntstools_include_HEADERS += libltntstools/descriptor.h
libltntstools_include_HEADERS += libltntstools/sectionextractor.h
libltntstools_include_HEADERS += libltntstools/probes.h
libltntstools_include_HEADERS += libltntstools/crc32.h
libltntstools_include_HEADERS += libltntstools/pes-extractor.h
libltntstools_include_HEADERS += libltntstools/smoother-pcr.h
libltntstools_include_HEADERS += libltntstools/proc-net-udp.h
libltntstools_include_HEADERS += libltntstools/audioanalyzer.h
libltntstools_include_HEADERS += libltntstools/h264_codec_metadata.h
libltntstools_include_HEADERS += libltntstools/h265_codec_metadata.h
libltntstools_include_HEADERS += libltntstools/kafka-platform.h
libltntstools_include_HEADERS += libltntstools/source-pcap.h
libltntstools_include_HEADERS += libltntstools/source-rcts.h
libltntstools_include_HEADERS += libltntstools/reframer.h
libltntstools_include_HEADERS += libltntstools/igmp.h
libltntstools_include_HEADERS += libltntstools/rtp-analyzer.h
