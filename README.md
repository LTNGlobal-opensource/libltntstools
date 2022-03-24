# Features / APIs / Discrete Frameworks
    * Audio Analyzer
	* H264 / H265(HEVC) codec metadata extraction
	* PES parsing and extraction
	* Process monitoring API for UDP socket buffer overflows
	* MPEG Section extractor
	* PCR queueing / PCR smoothing framework - usefull for building bitrate smoothers
	* MPEG-TS ISO13818-1 stream monitoring / packet / pid loss detection
	* MPEG-TS StreamModel framework for quickly accessing the structure of a SPTS/MPTS
	* High reoslution counter framework for accurately tracking items/bits per second
	* MPEG-TS Packetizer - shovel a buffer of bytes into properly formed TS packets
	* UDP Recevier (slowly being deprecated in favor of a more generate libavformat handler)
    * Nielsel Audio wrapper framework, to expose nielsen codes (depends on proprietary SDK)
    * TR101290 framwork for tracking and monitoring P1 and P2 alerts.

# Utility classes / Features
    * Ring Buffer framework
    * Clock framework to accurately track drift across different timebases
	* General histogram utilities
    * General queue implementation - mutex protected and non-blocking.
	* Threaded file writer - trades memory for slow I/O

# LICENSE

	LGPL-V2.1
	See the included lgpl-2.1.txt for the complete license agreement.

