# Changelog

All notable changes to this project will be documented here.

## Unreleased

- Reconstructed the BBASIC bytecode interpreter and 9588 BDA host backend.
- Added legacy headerless BIN and 16-byte BBE image loading.
- Added text, graphics pages, RGB565 LIB resources, input, timing, and file I/O.
- Added compatibility for the legacy 9388 environment check used by Rich.
- Corrected one-based LIB resource indexing and the 16-byte picture record header.
- Added host regression tests, GitHub Actions CI, and tag-driven BDA releases.

## Unreleased — 9688 runtime and audio/resource extensions

- Added bounded 4096-byte BIN loading with one VM buffer, staged loader errors and working-directory selection.
- Added non-blocking BDA message pumping and MIPS32-safe bounded unaligned VM access.
- Added streamed WAV playback, tones, pause/resume, BBAL v1 indexed audio resources and the two-voice mixer extension.
- Added resource paging, save primitives, render effects, map tile batches and screen-wave helpers.
- Added `AudioBB.lib`, `ResourceBB.lib`, `RenderBB.lib`, `MixerBB.lib`, WAV conversion/BBAL packing tools, and host regression coverage.
- Removed the public build helper's fixed local `E:/...` compiler fallback; use `BBASIC_TOOL_DIR` instead.
- Added [`docs/BBVM_9688_CHANGES_ZH.md`](docs/BBVM_9688_CHANGES_ZH.md) with design rationale, ABI details and verification limits.

The release still excludes firmware, original BDA files, game BIN/LIB/IDX/SAV resources, proprietary compiler binaries and toolchains.
