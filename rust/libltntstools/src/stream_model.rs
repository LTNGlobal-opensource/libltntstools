use crate::{TS_PACKET_LEN};
use libc::{c_void, c_int};
use std::{
    io::{self, prelude::*},
    marker::PhantomData,
    alloc,
    ptr,
};

#[derive(Debug)]
pub struct StreamModel<F>
where
    F: FnMut(Vec<u8>),
{
    handle: *mut c_void,
    callback: *mut F,
    phantom_callback: PhantomData<F>,
}

impl<F> StreamModel<F>
where
    F: FnMut(Vec<u8>),
{
    pub fn new(callback: F) -> Self {
        println!("Creating StreamModel");

        let callback = Box::into_raw(Box::new(callback));

        let mut handle = ptr::null_mut();
        unsafe {
            sys::streammodel_alloc(&mut handle, callback as _);
        }

        Self {
            handle,
            callback,
            phantom_callback: PhantomData,
        }
    }
}

impl<F> Write for StreamModel<F>
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

        let mut val: i32 = 0;

        // Reading packets from disk and smoothing them out with gettome of day,
        // is this even reliable?
        let mut timestamp: libc::timeval = libc::timeval {
            tv_sec: 0,
            tv_usec: 0,
        }; 
        unsafe {
            libc::gettimeofday(&mut timestamp, std::ptr::null_mut());

            let len: i32 = buf.len().try_into().expect("Buffer length exceeds i32 range");
    
            let val_ptr = &mut val as *mut c_int;
            let _ = sys::streammodel_write(self.handle, buf.as_ptr(), len / 188, val_ptr, &mut timestamp);

            if val == 1 {
                let mut pat = ptr::null_mut();

                sys::streammodel_query_model(self.handle, &mut pat as _);
                let mut sm = {
                    let sm_layout = alloc::Layout::new::<sys::pat_s>();
                    let sm_ptr = alloc::alloc(sm_layout);
                    ptr::write_bytes(sm_ptr, 0, sm_layout.size());
                    Box::from_raw(pat as *mut sys::pat_s)
                };
                let sm_ptr = sm.as_mut();
                println!("Total programs in stream {}", (*sm_ptr).program_count);

                /* Display the entire pat, pmt and descriptor model to console */
                //sys::pat_dprintf(pat, 1);
                //sys::pat_free(pat);

                //break;
            }
        };

        Ok(buf.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

impl<F> Drop for StreamModel<F>
where
    F: FnMut(Vec<u8>),
{
    fn drop(&mut self) {
        unsafe {
            sys::streammodel_free(self.handle);
            drop(Box::from_raw(self.callback));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    #[test]
    fn test_stream_model() {

        println!("Running a test of the stream_model");

        let callback = |v: Vec<u8>| {
            println!("Callback received buffer with entire pat object");
        };

        // Instantiate a model object
        let mut model = StreamModel::new(callback);

        let mut file_in = fs::File::open("../test-data/demo.ts").unwrap();
        let mut buffer = [0u8; 7 * 188];

        // Feed it content from a file
        loop {
            let nbytes = file_in.read(&mut buffer).unwrap();
            if nbytes < buffer.len() {
                break;
            }
            let _ = model.write(&buffer);
        }
        drop(model);
    }
}
