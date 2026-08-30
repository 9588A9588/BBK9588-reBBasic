# BBK9588-reBBasic

[![CI](https://github.com/HelloClyde/BBK9588-reBBasic/actions/workflows/ci.yml/badge.svg)](https://github.com/HelloClyde/BBK9588-reBBasic/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/HelloClyde/BBK9588-reBBasic?display_name=tag)](https://github.com/HelloClyde/BBK9588-reBBasic/releases)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

步步高 9588 `BB虚拟机.bda` 的可重新编译、功能兼容源码重建。项目使用
[BBK 9588 Native BDA SDK](https://github.com/HelloClyde/bbk9588-bda-sdk)
重新实现 BBASIC 字节码解释器、运行时端口和 9588 图形/输入/文件后端；目标是让旧版
BBASIC 程序能够继续运行，而不是让生成文件与原 BDA 字节一致。

> 独立、非官方项目，与 BBK 或设备制造商没有隶属或背书关系。仓库不包含原版
> `BB虚拟机.bda`、固件、NAND、商业游戏或第三方 `BIN`/`LIB`/`SAV` 资源。

## 自有测试截图

| 图形页、翻页与几何图元 | 触摸输入键盘 |
|---|---|
| ![BBASIC graphics smoke test](qa/emulator-smoke.png) | ![BBASIC touch keyboard](qa/input-keyboard.png) |

两张截图均来自仓库自带、可重新生成的测试程序，不依赖原机或第三方游戏素材。

## 游戏兼容性截图

| Flappy Bird | Rich / 大富翁 |
|---|---|
| ![Flappy Bird compatibility test](qa/compatibility/flappy-bird.png) | ![Rich Monopoly compatibility test](qa/compatibility/rich-monopoly.png) |

截图来自 9588 QEMU 固件中的功能回归，只用于说明兼容性。游戏名称、画面和美术素材
归各自权利人所有，不适用本仓库的 Apache-2.0 许可；游戏本体及 `BIN`/`LIB`/`SAV`
资源不随仓库或 Release 分发。

## 当前状态

- 支持带 16 字节 `BBE` 头及无头的旧版 BBASIC BIN；
- 恢复 2 MiB 小端虚拟内存、8 个寄存器、栈、分支、调用、比较与数值运算；
- 实现 `0..39` IN 端口及已知 `0..80` OUT 端口；
- 支持动态字符串、文本、9 个 RGB565 页面、LIB 图片、色键、像素和几何图元；
- 支持物理按键、触摸、毫秒计时器以及 BBASIC 文件读写；
- 支持旧游戏使用的 `9388.0f` 环境检查；
- 已在 9588 QEMU 固件中回归 Flappy Bird 和 Rich/大富翁：标题、棋盘、骰子、人物
  移动、地图滚动和连续回合正常，无重复贴图、黑块或资源错位。

第三方回归程序仅用于本地功能测试，不随本仓库或 Release 分发。

## 快速开始

要求：Windows PowerShell、Python 3.10+、Git，以及能运行 `gcc` 或 `clang` 的主机
C 编译器。MIPS 工具链由 SDK 安装脚本下载并校验 SHA-256。

```powershell
git clone https://github.com/HelloClyde/bbk9588-bda-sdk.git
cd bbk9588-bda-sdk

# reBBasic 需要位于 SDK 根目录下的 re-bbasic 目录
git clone https://github.com/HelloClyde/BBK9588-reBBasic.git re-bbasic

python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -e .
.\scripts\setup_toolchain.ps1

.\re-bbasic\tests\run.ps1
.\re-bbasic\build.ps1
python -m bda_packer.validate .\re-bbasic\build\BB虚拟机-re.bda
```

构建产物位于：

```text
re-bbasic/build/BB虚拟机-re.bda
```

如果仓库不位于 SDK 子目录，可在构建前指定 SDK 源码根目录：

```powershell
$env:BBK9588_SDK_ROOT = "D:\src\bbk9588-bda-sdk"
.\build.ps1
```

不想本地编译时，也可以从
[GitHub Releases](https://github.com/HelloClyde/BBK9588-reBBasic/releases)
下载 CI 构建并校验过的 `BBK9588-reBBasic.bda` 和 `SHA256SUMS.txt`。

## 运行 BBASIC 程序

把你有权使用的程序及其资源放在设备或模拟器的：

```text
A:\BBasic\
```

例如某程序使用 `GAME.bin`、`GAME.LIB`、`GAME.SAV`，三个文件应放在同一目录。
启动 BDA 后，从文件选择器选择 `GAME.bin`。

使用 SDK 配套的 8013 模拟器部署脚本：

```powershell
$env:BBK9588_EMULATOR_ROOT = "E:\bbk9588-emulator-v0.1.5"
.\scripts\test_bda_in_emulator.ps1 `
  .\re-bbasic\build\BB虚拟机-re.bda -ResetImage
```

部署脚本只修改专用测试 NAND 副本，不修改原始 NAND。

## 构建与发布

每次推送到 `main` 或创建 Pull Request，CI 会：

1. 检出公开 9588 SDK；
2. 运行主机端解释器及 LIB 格式回归测试；
3. 使用 MIPS little-endian soft-float 工具链编译 BDA；
4. 校验 BDA header、入口、图标、长度和 checksum；
5. 上传本次构建的 BDA artifact。

推送 `v*` 标签会自动创建 GitHub Release：

```powershell
git tag v0.1.0
git push origin v0.1.0
```

Release 工作流会重新测试和构建，并发布：

```text
BBK9588-reBBasic.bda
SHA256SUMS.txt
```

## 项目结构

```text
src/bbvm_core.c       指令解码、寻址、寄存器、栈、比较和运算
src/bbvm_runtime.c    字符串、转换、数学和 IN 端口
src/bbvm_lib.c        1 起始 LIB 索引和图片记录边界检查
src/bda_backend.c     9588 GUI、文件、输入、资源和 OUT 端口
src/main.c            文件选择、装载、窗口与 VM 生命周期
tools/build_bda.py    freestanding 编译、链接和 BDA 打包
tests/                主机回归测试及原创 BBASIC 字节码夹具
docs/                 逆向与兼容性记录
qa/                   原创固件级测试截图
```

## 已知边界

- 三角函数使用 freestanding 多项式/牛顿近似，不保证逐位等同原运行库；
- 最多同时存在 256 个动态字符串；
- 原机私有中文手写输入服务没有公开 SDK，本实现提供 ASCII 触摸键盘；
- 字体编号保持 BBASIC 字符格尺寸，具体字形由当前固件字体对象提供；
- 当前图形后端使用固定的 240×320 RGB565 页面；
- 模拟器验证不自动等同于所有 9588 真机和固件版本。

更详细的指令、端口和 LIB 格式证据见
[逆向记录](docs/reverse-notes.md)。提交兼容性修复前请阅读
[CONTRIBUTING.md](CONTRIBUTING.md)。

## License 与数据边界

本项目中有权授权的原创源码、文档和测试使用
[Apache License 2.0](LICENSE)。该许可证不覆盖固件、NAND、原机应用、第三方游戏、
资源、商标或外部工具链；详见 [NOTICE](NOTICE) 和
[DATA_NOTICE.md](DATA_NOTICE.md)。
