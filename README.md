# rptadv-ffmpeg-adapter

`rptadv-ffmpeg-adapter` is the future versioned dynamic FFmpeg graph boundary
for `rpt_advanced` and USBRadioPlus. It deliberately owns the unstable FFmpeg
C ABI behind a small, versioned descriptor. Consumers exchange mono,
normalized F32 PCM and never include FFmpeg headers or link FFmpeg directly.

ABI v1 creates one fixed-rate graph from a caller-supplied FFmpeg filter
description and processes bounded, sample-preserving PCM blocks. It is an
isolated migration scaffold: USBRadioPlus does not use it yet and the adapter
does not add a second DSP implementation. FFmpeg remains a dynamic runtime
dependency; this project neither vendors nor static-links it.

The original ABI-v1 streaming entry reports the graph output currently
available to the caller.  ABI-v1 also has an append-only optional
`process_block` descriptor entry for prepared real-time callers. It returns
exactly one normalized F32 output block for every input block and owns a fixed
eight-frame source pool plus a bounded delayed-output FIFO allocated at graph
creation. A consumer must check
`RPTADV_FFMPEG_ADAPTER_DESCRIPTOR_V1_PROCESS_BLOCK_MIN_SIZE` and the function
pointer before using it; older ABI-v1 providers expose only the streaming
prefix. A graph uses either entry for its lifetime, never both.

The public contract is in
[`include/rptadv_ffmpeg_adapter/rptadv_ffmpeg_adapter.h`](include/rptadv_ffmpeg_adapter/rptadv_ffmpeg_adapter.h).
Build a local shared object with `make`, run focused checks with `make test`,
and stage an install with `make install-check`. `make quality` runs the fast
source checks. `make platform-verify` verifies builds, tests, staged installs,
Debian packages, and source archives. The pull-request gate additionally
requires 100% production Rust and C line and branch coverage on Debian 13
amd64 before this adapter can be released.
