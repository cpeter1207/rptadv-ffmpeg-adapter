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

/// Retain one real FFmpeg graph for the duration of an ABI test.
struct TestGraph(*mut super::Graph);

impl TestGraph {
    fn new(filter: &std::ffi::CStr) -> Self {
        let mut config = valid_config();
        config.maximum_frame_count = 8;
        config.filter_description = filter.as_ptr();
        let mut graph = ptr::null_mut();
        assert_eq!(super::create(&config, &mut graph), super::OK);
        assert!(!graph.is_null());
        Self(graph)
    }
}

impl Drop for TestGraph {
    fn drop(&mut self) {
        super::destroy(self.0);
    }
}

#[test]
fn graph_creation_propagates_configuration_and_filter_failures() {
    let mut config = valid_config();
    let mut graph = ptr::null_mut();
    config.struct_size = 0;
    assert_eq!(super::create(&config, &mut graph), INVALID_ARGUMENT);
    config = valid_config();
    config.filter_description = c"missing_rptadv_filter".as_ptr();
    assert_eq!(super::create(&config, &mut graph), super::FFMPEG_ERROR);
    assert!(graph.is_null());
    super::destroy(ptr::null_mut());
}

#[test]
fn streaming_checks_every_pointer_and_capacity_before_processing() {
    let graph = TestGraph::new(c"volume=0.5");
    let input = [0.5_f32; 8];
    let mut output = [0.0_f32; 8];
    let mut used = 99;
    let mut generated = 99;
    for (handle, used_out, generated_out) in [
        (
            ptr::null_mut(),
            &mut used as *mut u32,
            &mut generated as *mut u32,
        ),
        (graph.0, ptr::null_mut(), &mut generated as *mut u32),
        (graph.0, &mut used as *mut u32, ptr::null_mut()),
    ] {
        assert_eq!(
            super::process(
                handle,
                input.as_ptr(),
                8,
                output.as_mut_ptr(),
                8,
                used_out,
                generated_out
            ),
            INVALID_ARGUMENT
        );
    }
    for (input_ptr, count, output_ptr, capacity) in [
        (input.as_ptr(), 9, output.as_mut_ptr(), 8),
        (input.as_ptr(), 8, ptr::null_mut(), 8),
        (input.as_ptr(), 8, output.as_mut_ptr(), 7),
        (ptr::null(), 8, output.as_mut_ptr(), 8),
    ] {
        assert_eq!(
            super::process(
                graph.0,
                input_ptr,
                count,
                output_ptr,
                capacity,
                &mut used,
                &mut generated
            ),
            INVALID_ARGUMENT
        );
        assert_eq!((used, generated), (0, 0));
    }
    assert_eq!(
        super::process(
            graph.0,
            ptr::null(),
            0,
            output.as_mut_ptr(),
            8,
            &mut used,
            &mut generated
        ),
        super::OK
    );
    assert_eq!((used, generated), (0, 0));
    assert_eq!(
        super::process(
            graph.0,
            input.as_ptr(),
            8,
            output.as_mut_ptr(),
            8,
            &mut used,
            &mut generated
        ),
        super::OK
    );
    assert_eq!((used, generated), (8, 8));
    assert_eq!(output, [0.25; 8]);
    assert_eq!(
        super::process_block(graph.0, input.as_ptr(), 8, output.as_mut_ptr()),
        INVALID_ARGUMENT
    );
}

#[test]
fn block_processing_preserves_delayed_samples_and_rejects_bad_arguments() {
    let graph = TestGraph::new(c"asetnsamples=n=16:p=0");
    let first = [0.25_f32; 8];
    let second = [-0.5_f32; 8];
    let mut output = [9.0_f32; 8];
    for (input_ptr, count, output_ptr) in [
        (ptr::null(), 8, output.as_mut_ptr()),
        (first.as_ptr(), 8, ptr::null_mut()),
        (first.as_ptr(), 0, output.as_mut_ptr()),
        (first.as_ptr(), 9, output.as_mut_ptr()),
    ] {
        assert_eq!(
            super::process_block(graph.0, input_ptr, count, output_ptr),
            INVALID_ARGUMENT
        );
    }
    assert_eq!(
        super::process_block(graph.0, first.as_ptr(), 8, output.as_mut_ptr()),
        super::OK
    );
    assert_eq!(output, [0.0; 8]);
    assert_eq!(
        super::process_block(graph.0, second.as_ptr(), 8, output.as_mut_ptr()),
        super::OK
    );
    assert_eq!(output, first);
    assert_eq!(
        super::process_block(graph.0, first.as_ptr(), 8, output.as_mut_ptr()),
        super::OK
    );
    assert_eq!(output, second);
    let mut used = 99;
    let mut generated = 99;
    assert_eq!(
        super::process(
            graph.0,
            first.as_ptr(),
            8,
            output.as_mut_ptr(),
            8,
            &mut used,
            &mut generated
        ),
        INVALID_ARGUMENT
    );
    assert_eq!((used, generated), (0, 0));
}

#[test]
fn explicit_non_f32_graph_output_is_normalized_at_the_adapter_boundary() {
    let graph = TestGraph::new(c"aformat=sample_fmts=s16");
    let input = [0.5_f32; 8];
    let mut output = [0.0_f32; 8];
    let mut used = 99;
    let mut generated = 99;
    assert_eq!(
        super::process(
            graph.0,
            input.as_ptr(),
            8,
            output.as_mut_ptr(),
            8,
            &mut used,
            &mut generated
        ),
        super::OK
    );
    assert_eq!((used, generated), (8, 8));
    assert_eq!(output, input);
    let block = TestGraph::new(c"aformat=sample_fmts=s16");
    assert_eq!(
        super::process_block(block.0, input.as_ptr(), 8, output.as_mut_ptr()),
        super::OK
    );
}
