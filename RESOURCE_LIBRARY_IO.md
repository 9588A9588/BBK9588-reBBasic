# 只读分页资源接口

本扩展独立于音频LIB。BBAL播放仍用SOUNDPLAYRES，不需要先RESREAD整段PCM。

- SDK：sdk/ResourceBB.lib，由tools/compile_basic.py在用户源码之前加入。
- RESVERSION()返回100；音频接口SOUNDVERSION()仍返回101。
- RESREAD(PATH$,OFFSET,DEST,LENGTH)：从文件字节偏移OFFSET读LENGTH字节到VM地址DEST。
- OUT104：r3路径句柄、r2偏移、r1目标、r0长度，r3返回完整字节数或负错误码。OUT105查询版本。
- 每次最大65536字节，文件seek偏移不超过0x7fffffff；VM范围以减法校验，防止溢出。
- 只以rb打开，缺失资源不会像旧OPEN的rb+→wb+回退那样新建空文件。成功打开后所有路径关闭文件。

| 返回值 | 含义 |
|---|---|
| >=0 | 完整读取长度；合法零长度直接返回0，不打开文件 |
| -1 | 空路径或文件打开失败 |
| -2 | 路径超限、目标VM范围、读取长度或偏移参数非法 |
| -3 | seek失败或文件范围不足 |
| -4 | 读取短于要求；目标前缀可能已改写，调用者必须弃用该buffer |

旧编译器的自定义函数嵌套实参存在计数错误，应先存临时变量：

~~~basic
DIM BUF(256)
FUNCTION BUFADDR()
    VASM("LD int [vint_BUFADDR],vint_buf")
END FUNCTION
DIM R,BP
BP=BUFADDR()
R=RESREAD("A:/BBasic/data.dat",0,BP,1024)
IF R<>1024 THEN END
END
~~~

全局整型数组是连续VM内存，每项4字节；这个示例不修改原编译器。
测试见tests/test_resource_mips.py，覆盖真BB2.0++编译/MIPS的精确读取、错误/短读及64KiB完整块与数组尾部保护。
固件文件服务为桩，尚无物理设备的读取时延验收。
