# 固件级 QA

`emulator-smoke.png` 是最终 `BB虚拟机-re.bda` 在 9588 QEMU 固件中运行自有
`tests/fixtures/smoke.bin` 的截图。它同时证明：

- BDA 能从固件菜单启动并进入文件选择器；
- 裸 BB BIN 能装载和执行；
- compatible context、RGB565 快速填充、线、圆、翻页和文本输出可用；
- 240×320 帧缓冲方向及 BB 的 BGR 颜色解释正确。

`input-keyboard.png` 是 OUT 11/12 使用的 ASCII 触摸键盘。它支持数字、大小写
英文字母、常用文件/数字符号、空格、退格和确认；物理方向键/确认键可作为备用输入。
