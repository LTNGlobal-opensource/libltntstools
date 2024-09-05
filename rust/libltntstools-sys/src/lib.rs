//! This crate contains the bindings for libltntstools created on the
//! fly using [bindgen](https://github.com/rust-lang/rust-bindgen).

#![allow(clippy::useless_transmute)]
// Warns about 128-bit integers not having a defined ABI
#![allow(improper_ctypes)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(non_upper_case_globals)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
