
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
libltntstools_la_SOURCES += nal.c
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
libltntstools_la_SOURCES += tr101290-summary.c
libltntstools_la_SOURCES += libltntstools/tr101290.h
libltntstools_la_SOURCES += libltntstools/streammodel.h
libltntstools_la_SOURCES += streammodel.c
libltntstools_la_SOURCES += kl-queue.c
libltntstools_la_SOURCES += descriptor.c
libltntstools_la_SOURCES += sectionextractor.c
libltntstools_la_SOURCES += probes.c
libltntstools_la_SOURCES += sei-timestamp.c
libltntstools_la_SOURCES += sei-timestamp.h
libltntstools_la_SOURCES += crc32.c

libltntstools_la_CFLAGS = -Wall -DVERSION=\"$(VERSION)\" -DPROG="\"$(PACKAGE)\"" \
	-D_FILE_OFFSET_BITS=64 -O3 -D_BSD_SOURCE -I$(top_srcdir)/include

if DEBUG
  libltntstools_la_CFLAGS += -g
endif

libltntstools_includedir = $(includedir)/libltntstools

libltntstools_include_HEADERS  = libltntstools/ltntstools.h
libltntstools_include_HEADERS += libltntstools/ts.h
libltntstools_include_HEADERS += libltntstools/nal.h
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

