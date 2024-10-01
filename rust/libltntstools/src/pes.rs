use std::{
    ffi::CString,
    fmt::{self, Debug},
    ops::Deref,
};

/// ISO13818-1 PES packet. See ISO13818 spec table 2.18.
///
/// This type provides read access to an inner [`ltn_pes_packet_s`](sys::ltn_pes_packet_s) via an
/// implementation of `Deref`.
pub struct PesPacket(*mut sys::ltn_pes_packet_s);

impl PesPacket {
    /// Allocate a pes packet, with no payload data.
    pub fn new() -> Self {
        let handle = unsafe { sys::ltn_pes_packet_alloc() };
        // init should be unnecessary
        Self(handle)
    }

    /// Take ownership of an existing pes packet.
    pub(crate) unsafe fn from_raw(handle: *mut sys::ltn_pes_packet_s) -> Self {
        Self(handle)
    }

    /// Dump the packet content to console in readable format.
    pub fn dump(&self, prefix: &str) {
        let prefix = CString::new(prefix).unwrap();
        unsafe {
            sys::ltn_pes_packet_dump(self.0, prefix.as_ptr());
        }
    }

    /// Determine if this PES packet represents audio.
    pub fn is_audio(&self) -> bool {
        unsafe { sys::ltn_pes_packet_is_audio(self.0) != 0 }
    }

    /// Determine if this PES packet represents video.
    pub fn is_video(&self) -> bool {
        unsafe { sys::ltn_pes_packet_is_video(self.0) != 0 }
    }
}

impl Drop for PesPacket {
    fn drop(&mut self) {
        unsafe {
            sys::ltn_pes_packet_free(self.0);
        }
    }
}

impl Default for PesPacket {
    fn default() -> Self {
        Self::new()
    }
}

impl Clone for PesPacket {
    /* Clone, allocating */
    fn clone(&self) -> Self {
        let copy = Self::new();
        unsafe { sys::ltn_pes_packet_copy(copy.0, self.0) };
        copy
    }

    /* Clone, reusing an allocation */
    fn clone_from(&mut self, source: &Self) {
        unsafe {
            sys::ltn_pes_packet_init(self.0);
            sys::ltn_pes_packet_copy(self.0, source.0)
        };
    }
}

impl Deref for PesPacket {
    type Target = sys::ltn_pes_packet_s;

    fn deref(&self) -> &Self::Target {
        unsafe { &*self.0 }
    }
}

impl Debug for PesPacket {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_tuple("PesPacket").field(&**self).finish()
    }
}
