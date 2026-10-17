# Streamed two-voice audio extension / version 100

The original ports 96–103 and AudioBB ABI remain intact. New OUT108 accepts
MIXCMD(path$, packet_address). OUT109 returns the extension version 100.
MixerBB.lib is prepended by compile_basic.py.

The packet is eight little-endian 32-bit words; unaligned BASIC arrays are valid.
Complete VM range validation is mandatory. Fields:

1. operation: 1 play, 2 stop, 3 fade out, 4 service/status
2. channel: 0 BGM, 1 SE; stop permits -1 for all voices
3. reserved (0)
4. one-based BBAL v1 resource ID
5. loop: 0 or 1
6. volume: 0–256
7. pitch: 50–200 percent, fixed-point nearest-sample resampling
8. play fade-in / fade-out duration in milliseconds, 0–600000

Path is a live string argument. Never save a temporary BASIC string handle
in an integer packet: the legacy compiler releases temporary argument strings
on function return.

Both channels stream 22050 Hz mono PCM8/16 members from one BBAL library, using
1024-byte per-channel input buffers. The native sum saturates to signed PCM16.
Two alternating 1024-byte output blocks retain partial firmware writes.
At most two blocks are submitted per 25 ms service tick, with estimated queue
capacity, finite backpressure timeout, and a bounded drain before PCM stop.
Starting a SE does not restart BGM or open a second firmware PCM endpoint.
Legacy playback stops the mixer; MIXCMD stops legacy playback. No ABI or memory
layout changes were made to the original bbvm_audio_t.

Firmware adapter uses a static mixer instance, no new heap allocations.
Spatial balance is folded to mono by the game port; not stereo reproduction.
Only host/MIPS firmware-stub tests have been performed, not physical sound/FPS.

Evidence and replay (game workspace):

    python -m unittest discover -s tests -p test_mixer_mips.py -v

Tests cover simultaneous BGM+SE, BGM continuation after SE, clipping, volume,
200% pitch, partial PCM writes, fade-out, invalid ranges/IDs, and cleanup.
# 游标扩展（2026-10-16）

MIXCMD 保留原8-word包与版本100，新增操作5/6，原1—4的ABI不变。此接口读写使用有界逐字节访问，BBASIC数组无需4字节对齐。

- op5查询：[5, channel, reserved, cursor_out, phase_out, active_out, volume_out, loop_out]。无文件读取/分配。cursor是当前已解码的源采样索引，phase是Q8重采样下一次待推进相位，不是声卡已经播放到的位置。
- op6恢复：[6, channel, reserved, cursor, phase, reserved, volume, reserved]。先成功打开目标成员，再恢复。cursor必须小于成员采样数，phase不超过该音高step+255（全局最多767），volume为0..256；边界失败不停止活动流。最大文件读取仍为1024字节，不整库载入。
- 游戏读档先停旧混音/PCM，再重开背景乐并seek，避免旧pending PCM混入新场景。直接对仍有驱动缓冲的流seek不保证队列立即清空。
- 当前不保存SE、淡变包络和固件PCM缓冲队列。恢复的是音乐资源、解码位置、音高相位与当前音量，不是硬件音频状态逐bit快照。
- 验证：游戏工程tests/test_mixer_mips.py中的150%音高游标PCM对照和tests/test_save_mips.py中的冷VM恢复；均为实际MIPS BDA+固件桩，不是设备出声测量。
# Original RPG2000 playback-once control (2026-10-16 / game rounds45-46)

- MIXCMD op7: [7,channel,reserved,played_once_out,samples_out,active_out,volume_out,loop_out]. No IO/allocation. The latch becomes 1 when the decoder reaches EOF/wrap; the existing bounded PCM queue can lead hardware playback by at most its queued samples, so this is not a hardware sample-position query. Natural EOF retains the latch after drain. Explicit stop/new track clears it.
- MIXCMD op8: [8,channel,reserved,played_once,unused,unused,unused,unused]. Restore only a 0/1 latch, with channel bounds; used after validated cold save restoration. Old op6 unused fields remain unused, preserving existing callers.
- Added one u32 per native channel; original bbvm_audio_t and ABI1..6 layouts are unchanged. Game saves now include ABGMONCE (119sections/199800bytes); old development fingerprints cannot be mixed.
- Game original conditional type9 (Map44/141) queries this latch, never substitutes a key wait. tests/test_mixer_mips.py covers repeat/EOF/drain/stop/new track/restore, test_gameplay_mips.py executes original false/true/stop branch, and cold-save test preserves the latch.
