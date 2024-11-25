mod pes;
mod pes_extractor;

/// Length of a single MPEG-TS packet, in bytes.
pub const TS_PACKET_LEN: usize = 188;

pub use pes::PesPacket;
pub use pes_extractor::PesExtractor;
