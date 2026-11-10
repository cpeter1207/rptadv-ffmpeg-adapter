# rptadv-ffmpeg-adapter

`rptadv-ffmpeg-adapter` is the versioned dynamic FFmpeg graph boundary
for `rpt_advanced` and USBRadioPlus. It deliberately owns the unstable FFmpeg
C ABI behind a small, versioned descriptor. Consumers exchange mono,
normalized F32 PCM and never include FFmpeg headers or link FFmpeg directly.

ABI v1 creates one fixed-rate graph from a caller-supplied FFmpeg filter
description and processes bounded, sample-preserving PCM blocks. It is an
adapter used during the USBRadioPlus native radio-engine migration.
FFmpeg remains a dynamic runtime
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

The optional `buffering_state` tail reports owner-serialized accepted/delivered
counters, actual output-FIFO occupancy, and bridge-generated prefix/suffix zero
counts. It does not consume PCM, allocate, infer source time, or qualify arbitrary
graphs. Delivered frames exclude bridge padding. Faults and counter overflow
invalidate accounting until quiescent replacement. The independent model-v1 flag
identifies audited FFmpeg 7.1.1–7.1.5 releases; unsupported backends keep
ordinary processing available. An SDK-owned audited sample-preserving recipe
must separately bound filter lookahead and conversion history before interpreting
counts as programme delay. Custom graph counters alone cannot establish that
bound. Fresh graph creation remains the history-retirement contract; ordinary
unkey does not reset gain history.

The compatible ABI/SONAME-1 extension requires checking
`RPTADV_FFMPEG_ADAPTER_DESCRIPTOR_V1_BUFFERING_MIN_SIZE`; older complete prefixes
remain valid for ordinary processing. The adapter now directly links dynamic
`libswresample` for backend-model version qualification and requires its
development package at build time. Debian derives the runtime dependency through
shlibs. The buffering record is 48 bytes and accepts larger caller tails without
touching them.

The model flag requires the audited release range, libavfilter 10.4/10.5.100,
and libswresample 5.3.100. It is cached at preparation, independently of counter
validity. A future or unsupported release leaves ordinary processing available
and clears this flag. The following upstream files are byte-identical in every
tag from n7.1.1 through n7.1.5; these hashes identify the limiter and finite
resampler/filter/phase implementation used by the separate SDK recipe proof:

| Upstream source | SHA-256 for each audited tag |
| --- | --- |
| [af_alimiter.c](https://github.com/FFmpeg/FFmpeg/blob/n7.1.5/libavfilter/af_alimiter.c) | `dc15973088a1e6197ae1001cfecbc0d847c479571563c37b5e2efe4073fa56eb` |
| [af_aresample.c](https://github.com/FFmpeg/FFmpeg/blob/n7.1.5/libavfilter/af_aresample.c) | `3bf7787762f4a816938d9f32737458d245206d03f8ec8a8f043b00d06246ab77` |
| [resample.c](https://github.com/FFmpeg/FFmpeg/blob/n7.1.5/libswresample/resample.c) | `58a342e68ad69945ff8d496b4d11a7471063d198ea287f09bb03fb0f368c61ec` |
| [options.c](https://github.com/FFmpeg/FFmpeg/blob/n7.1.5/libswresample/options.c) | `82b3a882ff3b812517153ab3a1b9e991fcd4903043baad1f038493d958de6c7f` |

Counter validity and model support do not certify a caller's recipe. An arbitrary
delay/FFT/duration-changing graph may keep old programme despite current counters
and must remain unqualified until its own buffering contract is established.

The public contract is in
[`include/rptadv_ffmpeg_adapter/rptadv_ffmpeg_adapter.h`](include/rptadv_ffmpeg_adapter/rptadv_ffmpeg_adapter.h).
Build a local shared object with `make`, run focused checks with `make test`,
and stage an install with `make install-check`. `make quality` runs the fast
source checks. `make platform-verify` verifies builds, tests, staged installs,
Debian packages, and source archives. The pull-request gate additionally
requires 100% production Rust and C line and branch coverage on Debian 13
amd64 before this adapter can be released.
