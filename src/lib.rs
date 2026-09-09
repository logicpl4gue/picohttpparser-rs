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
//! - ✅ Milestone 2 (request parser): safe zero-copy core (`core`,
//!   `request`) behind the FFI seam; 87,052 differential cases vs the C
//!   oracle with 0 mismatches (`scripts/diff_request.sh`).
//! - ✅ Milestone 3 (response parser): `response` reuses the shared core;
//!   20,678 differential cases with 0 mismatches (`scripts/diff_response.sh`).
//! - ✅ Milestone 4 (standalone header parser): `phr_parse_headers` wired
//!   straight to the shared core; 21,728 differential cases with 0 mismatches
//!   (`scripts/diff_headers.sh`).
//! - ✅ Milestone 5 (chunked decoder): stateful in-place `chunked` core;
//!   3,506 differential cases with 0 mismatches (`scripts/diff_chunked.sh`).
//!   All five C entry points are live; no stubs remain.
//! - ⏳ Milestone 6+ (compatibility gate, fuzzing, benchmarks, consumer
//!   integration): not started.

#![warn(missing_docs)]

pub mod ffi;

/// Chunked-transfer decoder (Milestone 5, the only stateful parser).
mod chunked;
/// Safe, allocation-free parsing core (offsets in, offsets out).
mod core;
/// Request-line parser (Milestone 2).
mod request;
/// Response-line parser (Milestone 3).
mod response;

pub use ffi::{PhrChunkedDecoder, PhrHeader};
