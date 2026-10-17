# BB 虚拟机 9688 版改动说明

本文说明本分支相对上游 `1ef5876b859ab3c5c9316d4048c42b4037e8c6f7` 的源码改动、设计原因、运行边界和验证方式。代码目标是步步高 9588/9688 同类 BDA 运行环境：MIPS32 little-endian、O32 调用约定、soft-float、非 Linux 裸机 BDA。项目不超频，不直接写未公开的音频或时钟 MMIO，也不把 ARM/Linux 的模拟器代码直接移植到设备上。

## 1. 提交范围

本仓库提交的是 BB 虚拟机和配套开发工具，不包含：

- 原厂 BDA、固件、NAND、图标原件；
- 游戏 BIN、LIB、IDX、SAV 或其他第三方资源；
- BBasic++、Blink、MIPS 工具链二进制；
- 用户账号、密码或本机路径。

游戏源代码和成品资源应在获得相应授权后，作为独立工程或本地资料包保存。这样可以让虚拟机源码在公开仓库中可复用，同时避免把游戏资源与运行时许可证混在一起。

## 2. 源码差异总表

相对上游修改的文件：

- `src/bbvm.h`
- `src/bbvm_core.c`
- `src/bbvm_runtime.c`
- `src/bda_backend.c`
- `src/main.c`

新增文件：

- `src/bbvm_audio.c`、`src/bbvm_audio.h`、`src/bbvm_audio_ports.c`
- `src/bbvm_mixer.c`、`src/bbvm_mixer.h`
- `src/bda_audio_backend.c`
- `src/bbvm_fx.h`、`src/bbvm_picture_fx.h`
- `src/bbvm_map_tiles.h`、`src/bbvm_screen_wave.h`
- `src/nobi_sound_index.h`

`bbvm_bda.h`、`bbvm_lib.c`、`bbvm_lib.h`、`bbvm_pixels.h` 与上游内容一致，随完整源码树保留，但不是本次新增改动。

## 3. 装载和大 BIN 文件处理

`src/main.c` 和 `src/bda_backend.c` 的装载流程针对 9688 的低内存、慢文件服务和旧文件系统做了收敛：

1. VM 使用一块 2 MiB 虚拟内存，不再同时保留一份完整 BIN 临时副本。
2. BIN 按 4096 字节分块读取。
3. 正短读会继续推进；零字节、负数或超过请求量的返回立即判错，避免死循环。
4. 文件大小、回读位置、VM 范围、BBE 头和程序负载分别校验。
5. 对文件服务调用和读取次数设置上限，文件异常时能退出而不是永久卡死。
6. 装载失败显示 L1—L6 阶段、路径、文件大小、已读字节和最后一次读取结果。
7. 文件选择器初始目录为 `B:\BBasic\`；选择后切换到 BIN 所在目录，使相对资源路径随游戏目录工作。
8. 成功装载前不启动游戏窗口，失败也会关闭文件并释放 VM 状态。

这解决的是“超过约 2 MiB 的程序或文件读取异常导致死机/无法判断”的运行时路径，不表示设备可以无限加载任意大小的程序。VM 可用容量仍由 2 MiB 内存、栈、程序映像和运行时字符串共同决定。

## 4. 消息泵和输入调度

旧路径在 `Idle` 或 GUI 服务上可能阻塞：输入越频繁，游戏反而越像变快；没有输入时则出现卡顿。当前主循环采用有限指令预算和有界非阻塞泵：

- 每次只执行有限 VM 指令，再调用音频服务；
- 按固定节拍调用 GUI/输入泵，而不是等待用户事件；
- 保留消息、原始触摸和实体按键处理；
- 继续处理未知消息前先验证队列布局；布局无法证明时停止当前解析，不回退到可能阻塞的旧路径。

这不是把游戏逻辑改成按键驱动，也没有通过快速点击人为“加速”游戏。

## 5. 解释器热路径和安全性

`src/bbvm_core.c` 的优化目标是 MIPS32 上减少无谓访存，同时保留边界保护：

- 对 VM 内小端数据使用有界的逐字节读取/写入，避免把任意 VM 地址强转成 `u32 *`；
- 未对齐数据可正常读取；
- 常用 `get_data` / `put_data` 内联；
- 数据清零采用对齐清零和小段展开；
- 解释器核心单独使用 `-O2`，其他 BDA 文件使用 `-Os`；
- 对 IN40—IN45 保持明确的位运算语义；
- 不使用 `fast-math`，不删除初始化、越界、除零和非法操作检查。

解释器仍接受旧版 BBASIC 字节码，增加的是端口和运行时能力，不是另造一套 CPU，也不是把 GBA/ARM 内核冒充成 BBASIC 内核。

## 6. 资源读取和低内存缓存

`src/bda_backend.c`、`src/bbvm_lib.c` 和 `sdk/ResourceBB.lib` 增加了只读分页资源接口：

- `RESREAD(PATH$, OFFSET, DEST, LENGTH)` / `OUT104`；
- `RESVERSION()` / `IN105`；
- 单次读取最多 65536 字节；
- 目标 VM 范围用减法形式检查，避免无符号加法溢出；
- 资源以 `rb` 打开，缺少资源不会被错误地创建成空文件；
- 文件、seek、短读和关闭路径都有明确错误码。

图片/地图读取使用固定小窗口而不是把整个大资源文件读入堆：最多 4 个 owner、48 个 2 KiB 窗口，总窗口约 96 KiB，并带低内存回退、写失效和结束释放。写存档时不会驱逐无关的只读 LIB/IDX 缓存。

## 7. 存档原语

`ResourceBB.lib` 同时提供：

- `SAVWRITE`：有界写入；
- `MEMCRC`：对 VM 内存做 CRC；
- `SAVCOPY`：受范围保护的内存复制；
- `SAVVERSION`：接口版本查询。

RPG 侧的双 bank/16 KiB 存档合并和 ROM CRC 表属于 BASIC 游戏代码，不硬塞进 VM C 端。这样既保留游戏原有存档格式，又避免 VM 为单一游戏增加不可复用状态。

## 8. 图形和 9688 屏幕适配

新增 `RENDERFX` / `RENDERVERSION`（`OUT106` / `IN107`）以及 `sdk/RenderBB.lib`。包为 21 个 32 位 word、84 字节，地址不要求 4 字节对齐。支持：

- RGB565 图片合成、透明度和品红色键；
- 页面区域色调；
- 不透明页面提交/复制；
- 只使用图片有效像素的闪光效果；
- 视口闪光；
- 局部旋转和缩放；
- 列波形；
- 有界地图图块批量绘制；
- 图集查找、实体筛选和稳定排序。

缩放使用整数 DDA 最近邻采样，不创建整屏缩放中间图，也不把逐像素绘制下沉到 BASIC。原生页面仍是 240×320；横屏显示由目标应用的视口裁剪完成，不把原始画面强行拉伸成错误比例。

操作 3 的不透明提交明确保留 RGB565 品红像素，避免旧的 `FLIPPAGE` 色键语义把新图层的品红误当透明。新增渲染参考算法和 GPL 许可说明见 `RENDER_FX.md` 与 `COPYING_RENDER_FX`。

## 9. 单声道音频接口

`src/bbvm_audio.c`、`src/bbvm_audio_ports.c` 和 `src/bda_audio_backend.c` 提供旧音频接口：

- `SOUNDPLAY` / OUT96：播放单个 WAV；
- `SOUNDSTOP` / OUT97；
- `SOUNDVOLUME` / OUT98；
- `SOUNDPOLL` / OUT99；
- `SOUNDTONE` / OUT100；
- `SOUNDPAUSE`、`SOUNDRESUME` / OUT101—102；
- `SOUNDPLAYRES` / OUT103：播放 BBAL 音频库成员；
- `SOUNDVERSION` / IN98，当前返回 101。

设备端统一使用 22050 Hz、单声道 PCM，支持合法 PCM8/PCM16 WAV。播放采用 1024 字节输入缓冲和两个 512 帧输出块，处理部分写、背压、有限超时、循环和排空；音频服务是协作式轮询，不创建线程，不把实时调度交给旧 BDA 的阻塞 Idle。

MP3、OGG、MIDI 不在真机端解码，应在电脑上离线转成 WAV 后再打包。`tools/convert_wav.py` 负责转为 22050 Hz mono PCM16。

## 10. BBAL 音频资源库

`tools/pack_audio.py` 生成 BBAL v1：

- 16 字节小端头：魔数、版本、成员数、精确文件长度；
- 一开始编号的固定索引，每项保存绝对偏移和成员长度；
- 成员按 4 字节对齐保存完整 RIFF/WAVE；
- 一库可以放长音乐和短音效；
- 库中不保存源路径和文件名；
- 可选 JSON 报告记录名字、ID、偏移、大小和 SHA256；
- 采用临时文件、fsync、原子替换；转换失败不会破坏旧库，也不会覆盖源 WAV。

`nobi_sound_index.h` 是当前特定 `sound.lib` 的编译期快速索引元数据，不是全库内容，也不是通用全文件校验。更换音频库后必须重新生成索引或禁用快速索引；引擎仍保留通用解析回退，不能把旧库的索引直接套给不同布局的库。

## 11. 双声道逻辑的设备化取舍

`src/bbvm_mixer.c` 提供 BGM + SE 两个逻辑通道，但只使用一个固件 PCM 端点：

- BGM 和 SE 在 C 端混合后饱和到有符号 PCM16；
- 每通道支持音量 0—256、音高 50%—200%、循环、淡入淡出；
- 一个音效不会重新打开第二个硬件 PCM 设备；
- 音频端点、PCM 队列、服务点和状态大小保持稳定；
- 游标是解码位置，不冒充声卡硬件已播放到的位置；
- 读档恢复资源 ID、采样游标、音高相位和音量，但不保存硬件 PCM 队列、淡变包络的完整现场。

`MIXCMD` / OUT108 与 `MIXVERSION` / OUT109 的包格式和新增操作见 `MIXER_API.md`。操作 7/8 专门处理原游戏的“播放一次”状态，避免用按键等待或伪造硬件时间替代原逻辑。

针对 9688 的最新批处理只对“100% 音高且已缓冲、片段至少 32 字节”的样本段启用；异音高、分数相位、极短片段、补读、循环边界和错误路径保留原采样逻辑。新增堆为 0，不改变 PCM、IO 次序、队列大小或游戏逻辑。

## 12. 编译和测试

MIPS 构建：

```powershell
$env:BBK9588_SDK_ROOT = 'D:\src\bbk9588-bda-sdk'
python tools/build_bda.py --output build/BB虚拟机-re.bda
```

工具会使用 SDK 的 MIPS32 little-endian soft-float 工具链，并将解释器核心单独按 `-O2` 编译。公开仓库不包含编译器和链接器二进制。

BBASIC 编译助手：

```powershell
$env:BBASIC_TOOL_DIR = 'D:\tools\BB2.0++\Tool'
python tools/compile_basic.py input.bas build/output.bin
```

`BBASIC_TOOL_DIR` 必须包含 `BBasic++.exe`、`Blink.exe`、`StdLib.lib` 和 `StdLib.fls`。不再硬编码某台电脑的 E 盘路径。

主机回归：

```powershell
python tests/run_all.py
# 或
.\tests\run.ps1
```

测试覆盖解释器、字符串/内存边界、音频 WAV 解析和流式行为、BBAL 索引/范围/原子失败保护、端口参数转发以及工具转换。测试使用生成的字节码和临时 WAV，不依赖游戏 BIN/LIB/SAV，也不等同于 9688 真机扬声器、FPS、触摸延迟或长时间稳定性验收。

## 13. 已知限制

- 真实设备的音频芯片/DMA 时序、扬声器输出和长时间稳定性仍需真机验收；
- 解释器主机测试通过不等于所有旧固件版本都完全兼容；
- 设备私有中文手写输入服务未公开，仓库只保留可验证的输入端口；
- `nobi_sound_index.h` 必须与对应音频库重新生成；
- 旧 BBasic++/Blink 前端没有在本仓库重写，新增接口通过 SDK 文本函数库提供；
- 硬件资源容量、文件系统可容纳大小和实际显示方向仍受具体固件限制。

## 14. 许可证

上游 Apache License 2.0、`NOTICE` 和本仓库自有源码许可证边界继续有效。渲染色调兼容代码受 GPL-3.0-or-later 约束，见 `COPYING_RENDER_FX`。游戏资源、固件、商标和外部工具链不因本仓库提交而获得授权。
