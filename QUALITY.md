# Quality checks

`make quality` runs non-mutating format, lint, static analysis, and Doxygen
checks. Pull requests additionally require native Debian 13 amd64 and arm64
tests, dynamic linkage, staged installs, Debian packages, and source archives.
`make coverage` requires 100% production Rust and C line and branch coverage
on Debian 13 amd64. Tests and external FFmpeg sources are not production code
in this repository. Releases require a merged pull request with a passing gate.
