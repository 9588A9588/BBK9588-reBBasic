# Changelog

All notable changes to this project will be documented here.

## Unreleased

- Reconstructed the BBASIC bytecode interpreter and 9588 BDA host backend.
- Added legacy headerless BIN and 16-byte BBE image loading.
- Added text, graphics pages, RGB565 LIB resources, input, timing, and file I/O.
- Added compatibility for the legacy 9388 environment check used by Rich.
- Corrected one-based LIB resource indexing and the 16-byte picture record header.
- Added host regression tests, GitHub Actions CI, and tag-driven BDA releases.
