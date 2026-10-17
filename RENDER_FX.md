# RGB565事件图片接口（独立版本100）

OUT106读取VM内21个32位word的包并返回r3：0成功，-1包地址/范围，-2无效page/操作，-3参数，-4无效图片。
OUT107返回100；SDK RenderBB.lib提供RENDERFX(address)及RENDERVERSION()。
包地址不要求4字节对齐：旧编译器全局数组可能未对齐，底层通过VM有界逐字节读32位word，禁止直接转换MIPS指针解引用。
编译助手前置AudioBB、ResourceBB、RenderBB，旧SHOWPIC及其原mode行为不变。

| word | 字段 |
|---|---|
| 0 | 操作：1图片合成，2当前page区域色调，3不透明页面复制/提交，4图片透明掩码闪光，5视口闪光 |
| 1 | page，-1为可见屏幕，0..8为已分配页面 |
| 2 | 图片句柄，操作2忽略 |
| 3..6 | 目标x,y,w,h，原生240×320坐标；w/h为1..65535 |
| 7..10 | 源图片x,y,w,h，必须完整位于图片中；操作2忽略 |
| 11 | 不透明度0..256；0全透明，256全不透明 |
| 12 | 图片色键：0关闭，1使用RGB565品红色键 |
| 13..16 | RPG2000色调r,g,b,saturation，0..200，100中性 |
| 17..20 | 额外目标裁剪x,y,w,h，应用viewport裁剪，防止画到侧栏 |

操作3的word2为源page（-1可见，0..8已分配）；源和目标不能相同。
不缩放（源w/h必须等于目标w/h）、alpha必须256、色键0、色调均100。
完整拷贝包括0xf81f，避免新色调/不透明图片的品红像素被旧FLIPPAGE色键当作透明而留下上一帧。
旧FLIPPAGE/COPYPAGE色键语义保持不变；新游戏最终合成使用此明确提交操作。

先按目标和viewport/屏幕求交集，缩放后用整数DDA精确最近邻选取源像素，
色键丢弃在色调之前，最终按RGB565通道的整数alpha混合。没有缩放中间全屏缓冲或逐像素BASIC调用。
操作2原地修改指定区域；调用者必须先从未染色的底图重绘，不能把每一帧色调重复叠加。

色调参考本地官方EasyRPG Player源码：game_screen.h将百分数乘128/100，bitmap.cpp使用hard-light与Y'CbCr饱和度。
本实现计算少量所需变换，不携带65536字节hard-light大查表；中性色调/全不透明快路径保留原RGB565。
预旋转横屏280×210对应原生裁剪(x=15,y=40,w=210,h=280)。

## 来源与许可

bbvm_fx.h及相关色调适配为GPL-3.0-or-later；随修改后的BDA提供本工程源码和COPYING_RENDER_FX。
使用EasyRPG Player的GPLv3+色调算法兼容行为，版权仍归原作者，未将其声称为自研原创算法。
官方参考为 EasyRPG Player 的公开 GPL 源码；本仓库只保留与算法行为相关的归因说明，不分发本机抓取目录：
- game_screen.cpp SHA256 06c31595c1e04c34b006e3457f6421fe347133f2eb3951cfdf6ee108d89fcbe8
- bitmap.cpp SHA256 769c4fbd14504ff825463dd804a2326e53c1b33b006ffd547aa17bf8dc80a650
- game_screen.h中的GetTone与上述源码一同核对。

没有引入SDL/pixman或实时全屏旋转。固件图形/音频仍需物理机验收，不保证主机测试用时等于真机速度。

## 2026-10-16兼容新增：闪光操作4/5

旧操作1/2/3及版本100返回值不变。操作4读取与操作1相同的图片源矩形/缩放/色键，但只借图片不透明像素作为mask；操作5作用于目标裁剪区域，不读图片。
word13/14/15改为闪光RGB各0..31（绿色再转换至RGB565的0..63），word16保留且仍需0..200；word11为闪光不透明度0..256。颜色为固定RGB混合，不冒充hard-light色调，也不把整个精灵包围盒染色。
新增操作仍有完整源/目标范围、84byte包及未对齐地址校验，不分配新缓冲、不修改历史SHOWPIC。
## Picture-local transforms (compatible extension, version 100)

Operations 6/7 preserve the 84-byte packet and old operations 1..5. Both use
word3/4 as native picture centre, word9/10 as complete source dimensions.
word7 is unsigned Q8.8 phase (256 logical angle units per revolution).
Operation6 word8 is zoom percent. Operation7 word8 packs zoom percent in the
high16 and wave power Q8.8 in low16. Tone/alpha/key/viewport retain old units.
Only visible pixels are processed. Rotation uses inverse Q16 incremental
sampling; wave uses per-column sine and quotient/remainder sampling, with no
per-pixel division, no allocations and no whole-screen runtime rotation.
Official behavior references are game_pictures.cpp (effect update/Move rules),
sprite_picture.cpp (angle/power), bitmap.cpp WaverBlit (clipped-wave phase).
Native deterministic tests: tests/test_picture_transform_mips.py.
# 地图与屏幕扩展（2026-10-16）

- operation8：有界动态图块批量渲染，单包最多512条记录；记录为world x/y、atlas slot、0=AB/1=C。当前地图缓存完整ground/high两张帧图集，由两个绘制pass分别合成，不按下层类型丢弃上层。全包及所有记录先验证后写屏幕；每帧无文件IO或分配。详情src/bbvm_map_tiles.h。
- operation9：[9,page,x,y,width,height,darkness,phase,0...0]，darkness0..256、phase0..65535；固定点波形+淡变，640字节列缓冲，不动视口外辅助栏。此为低内存硬件适配效果，不宣称RPG_RT逐帧完全相同。详情src/bbvm_screen_wave.h。
- 原operation1—7的ABI和RENDERVERSION100不变。operation8/9的实际MIPS整屏像素对照在游戏工程tests/test_map_animation_mips.py、tests/test_screen_transition_mips.py。

