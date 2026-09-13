//! Versioned F32 FFmpeg graph adapter for `rpt_advanced`.
//!
//! The exported descriptor hides FFmpeg's C API behind an opaque graph. ABI v1
//! accepts mono normalized F32 PCM and owns a fixed-rate, sample-preserving
//! graph for one caller. The C bridge deliberately contains all FFmpeg types.
//! The append-only `process_block` descriptor entry provides prepared
//! exact-block processing without changing the original streaming ABI prefix.

#![deny(unsafe_op_in_unsafe_fn)]

mod bridge;

use std::ffi::{c_char, c_int};
use std::mem::{offset_of, size_of};
use std::ptr::{self, NonNull};

/// ABI version exported by this adapter.
const ABI_VERSION: u32 = 1;
/// Stable name used to select this adapter capability.
const CAPABILITY_NAME: &[u8] = b"rptadv.ffmpeg\0";
/// Successful adapter result.
const OK: c_int = 0;
/// Invalid public pointer, rate, frame count, or graph description.
const INVALID_ARGUMENT: c_int = -1;
/// FFmpeg rejected or failed a requested operation.
const FFMPEG_ERROR: c_int = -2;
/// Smallest configuration prefix safe for an ABI-v1 provider to read.
const CONFIG_V1_MIN_SIZE: usize =
    offset_of!(GraphConfig, filter_description) + size_of::<*const c_char>();

const _: () = assert!(OK == 0);
const _: () = assert!(INVALID_ARGUMENT == -1);
const _: () = assert!(FFMPEG_ERROR == -2);

/// Fixed graph properties passed through the stable C ABI.
#[repr(C)]
pub struct GraphConfig {
    struct_size: u32,
    abi_version: u32,
    sample_rate_hz: u32,
    maximum_frame_count: u32,
    filter_description: *const c_char,
}

/// Opaque persistent FFmpeg graph represented by the public C ABI.
#[repr(C)]
pub struct Graph {
    bridge_graph: NonNull<bridge::BridgeGraph>,
    maximum_frame_count: u32,
}

impl Drop for Graph {
    /// Release the C-owned FFmpeg graph when the public handle is destroyed.
    fn drop(&mut self) {
        unsafe {
            bridge::rptadv_ffmpeg_bridge_destroy(self.bridge_graph.as_ptr());
        }
    }
}

/// Versioned descriptor exposed to C and adapter-neutral Rust consumers.
#[repr(C)]
pub struct AdapterDescriptor {
    struct_size: u32,
    abi_version: u32,
    capability_name: *const c_char,
    create: extern "C" fn(*const GraphConfig, *mut *mut Graph) -> c_int,
    process: extern "C" fn(*mut Graph, *const f32, u32, *mut f32, u32, *mut u32, *mut u32) -> c_int,
    destroy: extern "C" fn(*mut Graph),
    process_block: extern "C" fn(*mut Graph, *const f32, u32, *mut f32) -> c_int,
}

// The descriptor contains only immutable pointers to static code and storage.
unsafe impl Sync for AdapterDescriptor {}

/// Return whether a public graph configuration contains an ABI-v1 prefix.
fn valid_config(config: &GraphConfig) -> bool {
    config.struct_size as usize >= CONFIG_V1_MIN_SIZE
        && config.abi_version == ABI_VERSION
        && config.sample_rate_hz != 0
        && config.maximum_frame_count != 0
        && config.maximum_frame_count <= i32::MAX as u32
        && !config.filter_description.is_null()
}

/// Create one persistent FFmpeg graph through the C bridge.
extern "C" fn create(config: *const GraphConfig, out_graph: *mut *mut Graph) -> c_int {
    let (Some(config), Some(out_graph)) =
        (NonNull::new(config.cast_mut()), NonNull::new(out_graph))
    else {
        return INVALID_ARGUMENT;
    };
    unsafe {
        *out_graph.as_ptr() = ptr::null_mut();
    }
    let config = unsafe { config.as_ref() };
    if !valid_config(config) {
        return INVALID_ARGUMENT;
    }
    let description_is_empty = unsafe { *config.filter_description == 0 };
    if description_is_empty {
        return INVALID_ARGUMENT;
    }

    let mut bridge_graph = ptr::null_mut();
    let result = unsafe {
        bridge::rptadv_ffmpeg_bridge_create(
            config.sample_rate_hz,
            config.maximum_frame_count,
            config.filter_description,
            &mut bridge_graph,
        )
    };
    let Some(bridge_graph) = NonNull::new(bridge_graph) else {
        return FFMPEG_ERROR;
    };
    if result != bridge::OK {
        unsafe {
            bridge::rptadv_ffmpeg_bridge_destroy(bridge_graph.as_ptr());
        }
        return if result == bridge::INVALID_ARGUMENT {
            INVALID_ARGUMENT
        } else {
            FFMPEG_ERROR
        };
    }
    let graph = Box::new(Graph {
        bridge_graph,
        maximum_frame_count: config.maximum_frame_count,
    });
    unsafe {
        *out_graph.as_ptr() = Box::into_raw(graph);
    }
    OK
}

/// Process or drain one graph block through the C bridge.
extern "C" fn process(
    graph: *mut Graph,
    input: *const f32,
    input_frames: u32,
    output: *mut f32,
    output_capacity: u32,
    out_input_used: *mut u32,
    out_output_generated: *mut u32,
) -> c_int {
    let (Some(graph), Some(out_input_used), Some(out_output_generated)) = (
        NonNull::new(graph),
        NonNull::new(out_input_used),
        NonNull::new(out_output_generated),
    ) else {
        return INVALID_ARGUMENT;
    };
    unsafe {
        *out_input_used.as_ptr() = 0;
        *out_output_generated.as_ptr() = 0;
    }
    let graph = unsafe { graph.as_ref() };
    if input_frames > graph.maximum_frame_count
        || output.is_null()
        || output_capacity < graph.maximum_frame_count
        || (input_frames != 0 && input.is_null())
    {
        return INVALID_ARGUMENT;
    }
    let result = unsafe {
        bridge::rptadv_ffmpeg_bridge_process(
            graph.bridge_graph.as_ptr(),
            input,
            input_frames,
            output,
            output_capacity,
            out_input_used.as_ptr(),
            out_output_generated.as_ptr(),
        )
    };
    if result == bridge::OK {
        OK
    } else if result == bridge::INVALID_ARGUMENT {
        INVALID_ARGUMENT
    } else {
        unsafe {
            *out_input_used.as_ptr() = 0;
            *out_output_generated.as_ptr() = 0;
        }
        FFMPEG_ERROR
    }
}

/// Process one exact F32 block through the preallocated real-time bridge path.
///
/// This is appended to the ABI-v1 descriptor rather than changing the original
/// streaming entry. One graph selects either API for its lifetime because their
/// output-draining contracts differ.
extern "C" fn process_block(
    graph: *mut Graph,
    input: *const f32,
    frame_count: u32,
    output: *mut f32,
) -> c_int {
    let Some(graph) = NonNull::new(graph) else {
        return INVALID_ARGUMENT;
    };
    let graph = unsafe { graph.as_ref() };
    if input.is_null()
        || output.is_null()
        || frame_count == 0
        || frame_count > graph.maximum_frame_count
    {
        return INVALID_ARGUMENT;
    }
    let result = unsafe {
        bridge::rptadv_ffmpeg_bridge_process_block(
            graph.bridge_graph.as_ptr(),
            input,
            frame_count,
            output,
        )
    };
    if result == bridge::OK {
        OK
    } else if result == bridge::INVALID_ARGUMENT {
        INVALID_ARGUMENT
    } else {
        FFMPEG_ERROR
    }
}

/// Destroy one graph after all callers have stopped using it.
extern "C" fn destroy(graph: *mut Graph) {
    let Some(graph) = NonNull::new(graph) else {
        return;
    };
    unsafe {
        drop(Box::from_raw(graph.as_ptr()));
    }
}

/// Immutable ABI-v1 production descriptor retained for the process lifetime.
static DESCRIPTOR: AdapterDescriptor = AdapterDescriptor {
    struct_size: size_of::<AdapterDescriptor>() as u32,
    abi_version: ABI_VERSION,
    capability_name: CAPABILITY_NAME.as_ptr().cast::<c_char>(),
    create,
    process,
    destroy,
    process_block,
};

/// Return the immutable function table for ABI version one.
#[unsafe(no_mangle)]
pub extern "C" fn rptadv_ffmpeg_adapter_descriptor() -> *const AdapterDescriptor {
    &DESCRIPTOR
}

#[cfg(test)]
mod tests;
