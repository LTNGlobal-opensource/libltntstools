use libltntstools_sys::*;
use std::{
    alloc,
    ffi::{c_int, c_void},
    fs::File,
    io::prelude::*,
    ptr, thread, time,
};

const UNALIGNED_TS: &[u8] = include_bytes!("../../test-data/unaligned.ts");

#[test]
fn test_find_sync_position() {
    let position = unsafe { findSyncPosition(UNALIGNED_TS.as_ptr(), UNALIGNED_TS.len() as _) };
    assert_eq!(position, 155);
}

fn basic_clocks_init(clk: *mut clock_s) {
    unsafe {
        clock_initialize(clk);
        clock_establish_timebase(clk, 90000);
        clock_establish_wallclock(clk, 1500);

        if clock_is_established_timebase(clk) <= 0 {
            panic!("Clock has no established timebase");
        } else {
            println!("Clock has effective established timebase");
        }

        if clock_is_established_wallclock(clk) <= 0 {
            panic!("Clock has no established wallclock");
        } else {
            println!("Clock has effective established wallclock");
        }
    };
}

#[test]
fn test_basic_clocks() {
    /* Put the clock struct on the stack */
    let mut clk = clock_s::default();

    /* Initialize it, setup some defaults.
     * Done in a differt funct intensionally, so we show
     * how to properly pass the reference around.
     * */
    basic_clocks_init(&mut clk);

    /* Yeah, sleep */
    thread::sleep(time::Duration::from_millis(32));

    /* Update the ticks and check we drifted -15ms from normality. */
    let ms;
    unsafe {
        clock_set_ticks(&mut clk, 3000);
        ms = clock_get_drift_ms(&mut clk);
    };

    assert_eq!(ms, -15, "Clock drifted by some value other than -15ms");
}

#[test]
fn test_basic_pid_stats() {
    unsafe {
        /* This is a 3.5MB struct, too big for the stack, allocate from the heap */
        let mut stats = {
            let stats_layout = alloc::Layout::new::<stream_statistics_s>();
            let stats_ptr = alloc::alloc(stats_layout);
            ptr::write_bytes(stats_ptr, 0, stats_layout.size());
            Box::from_raw(stats_ptr as *mut stream_statistics_s)
        };
        let stats_ptr = stats.as_mut();

        stats_ptr.packetCount = 5;
        println!("A. stats.packetCount = {}", stats_ptr.packetCount);

        /* This will reset the packetCount and all other struct vars. */
        pid_stats_reset(stats_ptr);

        let mut file_in = File::open("../test-data/demo.ts").unwrap();
        let mut buffer = [0u8; 128 * 188]; /* Stack */
        //let mut processed = 0;

        loop {
            let nbytes = file_in.read(&mut buffer).unwrap();
            if nbytes < buffer.len() {
                break;
            }
            //processed += nbytes;
            //println!("pid stats: Read {} / {} bytes", nbytes, processed);

            let b: u32 = nbytes.try_into().unwrap();

            pid_stats_update(stats_ptr, &buffer[0], b / 188);
        }

        pid_stats_dprintf(stats_ptr, 1);

        let r = pid_stats_pid_get_contains_pcr(stats_ptr, 0x31);
        assert_eq!(r, 0);

        let cc = pid_stats_stream_get_cc_errors(stats_ptr);
        assert_eq!(cc, 0);

        let pc = pid_stats_pid_get_packet_count(stats_ptr, 0x31);
        assert_eq!(pc, 4206);
    };
}

#[test]
fn test_basic_stream_model() {
    let mut handle = ptr::null_mut();

    unsafe {
        streammodel_alloc(&mut handle as _, ptr::null_mut());
    }

    let mut file_in = File::open("../test-data/demo.ts").unwrap();
    let mut buffer = [0u8; 128 * 188];
    //let mut processed = 0;

    let mut val: i32 = 0;
    loop {
        let nbytes = file_in.read(&mut buffer).unwrap();
        if nbytes < buffer.len() {
            break;
        }
        //processed += nbytes;
        //println!("StreamModel - Read {} / {} bytes", nbytes, processed);

        let b: i32 = nbytes.try_into().unwrap();

        unsafe {
            let mut timestamp: libc::timeval = libc::timeval {
                tv_sec: 0,
                tv_usec: 0,
            };        
            libc::gettimeofday(&mut timestamp, std::ptr::null_mut());

            let val_ptr = &mut val as *mut c_int;
            streammodel_write(handle, &buffer[0], b / 188, val_ptr, &mut timestamp);
            if val == 1 {
                let mut pat = ptr::null_mut();
                streammodel_query_model(handle, &mut pat as _);

                /* Display the entire pat, pmt and descriptor model to console */
                pat_dprintf(pat, 1);
                pat_free(pat);

                break;
            }
        }
    }

    unsafe {
        streammodel_free(handle);
    }

    assert_eq!(val, 1);
}

#[allow(clippy::missing_safety_doc)]
pub unsafe extern "C" fn basic_pe_callback(_user_context: *mut c_void, pes: *mut ltn_pes_packet_s) {
    unsafe {
        //println!("PTS = {}", (*pes).PTS);
        //let s: i8 = 0;
        //ltn_pes_packet_dump(pes, &s);
        {
            let pes = &*pes;

            match pes.PTS {
                3591437680 => (),
                3591441280 => (),
                3591444880 => (),
                3591448480 => (),
                3591452080 => (),
                _ => panic!("Unexpected PTS {}", pes.PTS),
            };
        }

        ltn_pes_packet_free(pes);
    };
}

#[test]
fn test_basic_pes_extractor() {
    let mut handle = ptr::null_mut();

    unsafe {
        pes_extractor_alloc(
            &mut handle as _,
            0x31,
            0xe0,
            Some(basic_pe_callback),
            ptr::null_mut(),
        );
    }

    let mut file_in = File::open("../test-data/demo.ts").unwrap();
    let mut buffer = [0u8; 128 * 188];
    //let mut processed = 0;

    loop {
        let nbytes = file_in.read(&mut buffer).unwrap();
        if nbytes < buffer.len() {
            break;
        }
        //processed += nbytes;
        //println!("pes_extractor - Read {} / {} bytes", nbytes, processed);

        let b: i32 = nbytes.try_into().unwrap();

        unsafe {
            pes_extractor_write(handle, &buffer[0], b / 188);
        }
    }

    unsafe {
        pes_extractor_free(handle);
    }
}
