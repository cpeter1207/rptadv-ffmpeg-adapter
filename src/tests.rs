//! Focused descriptor and configuration validation tests.

use std::ffi::c_char;
use std::mem::size_of;
use std::ptr;

use super::{ABI_VERSION, AdapterDescriptor, CAPABILITY_NAME, GraphConfig, INVALID_ARGUMENT};

/// Return a valid ABI-v1 configuration without invoking the C FFmpeg bridge.
fn valid_config() -> GraphConfig {
    GraphConfig {
        struct_size: size_of::<GraphConfig>() as u32,
        abi_version: ABI_VERSION,
        sample_rate_hz: 48_000,
        maximum_frame_count: 960,
        filter_description: c"anull".as_ptr().cast::<c_char>(),
    }
}

#[test]
fn descriptor_identifies_complete_abi_v1_function_table() {
    let descriptor = unsafe { &*super::rptadv_ffmpeg_adapter_descriptor() };

    assert_eq!(descriptor.abi_version, ABI_VERSION);
    assert!(descriptor.struct_size as usize >= size_of::<AdapterDescriptor>());
    assert_eq!(
        unsafe { std::ffi::CStr::from_ptr(descriptor.capability_name) }.to_bytes(),
        &CAPABILITY_NAME[..CAPABILITY_NAME.len() - 1]
    );
    let _ = descriptor.create;
    let _ = descriptor.process;
    let _ = descriptor.destroy;
    let _ = descriptor.process_block;
}

#[test]
fn process_block_rejects_a_missing_graph_before_entering_the_bridge() {
    let descriptor = unsafe { &*super::rptadv_ffmpeg_adapter_descriptor() };
    let input = [0.0_f32];
    let mut output = [0.0_f32];

    assert_eq!(
        (descriptor.process_block)(ptr::null_mut(), input.as_ptr(), 1, output.as_mut_ptr()),
        INVALID_ARGUMENT
    );
}

#[test]
fn config_validation_rejects_bad_prefix_and_fixed_properties() {
    let mut config = valid_config();
    assert!(super::valid_config(&config));
    config.struct_size = 0;
    assert!(!super::valid_config(&config));
    config = valid_config();
    config.abi_version += 1;
    assert!(!super::valid_config(&config));
    config = valid_config();
    config.sample_rate_hz = 0;
    assert!(!super::valid_config(&config));
    config = valid_config();
    config.maximum_frame_count = 0;
    assert!(!super::valid_config(&config));
    config = valid_config();
    config.maximum_frame_count = i32::MAX as u32 + 1;
    assert!(!super::valid_config(&config));
    config = valid_config();
    config.filter_description = ptr::null();
    assert!(!super::valid_config(&config));
}

#[test]
fn create_rejects_bad_public_arguments_before_calling_ffmpeg() {
    let descriptor = unsafe { &*super::rptadv_ffmpeg_adapter_descriptor() };
    let mut graph = ptr::null_mut();
    let mut config = valid_config();

    assert_eq!(
        (descriptor.create)(ptr::null(), &mut graph),
        INVALID_ARGUMENT
    );
    assert_eq!(
        (descriptor.create)(&config, ptr::null_mut()),
        INVALID_ARGUMENT
    );
    config.filter_description = c"".as_ptr().cast::<c_char>();
    assert_eq!((descriptor.create)(&config, &mut graph), INVALID_ARGUMENT);
    assert!(graph.is_null());
}
