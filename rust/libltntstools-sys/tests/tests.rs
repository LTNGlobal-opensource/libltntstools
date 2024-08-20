use libltntstools_sys::*;

const UNALIGNED_TS: &[u8] = include_bytes!("../tests/unaligned.ts");

#[test]
fn test_find_sync_position() {
    let position = unsafe { findSyncPosition(UNALIGNED_TS.as_ptr(), UNALIGNED_TS.len() as _) };
    assert_eq!(position, 155);
}
