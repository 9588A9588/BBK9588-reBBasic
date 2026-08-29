# BB虚拟机逆向记录

## 样本与装载

原始样本：`应用/程序/BB虚拟机.bda`

```text
size             315964 bytes
sha256           6af76b4d38354dac3528f84d6651d450209cb2046cc32a977b8d0da2d3b9edb5
entry file off   0x95f8
entry VA         0x81c00020
runtime base     0x81bf6a28
category         4
```

入口保存固件 API 表后进入自己的窗口和 VM 初始化。原程序使用的表地址为：

```text
RES  0x81c43c60
GUI  0x81c43c64
SYS  0x81c43c68
FS   0x81c43c6c
MEM  0x81c43c70
```

静态调用统计为 275 次运行时调用、69 个唯一偏移，其中 GUI 200 次、FS 43 次、
MEM 18 次。重建后端只依赖当前 SDK 已确认的表项；取像素使用原 VM 同样调用过的
`GUI+0x374` 六参数形式。

## BB 程序格式

装载器先读 16 字节。如果前三字节为 `BBE`，跳过这 16 字节；否则从文件开头装载，
因此兼容旧版裸 BIN。有效负载复制到 2 MiB VM 内存的地址 0。

复位寄存器：

```text
rp = 0                 指令指针
rf = 0                 比较标志
rb = payload_size      静态区尾/栈基准
rs = rb + 1000         栈顶；PUSH 后 rs -= 4
r0..r3 = 0             通用寄存器
```

寄存器编号是 `rp/rf/rs/rb/r0/r1/r2/r3 = 0..7`。四种寻址方式为寄存器、寄存器
间接、立即数和绝对内存；五种数据类型为 dword、word、byte、float 和 int，长度
分别为 4、2、1、4、4。

## 指令编码

高半字节是指令，恢复出的长度表为：

```text
opcode  0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
length  1 10  5  5 10 10  5  6  5  1 10 10  0  0  0  1
name    N LD PS PP IN OT JP JC CL RT CP CA          EXIT
```

10 字节指令：

```text
byte 0      opcode:4 | datatype:4
byte 1      calop:4  | first_mode:2 | second_mode:2
byte 2..5   first argument, little endian
byte 6..9   second argument, little endian
```

5 字节指令把寻址方式放在 byte 0 的低半字节，参数位于 byte 1..4。`JPC` 是 6
字节：低半字节为条件，byte 1 为寻址方式，byte 2..5 为目标。

比较标志：`Z=1, B=2, BE=3, A=4, AE=5, NZ=6`。`CAL` 子操作为
`ADD=0, SUB=1, MUL=2, DIV=3, MOD=4`；float 不支持 MOD。

## 字符串

动态字符串句柄是 `-index-1`，因此所有有效句柄都按有符号数表现为负数。非负字符串
值是 VM 内存地址。原 BDA 的修订字符串显示它修复过 `LEFT$` 和 `INSTR`；重建版按
修订后语义实现，`INSTR` 找不到时返回 `-1`。

## IN 端口

```text
 0 float->int       1 int->float       2 new string       3 string->int
 4 int->string      5 copy string      6 append string    7 strlen
 8 free string      9 strcmp          10 float->string   11 string->float
12 char at         13 set char        14 constant 65536  15 tick in ms
16 sin             17 cos             18 tan             19 sqrt
20 integer abs     21 float abs       22 set DATA ptr    23 read VM dword
24 write VM dword  25 environment     32 int->string     33 atoi
34 first char      35 LEFT$           36 RIGHT$          37 MID$
38 INSTR           39 length
```

环境端口 25 返回 IEEE-754 `9388.0f` 的位模式 `0x4612b000`。这里返回的是老版
BBASIC 运行环境兼容标识，不是当前设备型号；《大富翁》的启动代码会检查该值，
返回 `9588.0f` 会直接显示“出错码 1002”。主机测试对这个精确位模式做了回归断言。

## OUT 端口

```text
 0 print int+NL     1 print str+NL      2 print str         3 print int
 4 print char       5 print float      10 input int        11 input string
12 input float     13 READ int         14 READ string      15 READ float
16 SETLCD          17 CREATEPAGE       18 DELETEPAGE       19 LOADRES
20 SHOWPIC         21 FLIPPAGE         22 BITBLTPAGE       23 FILLPAGE
24 PIXEL           25 READPIXEL        26 FREERES          27 DELAY
32 RANDOMIZE       33 RND              34 KEYPRESS         35 CLS
36 LOCATE          37 COLOR            38 FONT             39 WAITKEY
40 PIC width       41 PIC height       42 PIXLOCATE        43 partial page copy
44 text mode       45 INKEY$           46 INKEY            48 OPEN
49 CLOSE           50 GET              51 PUT              52 EOF
53 LOF             54 LOC              55 SEEK             64 SETPEN
65 SETBRUSH        66 MOVETO           67 LINETO           68 RECTANGLE
69 CIRCLE          80 extended page copy
```

文件端口用 `r1` 选择 0..15 的 BB 文件槽。GET/PUT 的相对位移编码是
`signed(r2 - 0x7fffffff)`；类型 16/17/18 分别为 int、float、零结尾字符串。

BB 键值沿用 PC 风格：左/上/右/下为 37/38/39/40，Escape 为 27，Enter 为 13。
触摸返回 `0x80000000 | lparam`，其中 low16=x、high16=y。

FONT 编号按字号分组而不是按字体分组：`0..2` 为 12 像素，`3..5` 为 16 像素，
`6..8` 为 24 像素；每组内部选择宋体/楷体/黑体。这个顺序会同时影响 LOCATE 的
字符格换算，弄反后《大富翁》的菜单文字会叠画，并在换行时错误清空游戏背景。

## 页和资源参数

页面 `-1` 是可见屏幕；离屏页由 CREATEPAGE 返回。颜色是 BBASIC 使用的
`0xBBGGRR`，资源像素是 RGB565。

`.lib` 文件：

```text
u32 count
u32 offsets[count]

record at offsets[i]:
  u32 record_length       不包含本字段自身
  u16 width
  u16 height
  u32 reserved
  u16 pixels[width*height]
```

SHOWPIC 的参数块从低到高为：

```text
+0 mode, +4 source_y, +8 source_x, +12 height, +16 width,
+20 destination_y, +24 destination_x, +28 picture, +32 page
```

FILLPAGE 为 `color,height,width,y,x,page`；PIXEL 为 `color,y,x,page`；
SETPEN 为 `color,width,style,page`；RECTANGLE 为 `bottom,right,top,left,page`；
CIRCLE 为 `radius,y,x,page`。

扩展页复制（端口 80）的参数块从低到高为
`source_page,destination_page,source_y,source_x,height,width,destination_y,destination_x`。
源坐标和目标坐标不能互换；《大富翁》用它恢复移动人物的旧区域并拼装棋盘，互换后会
留下人物残影，且棋盘资源落在错误位置。

LIB 的资源号是 1 起始，不是 0 起始。文件开头是 `count`，第 `n` 个资源的 offset
位于 `4 + (n - 1) * 4`。记录 offset 处的长度字段之后依次为 `u16 width`、
`u16 height`、两个保留 `u32`，RGB565 像素从记录 offset `+16` 开始；长度字段的值
等于 12 字节元数据加像素字节数。旧实现把资源号当成 0 起始且只跳过一个保留字段，
因此每次 LOADPIC 都会错取下一张图，并把第二个保留字段解释成图片开头，直接造成
《大富翁》错图、重复场景和黑块。

## 原包内可见版本线索

原 BDA 的字符串记录了这些修订：毫秒 tick、从文件读写字符串、`LEFT$`、
`freeres`、`INSTR` 找不到返回 `-1`、位运算和版本信息。这些线索用于在多个可能
实现之间选择修订后的行为。

## 交叉验证与独立性

恢复过程以本仓库原 BDA、MIPS 反汇编、SDK API 表和固件动态测试为主。公开的
`0ximst1ll/bbasic` 项目只用于交叉核对指令布局和老程序样本行为；该仓库未提供明确
许可证，因此本目录没有复制其 C++ 实现，也没有包含它的 BIN/LIB 资源。

最终验证：

- 主机端指令/分支/栈/字符串测试通过；
- BDA header、入口、文件长度、校验和、四组 VX 图标静态校验通过；
- 自有 smoke.bin 在固件中完成页填充、线、圆、翻页和字符串输出；
- 自有 input.bin 显示触摸 ASCII 键盘并接收触摸；
- 公开 Flappy Bird 程序成功加载 451168 字节 LIB，显示资源图、运行游戏并响应确认键；
- 《大富翁》成功加载 `rich.bin`、`RICH.LIB` 和 `RICH.SAV`，通过机型检查；修正
  LIB 1 起始索引和 16 字节记录头后，模拟器中实际显示完整标题、棋盘、骰子、人物、
  地图滚动、日期/现金变化及多角色连续回合；先前基于错乱画面作出的“买地画面通过”
  记录不作为有效视觉验收证据；
- 最终模拟器状态 `crash=null`、`invalid=[]`，随后安全关机。
