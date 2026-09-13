# rptadv-ffmpeg-adapter development rules

This is a versioned dynamic adapter under ADR 0018, ADR 0021, ADR 0022, and
ADR 0029. Keep its public C descriptor narrow and ABI-versioned. The adapter
owns FFmpeg headers and ABI details, while consumers exchange only normalized
mono F32 PCM in the nominal range -1.0 through +1.0. Do not vendor or
static-link FFmpeg. Update the public header, focused C ABI test, Rustdoc, and
Doxygen comments together with every ABI change.

Quality checks must not rewrite source files. Treat warnings as errors. Use
source-owned, labeled disposable containers when a Linux test environment is
needed, and remove stale containers before and after a run.

