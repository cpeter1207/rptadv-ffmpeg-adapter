//! Private FFI declarations for the adapter-owned C FFmpeg bridge.
//!
//! FFmpeg headers and types remain contained in the C bridge, so no FFmpeg
//! layout or callback ABI leaks into the stable public Rust/C descriptor.

use std::ffi::{c_char, c_int};

/// Opaque C bridge graph state.
#[repr(C)]
pub(crate) struct BridgeGraph {
    _private: [u8; 0],
}

/// Successful private bridge result.
pub(crate) const OK: c_int = 0;
/// Invalid private bridge argument.
pub(crate) const INVALID_ARGUMENT: c_int = -1;

unsafe extern "C" {
    /// Create one C-owned FFmpeg graph.
    pub(crate) fn rptadv_ffmpeg_bridge_create(
        sample_rate_hz: u32,
        maximum_frame_count: u32,
        filter_description: *const c_char,
        out_graph: *mut *mut BridgeGraph,
    ) -> c_int;
    /// Process or drain one C-owned FFmpeg graph.
    pub(crate) fn rptadv_ffmpeg_bridge_process(
        graph: *mut BridgeGraph,
        input: *const f32,
        input_frames: u32,
        output: *mut f32,
        output_capacity: u32,
        out_input_used: *mut u32,
        out_output_generated: *mut u32,
    ) -> c_int;
    /// Process one complete preallocated F32 callback block.
    pub(crate) fn rptadv_ffmpeg_bridge_process_block(
        graph: *mut BridgeGraph,
        input: *const f32,
        frame_count: u32,
        output: *mut f32,
    ) -> c_int;
    /// Release one C-owned FFmpeg graph.
    pub(crate) fn rptadv_ffmpeg_bridge_destroy(graph: *mut BridgeGraph);
}
