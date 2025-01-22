use crate::{PesPacket, TS_PACKET_LEN};
use libc::c_void;
use std::{
    io::{self, prelude::*},
    marker::PhantomData,
    ptr,
};

/// Parse and demux MPEG transport streams and produce fully formed PES
/// structures for analysis other work. Capable of parsing fixed length PES
/// packets or variable length packets (larger than 65536 bytes).
#[derive(Debug)]
pub struct PesExtractor<F>
where
    F: FnMut(PesPacket),
{
    handle: *mut c_void,
    pid: u16,
    streamid: u8,
    callback: *mut F,
    phantom_callback: PhantomData<F>,
    buffer_min: i32,
    buffer_max: i32,
}

impl<F> PesExtractor<F>
where
    F: FnMut(PesPacket),
{
    /// Allocate a framework context capable of demuxing and parsing PES streams.
    ///
    /// The extractor will demux the MPEG-TS transport PID given by `pid` and look for the PES
    /// StreamID given by `streamid` (e.g. `0xc0` for audio and `0xe0` for video).
    ///
    /// Found PES frames will be supplied to the given `callback`.
    pub fn new(pid: u16, streamid: u8, callback: F, buffer_min: i32, buffer_max: i32) -> Self {
        log::debug!("Creating PES Extractor for PID {pid}, StreamID {streamid} {buffer_min} {buffer_max}");

        let callback = Box::into_raw(Box::new(callback));

        unsafe extern "C" fn callback_trampoline<F>(
            context: *mut libc::c_void,
            pes: *mut sys::ltn_pes_packet_s,
        ) where
            F: FnMut(PesPacket),
        {
            let callback = &mut *(context as *mut F);
            let packet = PesPacket::from_raw(pes);
            callback(packet);
        }

        let mut handle = ptr::null_mut();
        unsafe {
            sys::pes_extractor_alloc(
                &mut handle,
                pid,
                streamid,
                Some(callback_trampoline::<F>),
                callback as _,
                buffer_min,
                buffer_max,
            );
        }

        Self {
            handle,
            pid,
            streamid,
            callback,
            phantom_callback: PhantomData,
            buffer_min,
            buffer_max,
        }
    }

    /// Returns the MPEG-TS transport PID the extractor was created with.
    pub fn pid(&self) -> u16 {
        self.pid
    }

    /// Returns the PES StreamID the extractor was created with.
    pub fn streamid(&self) -> u8 {
        self.streamid
    }

    /// Returns the PES ring buffer minimum size
    pub fn buffer_min(&self) -> i32 {
        self.buffer_min
    }

    /// Returns the PES ring buffer minimum size
    pub fn buffer_max(&self) -> i32 {
        self.buffer_max
    }
    
}

impl<F> Write for PesExtractor<F>
where
    F: FnMut(PesPacket),
{
    /// Write an entire MPTS into the framework, pid filtering and demuxing the stream.
    ///
    /// Once an entire PES has been parsed, the caller is handed the PES structure via
    /// the callback.
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        if buf.is_empty() {
            return Ok(0);
        }

        if buf.len() < TS_PACKET_LEN {
            // We don't have an internal buffer to support partial writes
            return Err(io::Error::other(format!(
                "Partial write of {} bytes into PES Extractor",
                buf.len(),
            )));
        }

        let packet_count = i32::try_from(buf.len() / TS_PACKET_LEN).unwrap();
        log::debug!("Writing {packet_count} packets into PES Extractor");

        let ret = unsafe { sys::pes_extractor_write(self.handle, buf.as_ptr(), packet_count) };
        let written = usize::try_from(ret)
            .map_err(|_| io::Error::other("Failed to write into PES Extractor"))?;

        Ok(written * TS_PACKET_LEN)
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

impl<F> Drop for PesExtractor<F>
where
    F: FnMut(PesPacket),
{
    fn drop(&mut self) {
        unsafe {
            sys::pes_extractor_free(self.handle);
            drop(Box::from_raw(self.callback));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    #[test]
    fn test_basic_pes_extractor() {
        static EXPECTED_PTS: &[i64] = &[3591437680, 3591441280, 3591444880, 3591448480, 3591452080];

        let mut packets = Vec::new();
        let mut extractor = PesExtractor::new(0x31, 0xe0, |packet| packets.push(packet), -1, -1);
        let mut ts_file = io::BufReader::with_capacity(
            // Needs to be more than 8 KiB so io::copy will use
            // the reader's buffer and our IO is packet-aligned
            64 * TS_PACKET_LEN,
            fs::File::open("../test-data/demo.ts").unwrap(),
        );
        io::copy(&mut ts_file, &mut extractor).unwrap();
        drop(extractor); // Allow access to `packets`

        assert_eq!(packets.len(), EXPECTED_PTS.len(), "Wrong packet count");

        for (i, (packet, expected_pts)) in
            packets.iter().zip(EXPECTED_PTS.iter().copied()).enumerate()
        {
            assert_eq!(packet.PTS, expected_pts, "PTS mismatch at packets[{i}]");
        }
    }
}
