use libltntstools_sys::*;
use std::{time};
use core::ffi::c_int;

const UNALIGNED_TS: &[u8] = include_bytes!("unaligned.ts");

#[test]
fn test_find_sync_position() {
    let position = unsafe { findSyncPosition(UNALIGNED_TS.as_ptr(), UNALIGNED_TS.len() as _) };
    assert_eq!(position, 155);
}

pub fn basic_stream_stats_do_something(s: *mut stream_statistics_s)
{
    unsafe {
        (*s).packetCount += 1;
    };
}

fn basic_clocks_init(clk: *mut clock_s)
{
    unsafe
    {
        clock_initialize(clk);
        clock_establish_timebase(clk, 90000);
        clock_establish_wallclock(clk, 1500);

        if clock_is_established_timebase(clk) <= 0 {
            println!("Clock has no established timebase");
            assert!(false);
        } else {
            println!("Clock has effective established timebase");
        }

        if clock_is_established_wallclock(clk) <= 0 {
            println!("Clock has no established wallclock");
            assert!(false);
        } else {
            println!("Clock has effective established wallclock");
        }
    };
}

#[test]
fn test_basic_clocks()
{
    /* Put the clock struct on the stack */
    let mut clk = clock_s::default();

    /* Initalize it, setup some defaults.
     * Done in a differt funct intensionally, so we show
     * how to properly pass the reference around.
     * */
    basic_clocks_init(&mut clk);

    /* Yeah, sleep */
    std::thread::sleep( time::Duration::from_millis(32) );

    /* Update the ticks and check we drifted -15ms from normality. */
    let ms;
    unsafe {
        clock_set_ticks(&mut clk, 3000);
        ms = clock_get_drift_ms(&mut clk);
    };

    println!("Clock drifted {} ms, should be -15ms", ms);
    assert!(ms == -15);
}

#[test]
fn test_basic_pid_stats() {
    /* This is a 3.5MB struct, too big for the stack, allocate from the heap */
    use std::alloc::{alloc, dealloc, Layout};
    use std::io::Read;

    unsafe {
        let stats = Layout::new::<stream_statistics_s>();
        let stats_ptr = alloc(stats) as *mut stream_statistics_s;

        (*stats_ptr).packetCount = 5;
        println!("A. stats.packetCount = {}", (*stats_ptr).packetCount);

        /* This will reset the packetCount and all other struct vars. */
        pid_stats_reset(stats_ptr);

        // TODO: absolute path needs fixed.
        let mut file_in = std::fs::File::open("/tmp/demo.ts").unwrap();
        let mut buffer = [0u8; 128 * 188]; /* Stack */
        let mut processed = 0;

        loop {
            let nbytes = file_in.read(&mut buffer).unwrap();
            if nbytes < buffer.len() {
                break;
            }
            processed += nbytes;
            //println!("pid stats: Read {} / {} bytes", nbytes, processed);

            let b: u32 = nbytes.try_into().unwrap();

            pid_stats_update(stats_ptr, &buffer[0], b / 188);
        }

        pid_stats_dprintf(stats_ptr, 1);

        let r = pid_stats_pid_get_contains_pcr(stats_ptr, 0x31);
        assert!(r == 0);

        let cc = pid_stats_stream_get_cc_errors(stats_ptr);
        assert!(cc == 0);

        let pc = pid_stats_pid_get_packet_count(stats_ptr, 0x31);
        assert!(pc == 4206);

        // TODO: DO we dealloc here?
//        dealloc(stats_ptr, stats);
    };
}

#[test]
fn test_basic_stream_model()
{
    let mut handle = std::ptr::null_mut();
    use std::io::Read;

    let _result = unsafe {
        streammodel_alloc(&mut handle as _, std::ptr::null_mut());
    };

    // TODO: absolute path needs fixed.
    let mut file_in = std::fs::File::open("/tmp/demo.ts").unwrap();
    let mut buffer = [0u8; 128 * 188];
    let mut processed = 0;

    let mut val: i32 = 0;
    let val_ptr = &mut val as *mut c_int;
    loop {
        let nbytes = file_in.read(&mut buffer).unwrap();
        if nbytes < buffer.len() {
            break;
        }
        processed += nbytes;
        println!("StreamModel - Read {} / {} bytes", nbytes, processed);

        let b: i32 = nbytes.try_into().unwrap();

        unsafe {
            streammodel_write(handle, &buffer[0], b / 188, val_ptr);
            if val == 1 {
                let mut pat = std::ptr::null_mut();
                streammodel_query_model(handle, &mut pat as _);

                /* Display the entire pat, pmt and descriptor model to console */
                pat_dprintf(pat, 1);
                pat_free(pat);

                break;
            }
        }

    };
    unsafe { streammodel_free(handle); };

    assert!(val == 1);

}
