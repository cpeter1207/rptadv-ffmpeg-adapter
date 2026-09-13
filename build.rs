//! Compile the adapter-owned FFmpeg C bridge and retain dynamic FFmpeg linkage.
//!
//! The bridge is project code compiled into the Rust shared object. FFmpeg is
//! discovered through pkg-config and remains a dynamic system dependency.

use std::env;
use std::path::PathBuf;
use std::process::Command;

/// Run one build command and return a clear Cargo build-script failure on error.
fn run(command: &mut Command, description: &str) {
    let status = command
        .status()
        .unwrap_or_else(|error| panic!("cannot start {description}: {error}"));
    assert!(status.success(), "{description} failed with {status}");
}

/// Query space-separated compiler flags from pkg-config for FFmpeg headers.
fn pkg_config_cflags() -> Vec<String> {
    let output = Command::new("pkg-config")
        .args(["--cflags", "libavfilter", "libavutil"])
        .output()
        .expect("cannot start pkg-config for libavfilter/libavutil");
    assert!(
        output.status.success(),
        "pkg-config could not locate development files for libavfilter/libavutil"
    );
    String::from_utf8(output.stdout)
        .expect("pkg-config emitted non-UTF-8 compiler flags")
        .split_whitespace()
        .map(ToOwned::to_owned)
        .collect()
}

/// Compile the private C bridge and request dynamic FFmpeg runtime libraries.
fn main() {
    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("Cargo did not set OUT_DIR"));
    let object = out_dir.join("ffmpeg_bridge.o");
    let archive = out_dir.join("librptadv_ffmpeg_bridge.a");
    let compiler = env::var("CC").unwrap_or_else(|_| "cc".to_owned());
    let archiver = env::var("AR").unwrap_or_else(|_| "ar".to_owned());
    let mut compile = Command::new(compiler);

    compile.args([
        "-std=c11",
        "-fPIC",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wpedantic",
        "-Isrc",
    ]);
    compile.args(pkg_config_cflags());
    compile
        .args(["-c", "src/ffmpeg_bridge.c", "-o"])
        .arg(&object);
    run(&mut compile, "C compiler for FFmpeg bridge");

    let mut archive_command = Command::new(archiver);
    archive_command.args(["crus"]).arg(&archive).arg(&object);
    run(&mut archive_command, "archiver for FFmpeg bridge");

    println!("cargo:rustc-link-search=native={}", out_dir.display());
    println!("cargo:rustc-link-lib=static=rptadv_ffmpeg_bridge");
    println!("cargo:rustc-link-lib=dylib=avfilter");
    println!("cargo:rustc-link-lib=dylib=avutil");
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=src/ffmpeg_bridge.c");
    println!("cargo:rerun-if-changed=src/ffmpeg_bridge.h");
}
