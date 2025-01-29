use crate::{TS_PACKET_LEN};
use libc::c_void;
use std::{
    io::{self, prelude::*},
    marker::PhantomData,
    ptr,
};
use sys::pcr_position_s;
use std::sync::atomic::{AtomicUsize, Ordering};

#[derive(Debug)]
pub struct PcrSmoother<F>
where
    F: FnMut(Vec<u8>),
{
    handle: *mut c_void,
    callback: *mut F,
    phantom_callback: PhantomData<F>,
    items_per_second: i32,
    write_size: i32,
    pcr_pid: u16,
    latency_ms: i32,
}

impl<F> PcrSmoother<F>
where
    F: FnMut(Vec<u8>),
{
    pub fn new(pcr_pid: u16, items_per_second: i32, write_size: i32, latency_ms: i32, callback: F) -> Self {
        println!("Creating PCR Smoother for PID 0x{:x}", pcr_pid);

        let callback = Box::into_raw(Box::new(callback));

        unsafe extern "C" fn callback_trampoline<F>(
            user_context: *mut c_void,
            buf: *mut u8,
            byte_count: i32,
            _array: *mut pcr_position_s,
            _array_length: i32,
        ) -> i32 where
            F: FnMut(Vec<u8>),
        {
            let callback = &mut *(user_context as *mut F);
            let len = byte_count.try_into().unwrap();

            let mut vec = Vec::with_capacity(len);
            unsafe {
                let ptr = &buf;
                ptr.copy_to_nonoverlapping(vec.as_mut_ptr(), len);
                vec.set_len(len);
            };
            callback(vec);
            return 0;
        }

        let mut handle = ptr::null_mut();
        unsafe {
            sys::smoother_pcr_alloc(
                &mut handle,
                callback as _,
                Some(callback_trampoline::<F>),
                items_per_second,
                write_size,
                pcr_pid,
                latency_ms,
            );
        }

        Self {
            handle,
            callback,
            phantom_callback: PhantomData,
            items_per_second,
            write_size,
            pcr_pid,
            latency_ms,
        }
    }

    pub fn items_per_second(&self) -> i32 {
        self.items_per_second
    }
    pub fn write_size(&self) -> i32 {
        self.write_size
    }
    pub fn pcr_pid(&self) -> u16 {
        self.pcr_pid
    }
    pub fn latency_ms(&self) -> i32 {
        self.latency_ms
    }
}

impl<F> Write for PcrSmoother<F>
where
    F: FnMut(Vec<u8>),
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
                "Partial write of {} bytes into PCR Smoother",
                buf.len(),
            )));
        }

        // Reading packets from disk and smoothing them out with gettome of day,
        // is this even reliable?
        let mut timestamp: libc::timeval = libc::timeval {
            tv_sec: 0,
            tv_usec: 0,
        }; 
        let ret = unsafe {
            libc::gettimeofday(&mut timestamp, std::ptr::null_mut());
            sys::smoother_pcr_write(self.handle, buf.as_ptr(), buf.len().try_into().unwrap(), &mut timestamp)
        };
        if ret < 0 {
            io::Error::other("Failed to write into PCR Smoother");
        }

        Ok(buf.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

impl<F> Drop for PcrSmoother<F>
where
    F: FnMut(Vec<u8>),
{
    fn drop(&mut self) {
        unsafe {
            sys::smoother_pcr_free(self.handle);
            drop(Box::from_raw(self.callback));
        }
    }
}


#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    #[test]
    fn test_pcr_smoother() {

        println!("Running a test of the smoother");

        let stop_looking = AtomicUsize::new(0);
        
        let callback = |v: Vec<u8>| {
            println!("Callback received buffer with {} bytes, please send to UDP.", v.len());
            stop_looking.store(1, Ordering::Relaxed);
        };

        // Instantiate a smoother object
        let mut smoother = PcrSmoother::new(0x31, 5000, 1316, 50, callback);

        let mut file_in = fs::File::open("../test-data/pcrsmoother.ts").unwrap();
        let mut buffer = [0u8; 7 * 188];

        // Feed it content from a file
        while stop_looking.load(Ordering::Relaxed) == 0 {
            let nbytes = file_in.read(&mut buffer).unwrap();
            if nbytes < buffer.len() {
                break;
            }
            let _ = smoother.write(&buffer);
        }
    }
}
