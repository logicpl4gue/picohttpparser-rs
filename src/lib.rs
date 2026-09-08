//! # picohttpparser-rs
//!
//! Drop-in Rust rewrite of
//! [PicoHTTPParser](https://github.com/h2o/picohttpparser) that preserves the
//! original's observable behavior and C-facing API/ABI closely enough that
//! existing consumers can switch without application-level changes.
//!
//! Status per the plan (`picohttpparser-rs-plan.md`):
//!
//! - ✅ Milestone 0 (baseline): upstream pinned in `reference/` (commit
//!   `f4d94b48b31e0abae029ebeafcfd9ca0680ede58`), repo scaffolded, methodology
//!   documented.
//! - ✅ Milestone 1 (ABI shell): `ffi` exports all five C symbols
//!   (`phr_parse_request`, `phr_parse_response`, `phr_parse_headers`,
//!   `phr_decode_chunked`, `phr_decode_chunked_is_in_data`) as **stubs** that
//!   pin names, calling convention, argument/return types, and struct layout
//!   but return `-1` (`0` for `_is_in_data`) until real parsing lands.
//! - ⏳ Milestone 2+ (request/response/headers/chunked parsers, differential
//!   testing, fuzzing, benchmarks): not started.

#![warn(missing_docs)]

pub mod ffi;

pub use ffi::{PhrChunkedDecoder, PhrHeader};
