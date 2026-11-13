# UEFI-DOS

[English](README.md) | **简体中文**

**用 UEFI 重写的 DOS。** 一个可在 x86_64 与 IA32 固件上以 UEFI 应用形态启动的小型
DOS 兼容运行环境：自己挂载 FAT32、自带类 `COMMAND.COM` 的命令解释器，并通过内置
8086 解释器运行 16 位实模式 DOS 程序 —— 一套"类似 V86"的软件兼容层，**不依赖 BIOS，
也不依赖硬件虚拟化**。

它就是一个 `.EFI` 文件，而 DOS 程序看到的是 **MS-DOS 4.0**
（`INT 21h AH=30h` 返回 `AX=0x0004`）；在此基础上补上 DOS 从未有过的东西：
FAT32、64 位 UEFI 启动、FAT16 大容量卷、原生 32/64 位 DOS API，以及 386 保护模式
与 DPMI 0.9 宿主。

当前版本 **1.1.8** —— 34 项自动化检查全部通过（Windows 宿主测试器，以及 QEMU + OVMF
的 x86_64 与 IA32 实机启动）。

1.1.1 修的是 DOS 的 EXEC 命令行约定：调用方 `PSP:0080h..00FFh` 那 **128 字节
原样**复制给子进程（`WIN.COM` 的 `'XW'` 标记就藏在这里），且计数字节不含结尾 CR。
这两处对上之后，**Windows 1.01 能在 CGA 640×200 下把启动画面画出来**。

1.1.0 补的是「实模式 GUI 需要的硬件」：`INT 10h` 现在支持 **CGA 模式 4/5/6**
（`B800` 的奇偶场交错位图，`AH=0Bh` 与 `3D8h`/`3D9h` 端口选择调色板），有了完整的
**`INT 33h` 鼠标驱动**（底层是固件的 `EFI_SIMPLE_POINTER_PROTOCOL`，指针由模拟器画进
帧缓冲），并且模拟器终于会**注入硬件中断** —— 18.2 Hz 的 `INT 8`，以及带 `60h` 端口
扫描码锁存的 `INT 9`：实模式 Windows 自己接管键盘与定时器中断，根本不读 BIOS 键盘缓冲。
`HLT` 变成「空转一步」，所以 `sti; hlt` 消息循环能正常等待；`0040:006C` 的 BIOS 计时
计数器也开始走。

为此还加了一个 DOS 层特性：**`SETVER`**（按程序设置 DOS 版本）。UEFI-DOS 报告的是
MS-DOS 4.00，而 Windows 1.x 遇到高于 3.x 的版本就拒绝启动，所以
`SETVER WIN.COM 3.30`（命令行或 `CONFIG.SYS`）是逃生口，而不是把所有人的版本降下去。
有了它，**Windows 1.01 的 `SETUP` 能装完整个发行版**；`WIN100.BIN` 现在能走完它对 DOS
内部结构的整套校验（SFT 链、SDA、InDOS 标志、PSP、作业文件表），随后在加载驱动之前以退出码 1
结束，所以桌面还没画出来——校验链的逐条反汇编、它需要的 DOS 表面，以及卡住的那个
实例/句柄遍历（`CS:0E71h`）都记在
[`docs/windows1x.md`](docs/windows1x.md)。最新一轮把这堵墙量到了字节：内核第一次
`GlobalAlloc` 要 **28 段（435 字节）** 建模块数据库，而它自己的内存场只给了
**27 段（可用 428 字节）**，差两个字节；在临时镜像里把这个请求改小 4 字节，
分配就成功（`CS:212Eh` 返回真实段而不是 `ES=0001h`），说明卡的是尺寸不是代码路径，
而下一道门 `CS:0E71h` 仍然返回「0 个模块」。

## 立刻跑起来（不需要编译器）

[**下载 `UEFI-DOS-1.1.9.zip`**](https://github.com/lzyAA5A/uefi-dos/releases/latest/download/UEFI-DOS-1.1.9.zip)
（240 KB）——里面是可直接启动的 64 MB 镜像加一个启动器：

1. 先装 Windows 版 QEMU（它自带 OVMF 固件）：
   `winget install SoftwareFreedomConservancy.QEMU`
2. 解压后双击 **`start.bat`** —— UEFI-DOS 直接启动，跑完演示程序后停在 `C:\>` 提示符。

同一个镜像也可以写到 U 盘（Rufus 选 *DD 镜像* 模式，或 Linux 下 `dd`），
在真实 UEFI 机器上启动；镜像里同时有 `EFI/BOOT/BOOTX64.EFI` 与 `BOOTIA32.EFI`。
`make_release.ps1` 可以从源码重新生成这个发布包。

## 功能状态

| 领域 | 状态 |
| --- | --- |
| UEFI 启动（x86_64） | 可用 — PE32+ `BOOTX64.EFI`，从磁盘启动项自动进入 |
| UEFI 启动（IA32） | 可用 — PE32 `BOOTIA32.EFI`，同一镜像同时携带两个文件 |
| 启动卷定位 | LoadedImage → `DeviceHandle` → `BlockIo` → MBR 分区（兼容无分区超级软盘） |
| FAT32 | 读 + 写（建/删/改名/建目录/删目录，簇分配与回收） |
| FAT12 / FAT16 | 读 + 写 |
| 软盘/磁盘镜像挂载 | `MOUNT A: IMAGE.IMG` 把 `C:` 上的镜像文件变成可读写驱动器 |
| 命令解释器 | `DIR CD MD RD DEL REN TYPE COPY CLS ECHO VER VOL DATE TIME PROMPT PATH SET MEM HELP APITEST MOUNT UNMOUNT CONFIG FCB DRIVES EXIT SHUTDOWN`、批处理、`AUTOEXEC.BAT` |
| DOS 内核 | 8.3 文件名（+ VFAT 长文件名）、按驱动器维护当前目录、DOS 错误码、**DOS 4.0 语义**（见下） |
| DOS 内部结构 | `AH=52h` 列表的列表里带**可遍历的 SFT 链**、`AH=5D06h`/`5D0Bh` **可换出数据区（SDA）**、`AH=34h` InDOS 标志指针、`AH=50h`/`51h` 当前 PSP，以及 `PSP:18h` 的**作业文件表**——Windows 1.x/2.x、DESQview 这类多任务环境按任务换出的正是这些结构 |
| 8086 模拟器 | 8086/80186 实模式解释器、16 MiB 模拟内存、PSP、IVT、带重定位的 `.COM` 与 MZ `.EXE` 加载器、8087 转义按 8086 空转处理 |
| INT 21h | 控制台、文件 I/O、查找首/次 + DTA、目录、日期时间、版本（4.00）、中断向量读写、IOCTL、句柄复制、PSP、内存桩、**EXEC（AH=4Bh）**、**AH=67h**、**AH=5Ch** 字节锁、**SHARE 模式**、**DOS 4.0 面：32h 52h 59h 5Ah 60h 63h 68h 6Ch** |
| FCB 调用族 | AH=0Fh 10h 11h 12h 13h 14h 15h 16h 17h 21h 22h 23h 27h 28h（`src/emu/fcb.c`） |
| INT 10h / 16h / 1Ah / 13h | 文本服务（置模式、光标、读写字符+属性、电传、窗口滚动）作用于模拟屏幕、支持扩展键的阻塞键盘、BIOS 计时、磁盘读/写/几何参数 |
| 显示（文本） | 模拟内存里的 80×25 彩色文本屏（`B800:0000`）：直接写显存和 `INT 10h` 输出都落在同一块屏上，再镜像到控制台（只重画变化的行） |
| 显示（图形） | `INT 10h AL=13h`（320×200×256）与 VESA VBE `4F00/4F01/4F02/4F03/4F05`（640×400、640×480、800×600，8 bpp）；调色板走 `INT 10h AX=1000h/1012h/1015h` 与端口 `3C7/3C8/3C9`；画面转成 GOP 像素格式后直接写固件帧缓冲，并按整数倍放大 —— 见 [`docs/graphics.md`](docs/graphics.md) |
| 盘符 | 固件发布的每个卷都会被挂载：启动卷 → `C:`，其余依次 `D:`、`E:`…（`DRIVES` 列出，只读介质会标注） |
| 长文件名 | 打开、`DIR` 与原生 API 都会解析 VFAT LFN 链；用长名建文件会写入 LFN 链并生成 8.3 别名，DOS 程序依旧只见别名 |
| 原生 32/64 位 API | `g_dosapi` 服务表 —— 操作与错误码与 INT 21h 完全一致，原生指针，同一份源码产出两种位宽的固件 |
| 16 位桥 | `exec16()` 下行调用（原生 → 实模式）与 `INT 2Fh AX=5544h` 上行调用（实模式 → 原生） |
| CGA 图形 | `INT 10h` 模式 4/5/6（320×200×4、640×200×2），`B800` 交错位图，`AH=0Bh` 与 `3D8h`/`3D9h` 端口选调色板与背景色 |
| 鼠标 | `INT 33h` 功能 0000h–0024h、事件回调与掩码（000Ch）、轮询固件指针，并把箭头画进帧缓冲 |
| 硬件中断 | 18.2 Hz 的 `INT 8` 与 `INT 9`（扫描码锁存到 `60h` 端口），只投递给客户自己装的处理程序；`0040:006C` 计时器在走 |
| 按程序版本覆盖 | `SETVER prog x.yy`（命令行）与 `SETVER=`（`CONFIG.SYS`）单独改变某个程序读到的 `INT 21h AH=30h` |
| 386 保护模式 | 实模式 → 保护模式切换、GDT/LDT 解析、带限长检查的隐藏段缓存、`MOV CRn`/`LGDT`/`LIDT`/`LLDT`/`CLTS`、32 位寻址（SIB）、`0F xx` 指令族、保护模式 IDT 分发 |
| DPMI 0.9 宿主 | `INT 2Fh AX=1687h` 引导、`INT 31h` 描述符/DOS 内存/实模式反射 —— 见 [`docs/protected-mode.md`](docs/protected-mode.md) |
| 内置工具 | `INFO CAL SUM HEX CLOCK` —— 由 `tools/make_tools.py` 生成的 16 位 `.COM` 程序 |
| 原生程序（SDK） | `RUN64 file.exe` 把自由式 PE32+/PE32 镜像映射进可执行内存、做基址重定位，然后在真 CPU 上调用 —— 见 [`docs/sdk-guide.md`](docs/sdk-guide.md) |
| 解释器测试 | `hosttest.exe --itest`：49 条字节流指令用例 —— 见 [`docs/itest.md`](docs/itest.md) |
| 构建方式 | PowerShell 脚本（Windows）、`make`（Linux/macOS/WSL）与 `Dockerfile` |

## 内置工具

五个小工具由 [`tools/make_tools.py`](tools/make_tools.py) 生成并打进 `build/UEFIDOS.img`。
它们都是为本项目从零手写的（MIT，与本仓库其余部分一致），所以镜像自带工具、
不含任何第三方二进制；而且它们走的是真正的 `INT 21h` 服务，而不是命令解释器自己的代码：

| 程序 | 作用 | 用到的 DOS 服务 |
| --- | --- | --- |
| `INFO` | 系统面板：DOS 版本、日期、时间、当前驱动器、当前目录、原生 API 版本与指针位宽 | `AH=30h, 2Ah, 2Ch, 19h, 47h`、`INT 2Fh AX=5544h` |
| `CAL` | 当前月份的日历（首日星期与闰年都正确） | `AH=2Ah`（星期）、控制台输出 |
| `SUM` | 命令行指定文件的字节数与 16 位校验和 | `AH=3Dh 42h 3Fh 3Eh`（打开/lseek/读/关） |
| `HEX` | 十六进制 + ASCII 转储，每行 16 字节 | 同一套句柄路径、命令行尾解析 |
| `CLOCK` | 实时 `HH:MM:SS` 时钟，按任意键退出 | `AH=2Ch, 0Bh, 08h`（取时间、查键、读键） |

```
C:\> INFO
=== UEFI-DOS system info ===
DOS version : 4.00
Date        : 2026-11-08
Time        : 13:58:09
Drive       : C:
Directory   : C:\
Native API  : 01/64-bit pointers

C:\> CAL

      November  2026
Su Mo Tu We Th Fr Sa
  1   2   3   4   5   6   7
  8   9  10  11  12  13  14
 15  16  17  18  19  20  21
 22  23  24  25  26  27  28
 29  30

C:\> SUM HELLO.TXT
HELLO.TXT: 15 bytes, checksum 0471
```

`CLOCK` 是交互式的，所以不放进 `AUTOEXEC.BAT`：在提示符下输入 `CLOCK`，
按任意键即回到命令行。

## 原生 32/64 位 API 与 16 位桥

同一套 DOS 服务有三条入口，语义完全一致：

| 调用方 | 入口 | 说明 |
| --- | --- | --- |
| 16 位 DOS 程序 | `INT 21h` | `src/emu/int21.c`，返回 DOS 可见错误码 |
| 原生 64 位代码 | `g_dosapi`（`src/api/dosapi.h`） | 同样的函数，原生指针与句柄 |
| 原生 32 位代码 | `g_dosapi`，同一张表 | `BOOTIA32.EFI` 链接同一份源码 |

`APITEST`（命令解释器内建命令，即原生代码调用自己的 API）端到端验证这条路径：

```
C:\> APITEST
APITEST: DOS version 4.00, native pointer size 8
APITEST: size 16, read 16 bytes: Hello UEFI-DOS!
APITEST: wrote 13 bytes to TMPAPI.TXT
APITEST: find '*.TXT' -> 1 file(s)
APITEST: cwd C:\
Hello from 8086 code!
APITEST: exec16 HELLO.COM -> 0 (16-bit program ran)
API-OK
```

服务表覆盖 INT 21h 的 AH=30h/3Ch/3Dh/3Eh/3Fh/40h/42h/41h/39h/3Ah/43h/3Bh/47h/
19h/0Eh/4Eh/4Fh/2Ah/2Ch 以及 INT 1Ah AH=00h，返回 DOS 错误码（0 成功、2 文件未找到、
3 路径未找到、5 拒绝访问、0x0B 格式错、0x27 磁盘满 …）。句柄是 `u16`，打开模式与
seek 的 whence 与 INT 21h 相同，`find_first`/`find_next` 回填的字段与 DTA 一致。

- **从 64/32 位落回 16 位**（`exec16`）：加载实模式 `.COM`/`.EXE`，在模拟 8086 上跑到
  结束并返回退出码；调用方状态被快照并在返回时恢复，因此原生代码可以把 DOS 程序当
  子程序用。
- **从 16 位升到原生**：兼容层把 `INT 2Fh AX=5544h` 路由进 `src/emu/int2f.c` ——
  `DX=1` 返回 `AX=0100h` 与 `BX` = 原生指针宽度（4 或 8），`DX=2` 打印原生横幅，
  `DX=3` 取 RTC 到 `ES:BX`，其余返回 `AX=FFFFh`。
- 外部 EFI 应用可在运行时获取服务表：镜像以 `UEFIDOS_API_GUID`
  （`55454449-2D44-4F53-4150-4900-000000000001`）安装一个 UEFI 配置表项，值为
  `dosapi_exports { version, pointer_size, services }` 记录。

## DOS 4.0 实模式语义

细节见 [`docs/dos4-semantics.md`](docs/dos4-semantics.md)，16 位代码的迁移坑见
[`docs/migration.md`](docs/migration.md)。

| 项 | 实现 |
| --- | --- |
| SHARE 兼容模式 | 每个句柄带 `share_mode` / `open_mode` / `psp` / `locks[8]`；DENY_RW/WRITE/READ/NONE 双向判定，`compat` 只在跨 PSP 时冲突 |
| `FILES=` | 默认 8（0..4 是设备，实际 3 个空闲）；`CONFIG.SYS` 的 `FILES=`/`FCBS=`/`LASTDRIVE=` 生效；超限报 DOS 错误 4；可用 `AH=67h` 修改 |
| FCB 调用族 | 无状态 FCB 层（每次调用重解析 8.3 名），14 个 AH 入口，记录缓冲区是 DTA |
| 字节锁 | `AH=5Ch AL=0/1`，重叠检测在句柄表内完成，冲突报错误 5 |
| FAT16 大卷 | 分配游标 + 空闲簇计数；`tools/mkfat16.py` 生成 32 MB / 1.9 GB 稀疏测试卷 |
| 文件大小 | 保持 `u32`，按 DOS 4.0 语义以 32 MB 为实测上限（已在 1.9 GB FAT16 卷上验证） |
| 报告版本 | `AH=30h` 返回 `AX=0x0004`（DOS 4.00），`VER` 显示同一级别；想报告别的版本改 `src/uefidos_config.h` 里的 `UEFIDOS_DOSVER_*` |
| 数据结构 | `AH=32h` 在保留 ROM 窗口里为每个已挂载盘生成一个 DPB；`AH=52h` 返回 list of lists，其 `+2` 指向该 DPB 链 |
| EXE 映像装载位置 | `CONFIG.SYS` 写 `EXECHIGH=ON` 时，EXE 映像贴着程序块的**顶端**装载（不再是 PSP 之后的默认位置）；`CONFIG` 显示当前模式，宿主测试器用 `UEFIDOS_EXEC_HIGH_IMAGE` |
| 错误上报 | `AH=59h` 把上次失败映射成 class / 建议动作 / locus 三元组（`2`→class 8、`5`/`21h`→class 3、`27h`→class 11、`50h`→class 12 …） |
| 扩展打开 | `AH=6Ch` 支持 DOS 的 action 位：打开已有 / 不存在则创建 / 覆盖已有，`AX` 返回句柄、`CX` 返回 `1`/`2`/`3`；`AH=68h` 提交 |
| 临时文件 | `AH=5Ah` 在指定目录下生成 `TMPxxxxx.TMP` 并带冲突重试，两个程序不会撞名 |
| truename / DBCS | `AH=60h` 规范化成 `C:\DIR\FILE.EXT`；`AH=63h` 返回 `AL=0`（单字节代码页） |

双路径一致性（`--phase46`）把同一操作分别走 `INT 21h` 与 `g_dosapi`，逐项断言：
句柄有效、字节数相同、seek 位置相同、读回内容逐字节相同、错误码相同
（文件不存在 → 2，删除只读文件 → 5）。

## 保护模式与 DPMI

16 位程序可以掉进 386 保护模式跑平坦 32 位代码，32 位客户也能通过内置 DPMI 宿主
引导。回归测试断言 `DPMITEST.COM` 会走完整条路：

```
DPMI-HOST-OK       INT 2Fh AX=1687h 找到 DPMI 宿主
PM16-OK            宿主入口把我们切到 16 位保护模式
PM32-DOS-OK        远跳转到平坦 32 位选择子，从 32 位代码调用 INT 21h AH=9
REFLECTED-21H-OK   经 INT 31h AX=0300h 反射到的真实实模式 INT 21h 打印
RMREFLECT-OK       反射返回保护模式
DOSMEM-OK          INT 31h AX=0501h 分配 DOS 内存成功
```

尚未实现的部分（ring 3、分页、完整 DPMI）与分阶段计划见
[`docs/protected-mode.md`](docs/protected-mode.md)。

## 构建与运行

### Linux / macOS / WSL：`make`

只要有一个能产出 PE 的 GCC（Linux/macOS 上的 mingw-w64），整个系统就能构建，
WSL 里同样适用：

```sh
sudo apt install build-essential gcc-mingw-w64-x86-64 gcc-mingw-w64-i686 python3 make

make            # build/BOOTX64.EFI
make all        # 再加上 build/BOOTIA32.EFI
make image      # 再加上 build/UEFIDOS.img（可启动，AUTOEXEC 末尾停在 C:\> 提示符）
make sdk        # 再加上 build/HELLO64.EXE、build/HELLO32.EXE（原生 32/64 位程序）
make run        # 用 QEMU 启动镜像（设 OVMF_DIR=/usr/share/OVMF）
make clean
```

`make` 不需要 Visual Studio、不需要 PowerShell、也不需要任何 Python 包 —— 只要交叉
编译器、`make` 与 `python3`（标准库）。`make host` / `make test`（宿主测试器与指令级
测试向量）只在 Windows 上可用，因为宿主平台层直接调用 Win32 控制台与文件 API。

### 或者完全在 Docker 里构建

```sh
docker build -t uefi-dos .
docker run --rm -v "$PWD/out:/src/build" uefi-dos        # BOOTX64/IA32 + 镜像 + SDK
docker run --rm -v "$PWD/out:/src/build" uefi-dos make sdk
```

镜像里安装 `gcc-mingw-w64`，全部从源码构建，构建过程本身不下载任何二进制。

### Windows：PowerShell

依赖：

- Windows + MSYS2 `mingw64` gcc —— 一个 x86_64 工具链同时产出 64 位与 32 位 UEFI 程序
  （项目不用 libc，64 位除法助手在 `src/common/divdi3.c`）
- Python 3（镜像生成与手汇编测试程序，仅用标准库）
- QEMU 与 OVMF/edk2 固件（用于启动测试）

依赖：

- Windows + MSYS2 `mingw64` gcc —— 一个 x86_64 工具链同时产出 64 位与 32 位 UEFI 程序
  （项目不用 libc，64 位除法助手在 `src/common/divdi3.c`）
- Python 3（镜像生成与手汇编测试程序，仅用标准库）
- QEMU 与 OVMF/edk2 固件（用于启动测试）

```powershell
# 构建 BOOTX64.EFI、BOOTIA32.EFI、hosttest.exe 与 build/UEFIDOS.img
powershell -ExecutionPolicy Bypass -File build.ps1

# 在 QEMU + OVMF x86_64 上启动（串口日志写到 build\serial.log）
powershell -ExecutionPolicy Bypass -File run_uefi.ps1

# 用 32 位 OVMF 固件启动同一镜像
powershell -ExecutionPolicy Bypass -File run_uefi.ps1 -Firmware ia32

# 全量回归（宿主测试 + 两种固件启动）
powershell -ExecutionPolicy Bypass -File run_tests.ps1
```

路径等差异用环境变量覆盖，不必改脚本：

| 变量 | 含义 | 默认值 |
| --- | --- | --- |
| `UEFIDOS_GCC_DIR` | 存放 `gcc.exe` / `objdump.exe` 的目录 | `C:\msys64\mingw64\bin`，否则用 `PATH` |
| `UEFIDOS_PYTHON` | Python 解释器 | `python` |
| `UEFIDOS_QEMU` | `qemu-system-x86_64.exe` | `C:\Program Files\qemu\qemu-system-x86_64.exe` |
| `UEFIDOS_OVMF_DIR` | 固件目录（`edk2-*-code.fd`、`edk2-i386-vars.fd`） | `C:\Program Files\qemu\share` |
| `UEFIDOS_DOS211` | 你自己的 MS-DOS 2.11 软盘镜像 | 未设置 → 跳过软盘相关检查 |
| `UEFIDOS_DOS5` | 你自己的 MS-DOS 5 软盘镜像 | 未设置 → 跳过该项检查 |

## 交互式 QEMU 启动器

双击 `start_qemu.bat`（或运行 `run_interactive.ps1`）即可在 QEMU 窗口里启动
UEFI-DOS：按需构建、跑一段演示 `AUTOEXEC.BAT`（VER/DIR/HELLO/TELETYPE/MZEXE/
RUNNER/VRAM/APITEST/UPCALL），然后**停在 `C:\>` 提示符**让你自己输入命令：

```
start_qemu.bat                 # 窗口模式，启动到 C:\>
start_qemu.bat -Headless       # 无窗口，只留串口日志
start_qemu.bat -Firmware ia32  # 32 位载荷
```

在提示符下，`APITEST` 演示原生 32/64 位 API，`UPCALL` 演示 16 位程序调用原生服务，
`HELP` 列出内建命令，`SHUTDOWN` 关机。

若设置了 `UEFIDOS_DOS211`，演示镜像还会带上 `DOS211.IMG` 并挂成 `A:`。截图工具（仅用
标准库）：先用 `-monitor tcp:127.0.0.1:55555,server,nowait` 启动 QEMU，然后
`python tools/screenshot.py 55555 build/screen.ppm` 与
`python tools/ppm2png.py build/screen.ppm build/screen.png`。

## 原生 32/64 位程序（SDK）

DOS 程序是 16 位的；UEFI-DOS 另外还能跑**原生**程序：自由式（freestanding）PE 镜像
被映射到可执行内存、做完基址重定位后**直接在真 CPU 上调用** —— 没有解释器、没有模式
切换 —— 而它用的 DOS 服务、文件句柄、当前目录，和 16 位世界是同一套。

```sh
make sdk                       # 产出 build/HELLO64.EXE（PE32+）与 HELLO32.EXE（PE32）
```

```powershell
powershell -ExecutionPolicy Bypass -File sdk\build_sdk.ps1
```

一个原生程序就是一个普通 C 文件加一个入口函数：

```c
#include "uefidos/dosapi.h"
#include "uefidos/runtime.h"

int uefidos_main(const uefidos_exports *api, const char *args)
{
    uefidos_init(api);
    rt_puts("hello from a 64-bit program\n");

    u16 h;
    if (api->svc->open("HELLO.TXT", DOS_OPEN_READ, &h) == 0) {
        char buf[80]; u32 got;
        api->svc->read(h, buf, sizeof(buf) - 1, &got);
        api->svc->write(1, buf, got, &got);       /* 句柄 1 = 控制台 */
        api->svc->close(h);
    }
    i16 code;                                      /* 16 位下行调用 */
    api->svc->exec16("HELLO.COM", "", &code);
    return 0;
}
```

```
C:\> RUN64 HELLO64.EXE
RUN64: loading HELLO64.EXE as a native 64-bit image
=== UEFI-DOS native SDK demo ===
pointer size : 64 bit
DOS version  : 4.00
reading      : HELLO.TXT (16 bytes)
Hello UEFI-DOS!
wrote        : NATIVE.TXT (44 bytes)
Hello from 8086 code!
exec16       : HELLO.COM -> exit 0 (16-bit program ran)
NATIVE-OK
RUN64: exit code 0 -> NATIVE-OK
```

其中 `Hello from 8086 code!` 那一行，是 64 位程序启动的 16 位 `.COM` 打印出来的 ——
下行调用链完整跑通。位宽由加载器强制校验（64 位内核拒绝 32 位镜像：那段代码不可能在
64 位地址空间里执行）。完整的服务表、链接参数与已知限制见
[`docs/sdk-guide.md`](docs/sdk-guide.md)。

## 指令级解释器测试

`src/emu/emu.c` 是一个巨大的 `switch`，过去这里的回归往往表现为"某个游戏画面糊了"。
`src/emu/itest.c` 把每条指令变成"一段字节流 + 寄存器/内存期望值"，交给**真实解码器**
执行：

```powershell
build\hosttest.exe --itest          # 49 条用例
build\hosttest.exe --itest shift    # 只跑名字里带 shift 的用例
```

```
  [PASS] add ax,bx (reg dst)
  [PASS] shl ax,1
  [PASS] rep stosw (3 words, 2 store units)
  [PASS] jmp far (EA)
itest: 49 passed, 0 failed
ITEST PASSED
```

加一条用例就是改五行表格；格式、坑（宏里的逗号、`lodsb` 不动 `AH`）以及尚未覆盖的
部分见 [`docs/itest.md`](docs/itest.md)。

## 图形、额外盘符与长文件名

**图形。** `GFXTEST.COM` 用 `INT 10h` 切到 mode 13h，通过 `AX=1012h` 装载整张
DAC 调色板，画出 320×200 并打印 `GFX-OK 320x200x256`；`VESATEST.COM` 走完 VESA
调用（`4F00`、`4F01`、带线性帧缓冲位的 `4F02`）进入 640×480×256，然后分五个
64KB 窗口把整屏画满。在 UEFI 下，模拟帧缓冲会转成 GOP 像素格式直接写进
`Mode->FrameBufferBase`，并按整数倍放大（320×200 在 640×480 屏上放大 2 倍、四周
留黑边）。图形模式下文本输出改走串口，否则控制台会把自己的文本网格重新画到画面
上。两条被放弃的固件 `Blt` 路径与细节见 [`docs/graphics.md`](docs/graphics.md)。

**每个卷都是一个盘符。** `plat_enum_blockdevs()` 向固件枚举所有 `BlockIo` 句柄：
启动卷排第一（`C:`），若存在逻辑分区就跳过整盘句柄（`D:` 不会与 `C:` 指向同一片
扇区），再按设备路径标注 `USB`/`SATA`/`NVME`/`SD`，依次挂成 `D:`、`E:`……只读介质
自动只读挂载。`DRIVES` 打印这张表：

```
C: >FAT32 UEFIDOS
D:  FAT32 USBSTICK
```

**长文件名。** LFN 链按 8.3 校验和验证，`open` 时大小写不敏感地解析，`DIR` 显示，
原生 API 一并返回。给一个 8.3 表达不了的名字（`Long Name Report.txt`）建文件时，
会写入 LFN 链并生成别名（`LONGNA~1.TXT`），原生 API 可用 `longname()` 把别名反查
回长名；DOS 程序依旧只见 8.3 —— 这正是设计：老的一侧保持老，新的一侧拿到真名。

```
C:\> RUN64 HELLO64.EXE
long name    : created "Long Name Report.txt"
long read    : this file has a VFAT long name
8.3 alias    : LONGNA~1.TXT -> Long Name Report.txt
...
C:\> DIR *.TXT
Long Name Report.txt         31 13:12  11-11-26
```

## 回归测试

`run_tests.ps1` 会先完整构建，再跑 34 项检查：FAT32 读/写自检；宿主命令解释器
DIR/TYPE/VER/COM/EXE/EXEC/显示 + 原生 API 与上行调用演示；宿主写路径；软盘镜像挂载
为 `A:`；在 2.11 镜像上写 FAT12；读取 2.11 与 MS-DOS 5 镜像；两项 QEMU/OVMF 启动
冒烟测试（x86_64 与 IA32）；阶段 4/6 语义与双路径对比；32 MB 与 1.9 GB FAT16 卷写入；
由真实 16 位代码调用 FCB 的 `FCBTEST.COM`；DOS 4.0 一致性探针 `DOS4TEST.COM`（DPB、
list of lists、扩展错误、`AH=6Ch` 的创建/打开/覆盖/拒绝四态、临时文件、truename、DBCS、
commit —— 共 30 项断言，最后打印一行 `DOS4-OK`）；以及 `KEYTEST.COM` —— 它通过 `INT 16h` 读五个键并打印扫描码，证明方向键和功能键确实以真实 BIOS 扫描码送达客户机。

需要 Microsoft DOS 软盘镜像的检查在 `UEFIDOS_DOS211` / `UEFIDOS_DOS5` 未设置时会
报告 `[SKIP]` 并跳过。

## 宿主测试器

`hosttest.exe` 在 Windows 上用文件模拟块设备，跑同一套 FAT 驱动、内核、命令解释器、
原生 API 与模拟器 —— 不经过 QEMU 的快速迭代：

```powershell
build\hosttest.exe build\UEFIDOS.img                 # FAT32 读/写自检
build\hosttest.exe --shell build\UEFIDOS.img "DIR" "TYPE HELLO.TXT" "APITEST" "UPCALL"
build\hosttest.exe --fatlist  "C:\path\to\floppy.img"  # 查看 FAT12/16/32 镜像
build\hosttest.exe --phase46  build\t_p46.img          # SHARE / FILES= / FCB / 锁 / 双路径
build\hosttest.exe --fat16    build\fat16_2g.img big   # 1.9 GB 卷 + 32 MB 文件
```

以下调试开关只影响宿主构建，用来让兼容性复现可重复：

| 变量 | 作用 |
| --- | --- |
| `UEFIDOS_MAX_STEPS=n` | 单个程序的指令预算（默认 200000000），每投递一个键就补满 |
| `UEFIDOS_HARD_STEPS=n` | 绝对预算，投递按键也不补，交互程序仍能被终止 |
| `UEFIDOS_KBD=k1,k2,…` | 脚本化按键：单字符，或 `ENTER ESC SPACE TAB BACKSPACE HOME END PGUP PGDN INS DEL UP DOWN LEFT RIGHT F1…F10`，以及 `CTRLA…CTRLZ` |
| `UEFIDOS_KBD_MAX_POLLS=n` | 轮询多少次后投递 `ESC`，让"等按键"循环结束（默认 100000） |
| `UEFIDOS_PSP_SEG=0x260` | 覆盖加载段，用于按固定 PSP 链接的程序 |
| `UEFIDOS_DUMP_VRAM=1` | 把模拟的 80×25 屏 dump 到 `build/vram_dump.txt`（带列标尺） |
| `UEFIDOS_DUMP_MEM=file` | 程序结束时把低端 640 KB 写文件（与真 DOS 下 QEMU `pmemsave` 的结果逐字节对比） |
| `UEFIDOS_DUMP_AT=CS:IP`（配合 `UEFIDOS_DUMP_AT_FILE=file`） | 执行第一次走到该地址时把低端 640 KB 存盘（默认 `build/mem_at.bin`）——用来在"分配器放弃"的那条指令上抓下程序的内存现场 |
| `UEFIDOS_TRACE_INTS=1`、`UEFIDOS_TRACE_VRAM=n`、`UEFIDOS_TRACE_STEPS=n`、`UEFIDOS_TRACE_WINDOW=CS:LO-HI`、`UEFIDOS_TRACE_PORTS=1` | 中断、显存写入、单步、地址窗口、端口 I/O 跟踪，输出在 `build/` |
| `UEFIDOS_TRACE_IRQ=1` | 硬件中断跟踪：每往客户机处理程序压入一个 INT 8h/9h 帧就打一行（`[irq] vec=08 depth=0 from CS:IP to CS:IP`），另加周期性 `[irqdbg]` 行（IF 状态、irq 嵌套深度、向量是否仍指向我们的桩） |
| `UEFIDOS_TRACE_SS=1` | 打印每一条改变了 `SS` 的指令，以及它落到的栈帧（`[ss] #1234 1409->0D00 sp=0826->0826 after 0D00:0592 next=0D00:0594 ax=...`）——程序悄悄跑到了错误的栈段（本应是自己的栈段却指向 PSP，或被错误的 `IRET` 恢复了过期栈帧）在 DOS 调用轨迹里完全看不出来；Windows 1.01 的 `FatalExit stack overflow` 就是这样被定位到「过期栈帧」而不是真正的栈溢出 |
| `UEFIDOS_TRACE_IVT=1` | 打印客户机每一次 `INT 21h AH=25h`（`[ivt] set 08 = 15B7:0244 by CS:IP`）——看程序装了哪个中断处理程序、以及到底有没有装，这是最快的手段 |
| `UEFIDOS_TRACE_PSP=1`、`UEFIDOS_TRACE_EXEC=1` | 打印每个新 PSP 的命令行尾部（及其后 16 字节）、每次 `INT 21h AH=4Bh`（路径、环境段、命令行远指针、调用者 PSP/DS），以及失败的 `AH=48h` 请求 |
| `UEFIDOS_TRACE_MEM=1` | 在 `INT 21h AH=4Ah` 之后、以及每次 `AH=48h` 失败时打印 DOS 内存 arena（MCB 链：段址、`M`/`Z`、owner PSP、大小、数据范围）——查看程序“先缩小再探测”的内存测算是否算对，这是最快的手段 |
| `UEFIDOS_EXEC_HIGH_IMAGE=1` | 实验开关（非默认）：把 EXE 镜像装到程序内存块的顶部、PSP 留在底部——这正是参考机 MS-DOS 5.00 跑 Windows 1.01 内核时的布局（默认关闭，因为程序普遍把 `PSP+0x10` 当作自己的段） |
| `UEFIDOS_TRACE_DIV=1` | `DIV`/`IDIV` 因商溢出陷入时打印 `CS:IP` 和各寄存器 |
| `UEFIDOS_EXEC_HIGH=1` | 把 `EXEC` 的子进程贴着常规内存顶端装载（而不是紧跟调用者之后），用于试探对装载地址敏感的程序 |

程序撞到指令上限时，最后 64 步会 dump 到 `build/limit_dump.txt`。

## 与真实 DOS 软件的兼容性

本仓库**不包含**任何 DOS 系统文件、商业应用或游戏 —— 测试镜像里只有
`tools/*.py` 生成的程序。要验证真实软件兼容性，请自备磁盘镜像
（`UEFIDOS_DOS211`、`UEFIDOS_DOS5`）或把程序拷进 FAT32 镜像。

`docs/compat.md` 记录了一次 CardPuter86 时期 DOS 程序（游戏、终端、汇编器、硬件
自检）的兼容性扫描（镜像由作者自备），包括这次扫描暴露的五个模拟器缺陷：ALU 方向位、
移/循环选择子编号、`0040:001A/001C/001E` 处缺失的 BIOS 键盘缓冲区、中断向量不可调用、
以及缺失的 `INT 1Ah` 计时。这些程序是第三方作品，不属于本仓库。

另外注意：`INT 21h AH=30h` 现在报告 DOS **4.00**（`AX=0x0004`），让现代工具走它们
的 DOS 4+ 分支。0.8.0 及之前报告的是 `2.10`（`AX=0x0A02`），因为 Compaq DOS 2.11 的
`SYS.COM` 版本门槛会拒绝大于 10 的次版本号 —— 报 4.00 的代价正是这一点：那张 2.11
软盘上的 `SYS.COM` 现在会拒绝运行。想回到旧行为，`src/uefidos_config.h` 里注释写明了
两行改法（`UEFIDOS_DOSVER_MAJOR 2`、`..._MINOR 11`、`..._COMPAT_MINOR 10`）。

### Windows 1.x / 2.x

在 CGA 图形模式下，文本服务现在会把 8x8 字形直接画进交错位图（`src/common/font8x8.c`，公有领域字体；模式 4/5 是 40 列、模式 6 是 80 列），所以程序切到 CGA 模式后再用 `AH=0Eh` 打印的内容是看得见的——Windows 1.01 的启动横幅就是这么打印的。`tools/make_cgatext.py` 生成的 `CGATEXT.COM` 就是回归项 `graphics: INT 10h text services paint glyphs in CGA mode 4` 的探针。

`INT 10h` 的 CGA 模式、`INT 33h` 鼠标驱动、硬件中断注入都是为 Windows 加的。
**Windows 1.01** 的 `SETUP` 在合并安装卷上能完整装完（识别 CGA、拷完 build/utility
盘、生成 `WIN.COM`/`WIN100.BIN`/`WIN100.OVL`）；`WIN` 在内核版本门槛被
`SETVER WIN.COM 3.30` / `SETVER WIN100.BIN 3.30` 满足后启动内核，**内核现在会把它的
启动画面画出来**：切到 CGA 640×200、画出 “MICROSOFT” logo，然后才以退出码 1 结束。
在它之前挡着的是两个 EXEC 的真 bug，都已修掉：

* `INT 21h AH=4Bh` 原来只拷“计数 + 字符串”，遇到第一个 CR 就截断。DOS 会把调用方
  **整个 128 字节命令行缓冲区**（`PSP:0080h..00FFh`）原样复制给子进程，而 3.0 之前的
  软件就往那里藏数据：`WIN.COM` 把字 `'XW'` 写在 `PSP:00FEh`，Windows 内核
  （`CS:653Eh`）没有这个标记就拒绝启动。
* 命令行**计数字节不含结尾的 CR**，所以光命令的 count 是 0（CR 仍在 `0081h`），不是 1。
  `WIN.COM` 用 `0081h+count` 定位它要追加的覆盖文件名、且从不回写 count；差一个字节就
  把路径推后两字节，内核于是拼出一个 30 字符的模块名，而不是 13 字符的 `C:WIN100.BIN`。

第三个决定性的调用是 `INT 21h AH=47h`（取当前目录）：我们在路径前多加了一个反斜杠，
且根目录返回 `AX=0000`，而真 MS-DOS 5 返回的是不带前导反斜杠的路径与 `AX=0100h`。
Windows 把这个答案拼进 overlay 名字，于是名字多了分隔符、长了 8 字节，模块数据库的
请求也跟着变大。修好格式、并把同一批文件平铺到根目录（真 DOS 参考跑就是这么摆的）后，
内核跑到 129 次 DOS 调用并把启动 logo 画了出来。`INT 21h AH=57h`（取文件日期时间）之前也是
个返回 0 的空壳；现在句柄携带目录项时间戳，SFT 也一并发布。剩下的分歧是：
内核打开 `WIN100.OVL`、取了时间戳、然后不读就关掉并仍以 1 退出，因为它自己的局部堆
（`ne_heap` = 512 字节 → 可用 428）比该布局下模块表要的 435 字节少了几个字节。追踪与
arena/堆转储对照都在
[`docs/windows1x.md`](docs/windows1x.md)。`TAILCHK.COM` 回归（`COUNT=00 C81=0D`）
防止 tail 语义再漂回去。

**Windows 1.0 Developer Release 5（1984-11-04）** 可以用 `tools/install_win1x.py`
按它 `INSTALL.BAT` 的布局装进 UEFI-DOS 镜像（`start_win1x.bat` 双击启动），但它的
`KERNEL.EXE` 在自己引导阶段就把自己的代码和栈清零了 —— 在发出第一个 `INT 21h` 之前
就死了。同一批文件放到**真实 Compaq MS-DOS 2.11** 启动软盘上跑，表现完全一样（黑屏
挂死，COM1 也没有输出），所以这不是 UEFI-DOS 的问题：这个 DR5 内核在任何标准 DOS
EXE 装载下都会写坏自己。证据与反汇编见 `docs/windows1x.md`。

Windows/286 2.10 需要 286 保护模式和 EMM，UEFI-DOS 目前没有。

**Windows 1.0 Developer Release 5（1984-11-04）** 可以用 `tools/install_win1x.py`
按它 `INSTALL.BAT` 的布局装进 UEFI-DOS 镜像（`start_win1x.bat` 双击启动），但它的
`KERNEL.EXE` 在自己引导阶段就把自己的代码和栈清零了 —— 在发出第一个 `INT 21h` 之前
就死了。同一批文件放到**真实 Compaq MS-DOS 2.11** 启动软盘上跑，表现完全一样（黑屏
挂死，COM1 也没有输出），所以这不是 UEFI-DOS 的问题：这个 DR5 内核在任何标准 DOS
EXE 装载下都会写坏自己。证据与反汇编见 `docs/windows1x.md`。

Windows/286 2.10 需要 286 保护模式和 EMM，UEFI-DOS 目前没有。

## 从图形模式交还文本模式

退出图形模式不只是“别再写像素”：缩放后的游戏画面只占屏幕中间一块（四周是边距带），
而固件控制台只重画它自己的字符网格。于是 Windows 1.x 的启动画面会**残留**在屏幕
上——`C:\WINDOWS>` 提示符回来时，底下还压着被弃置 logo 的两块白斑。现在
`plat_video_text()` 会把整块线性帧缓冲擦掉（`gfx_clear()`）并刷空 25x80 的字符网格，
`plat_video_gfx_begin()` 进图形模式时也擦一次，边距带因此是黑的而不是上一屏的文字。

## 目录结构

```
src/
  uefi/main.c        入口、启动横幅、挂载 C:、发布 API 表、跑命令解释器
  uefi/efi_plat.c    UEFI 平台层：ConOut/ConIn、BlockIo（全部卷）、GOP 帧缓冲、
                     RTC、计时、串口诊断、关机
  plat.h             平台抽象（Windows 宿主也有实现）
  common/util.c      自立 mem/str 例程（不用 libc）
  common/divdi3.c    64 位除法助手（-m32 -nostdlib 需要）
  common/cp437.c     CP437 → Unicode 表，供控制台镜像使用
common/font8x8.c   公有领域 8x8 字体（CGA 图形模式下的文本输出）
  fs/fat.c|h         块设备上的 FAT12/16/32 驱动（三者都有写路径）
                     含 VFAT 长文件名的解析与写入
  kernel/dos.c|h     驱动器（多卷）、含长文件名的路径解析、文件句柄、find/DTA、
                     CONFIG.SYS、SHARE 模式、字节区间锁、只读删除保护
  api/dosapi.c|h     原生 32/64 位 DOS API（与 INT 21h 对齐的服务表 + 导出）
  api/native.c|h     原生 PE 加载器：映射、重定位，在真 CPU 上调用 uefidos_main()
  shell/shell.c|h    类 COMMAND.COM 解释器与批处理执行
  emu/emu.c          8086 实模式解释器、PSP/IVT、.COM/.EXE 加载器、
                     文本显存镜像 + 图形模式与 VGA DAC
  emu/int21.c        INT 21h / INT 10h / INT 13h / INT 16h / INT 1Ah 服务
  emu/fcb.c          DOS 4.0 FCB 调用族（AH=0Fh..28h）
  emu/int2f.c        INT 2Fh 原生命令复用（16 位 → 原生上行调用）
  emu/dpmi.c         DPMI 0.9 宿主：INT 2Fh/1687h、INT 31h、INT 60h 入口
  emu/int33.c        INT 33h 鼠标驱动（位置、按键、事件回调）
  emu/itest.c        指令级测试向量（hosttest --itest）
  host/              Windows 平台 + 自检/命令解释器/fatlist/phase46/fat16 测试器
sdk/
  include/uefidos/   dosapi.h（公开 ABI）、runtime.h（自立辅助函数）
  examples/hello64.c 原生演示程序（文件 I/O、查找、时钟、exec16）
  Makefile           用 mingw 交叉编译器构建 HELLO64.EXE / HELLO32.EXE
  build_sdk.ps1      Windows 上的同一套构建
tools/
  fat32.py           FAT32 格式化器（无依赖）
  make_gfx_test.py   生成 GFXTEST.COM / VESATEST.COM（图形探针）
  gfx_shot.ps1       在 QEMU 里启动镜像并给帧缓冲截图
  make_image.py      镜像生成器（启动文件、文本文件、AUTOEXEC.BAT）
  make_hello.py      生成 HELLO.COM / TELETYPE.COM / MZEXE.EXE / RUNNER.COM /
                     VRAM.COM / UPCALL.COM
  make_pm_tests.py   手汇编 DPMITEST.COM（实模式 → PM16 → PM32 → 反射）
  make_fcb_test.py   手汇编 FCBTEST.COM（FCB 建/写/读/删）
  make_dos4_test.py  手汇编 DOS4TEST.COM（DOS 4.0 调用面，30 项检查）
  make_tools.py      生成内置工具 INFO/CAL/SUM/HEX/CLOCK
  make_release.ps1   打包"开箱即用"发布版（镜像 + start.bat）
  mkfat16.py         稀疏 FAT16 格式化器（32 MB .. 1.9 GB 测试卷）
  fat_extract.py     只读 FAT12/16/32 提取器（list/get/extract）
  extract_dos_file.py  按簇链从裸 FAT 镜像里取出单个文件
  qemu_probe.py      通过 QEMU monitor 驱动 UEFI shell
  qmon.py qtype.py screenshot.py ppm2png.py   QEMU monitor/键盘/截图助手
docs/                兼容性报告、DOS 4.0 语义、迁移坑、保护模式/DPMI 笔记
Makefile Dockerfile   一条命令构建（Linux/macOS/WSL，以及容器内）
build.ps1 run_uefi.ps1 run_interactive.ps1 run_tests.ps1 start_qemu.bat
```

## 设计要点

- **没有 BIOS，没有 V86，不用硬件虚拟化。** 16 位 DOS 软件由软件 8086 CPU 在模拟内存
  上解释执行，因此可移植到 x86_64 与 IA32 两种 UEFI（以及任何宿主），不依赖 VT-x/AMD-V。
- **中断重定向。** 受服务的中断指向每向量一条跳板 `F000:E000+n*4`（`INT n; IRET`），
  模拟器认出这是自己的入口，于是 `pushf; call far [vector]` 与 `int n` 两种调用约定都
  像真 BIOS，程序自己装的中断处理程序（`INT 21h AH=25h`）也照常工作。
- **显示。** 模拟的 80×25 文本屏就在 `B800:0000`，写入会标脏，然后在中点边界与长时间
  运行期间被拷到 UEFI 控制台。
- **EXEC（AH=4Bh）。** 子程序加载到全新段、带自己的 PSP，在同一颗模拟 CPU 上运行；
  子进程结束时父进程的寄存器、DTA 与句柄表被恢复。
- **段缓存与 `op32`。** 32 位代码段的默认操作数/地址宽度是 32，`0x66`/`0x67` 反而切回
  16 位 —— 所有宽度判断都走 `op_is32()` / `ad_is32()`，而不是裸前缀标志。
- **FAT 写路径**由 FAT12/16/32 共用；挂载的软盘镜像拥有一层块设备垫片，写入被转发到
  `C:` 上映像文件的簇链；FAT 扇区缓存有 8 个槽位，避免簇链遍历反复读盘（QEMU 里写
  8 个扇区从 32.7 秒降到 113 毫秒）。
- **路径模型**：目前只支持 8.3 名；`\`、`/`、`.`、`..` 归一化，盘符 `A:`..`D:`，先探测
  MBR 分区，失败再按超级软盘处理。

## 值得一提的已修 bug

1.1.0 之后的 Windows 1.01 攻关又逮到四个：

- **`AH=36h` 在每块盘上都报“0 字节可用”。** 它把 FAT 链值为 `0FFFFFFFDh` 的簇
  当成空闲（真正的空闲簇链值是 0），只扫 3 万个簇，还把每扇区字节数写死成 512，
  于是 `BX` 永远是 0。安装程序每次写文件前都会查这个数——Windows 1.01 的
  `SETUP` 就是其中之一——读到 0 就当“磁盘满”。现在直接返回卷维护的空闲簇数。
- **`AH=25h` 用错了寄存器取中断号。** 设置中断向量的调用把中断号放在 **`AL`**，
  实现却按 `AH` 算 IVT 槽位，于是不管调用者要哪个向量，写进去的都是 37 号槽。
  只保存/恢复向量的程序察觉不到；真正*安装*处理程序的程序会当场失败——Windows
  1.x 用 `INT 22h` 这么做，随后回读向量，读不到就打 `Incorrect DOS version` 退出，
  这正是 1.01 卡了很久的地方。
- **`AH=50h`（设置当前 PSP）是个空操作。** 它落到“未知调用，直接报成功”的 default
  分支，`g_dos.psp_seg` 一直是 0，所有记录当前 PSP 的内部结构与程序看到的都不一致。
- **把 DOS 内部结构用 `emu_wb` 写，在 DPMI 下会炸。** 新加的 SFT/SDA/JFT 一开始
  走 `emu_wb(e, MAGIC_SEG, …)`，保护模式下 `0F000h` 会被当选择子解析并触发保护
  错误，`DPMITEST` 的 32 位那一半就此挂掉；现在全部改成按线性地址写，回归里也加了
  `AH=25h/34h/50h/51h/52h/5D06h` 与 `PSP:18h` 作业文件表的用例。

1.0.0 这一批来自实跑 DOOM 0.99（见 `docs/doom.md`）——它自带的 DOS/16M 加载器
是最苛刻的 CPU/DOS 一致性测试：

- **`D6h`（SALC）曾是未知指令。** Windows 1.01 从 `WIN100.OVL` 载入的第一个模块会执行
  未公开的 8086 指令 `D6h`，模拟器当时直接报 `UEFI-DOS: unsupported 8086 opcode 0xD6`
  并终止。现已实现为 `AL = CF ? 0FFh : 0`（不影响标志位），并补了两条指令测试向量。
- **`POPF` 丢弃了 IOPL 与 NT**（掩码 `0x0FD5`，那是 8086/286 的行为）。扩展器写
  `FLAGS=0F000h` 再读回来判断是 386 还是 286，于是在一个**已经支持保护模式**的
  模拟器上打出 `DOS/16M error: [15] protected mode available only with 386 or 486`。
  386/486 的 12-14 位可写、只有第 15 位保留，16 位 `POPF` 现在按 `0x7FD5` 掩码。
- **缺 `0F BA`**（立即数位的 `BT`/`BTS`/`BTR`/`BTC`）——加载器通过 CPU 检测后
  执行的下一条指令，模拟器直接报 `unsupported 386 opcode 0F BA` 停机。
- **环境块里没有程序路径**。DOS 把被执行的程序全路径放在 `env:2`（前面是串计数
  字），所有 DOS 扩展器都靠它重新打开自己的镜像。我们只写了 `PATH=C:\`，于是
  加载器打开 `""`，随后报 `not a DOS/16M executable 'DOOM.EXE'`。`build_psp()`
  现在写完整的 DOS 布局，包括 DOS 5 的 "extra strings" 尾块。

- `fat_read_chain_byte` 在跨多个 512 字节块读取时重复累加输出偏移、并且重加了扇区索引，
  导致多簇 `.COM`/`.EXE` 整体偏移一个簇加载。
- `INT 21h AH=35h` 会把内部的 `F000:FFF0` 服务标记泄漏给程序（应返回可远调用的 IRET 跳板）。
- ALU 方向位（`0x02/0x03/0x0A/0x0B/…/0x2B/0x3B`）：寄存器形式把结果写回了**源**操作数，
  于是 `sub bx,ax` 等于没改 `BX`。
- `do_shift` 里移/循环的选择子编号写成了 `0=SHL,1=SHR,2=SAR,3=ROL,4=ROR,…`，而架构编码是
  `0=ROL,1=ROR,2=RCL,3=RCR,4=SHL,5=SHR,6=SAL,7=SAR`，于是 `SHL AX,1` 实际跑成
  `ROR AX,1` —— 这正是当年所有游戏画面错位的根因。
- FCB 调用从 `ES:DI` 取 FCB、从 `DS:DX` 取 I/O 缓冲区，正确约定是 `DS:DX` 取 FCB、
  缓冲区取 DTA。
- `dos_fat_name()` 无条件解引用可能为 NULL 的 `ok` 指针；`dos_unlink()` 不拒绝只读文件。
- FAT 扇区缓存原本只有单个槽位，簇链遍历反复读盘 —— 写 8 个扇区要 32720 毫秒，改成 8 槽
  缓存后 113 毫秒。
- `INT 21h AH=3Ch` 对已存在的文件做截断时，把目录项的位置查回到了**刚被清零的同一个结构体**里，
  `fat_find()` 因此把旧的簇号与大小又写了回去：文件指向一个刚刚被释放的簇
  （`FAT[簇] == 0`），下一次写入直接报"损坏"（DOS 错误 0x1E）。修法是另用一份临时目录项查位置，
  并且**先**把目录项收缩、**再**释放簇链。这个 bug 是 SDK 示例连续两次创建同一个文件才暴露出来的。
- `src/efi/efi.h` 里 `AllocatePages`/`AllocatePool` 的内存类型参数写成了 `UINT64`/`UINT32`，
  而固件 ABI 里 `EFI_MEMORY_TYPE` 是 `UINTN`：IA32 上多压了 8 字节，于是**每一个**原生镜像
  加载都失败并打印 `native loader: out of memory`，而参数宽度恰好对上的 x86_64 却正常。
  改成 `UINTN` 并补了 `EfiLoaderCode` 池回退，现在两种固件各自的原生程序都能跑
  （IA32 冒烟测试已验证）。
- **新指令级测试框架第一次运行报了 4 条失败，逐条排查后全部是测试自己期望值写错**
  （`div` 把商写进 `AX`、`lodsb` 不改 `AH`、有一处 `mov [bx],ax` 夹具写的正是原值）。
  这正是想要的结果：先把标尺校准，再怀疑解释器。

- `INT 21h AH=19h`（取当前驱动器）永远返回 `A:`，`AH=0Eh` 根本没有真正切换驱动器，
  `AH=2Ah` 的星期值是写死的 1 而不是算出来的 —— 日历和星期相关逻辑因此全错。
  这三个问题都是写内置 `INFO`/`CAL` 工具时测出来的。

- **方向键以及所有扩展键被静默丢弃**：两个平台层的 `plat_kbd_poll()` 都把
  `EFI_INPUT_KEY.ScanCode` 原样返回（`UnicodeChar == 0` 时干脆返回“没有键”），
  host 层则把 `0x00`/`0xE0` 序列的第二个字节扔掉。于是所有读 BIOS 键盘缓冲区的菜单、
  编辑器和小游戏都收不到任何光标键。现在 EFI 扫描码会翻译成 PC 扫描码集 1，并以
  `0x100 | scan` 交给模拟器，在 BIOS 缓冲区里落成“扫描码 + ASCII 0”。连带修掉两个键盘问题：
  `INT 16h AH=01h` 会把本该只是“看一眼”的键消费掉（先查后读就丢键，菜单一次跳两格）；
  交互程序永远不会被终止，因为每投递一个键就把指令预算补满（宿主侧新增
  `UEFIDOS_HARD_STEPS` 作为不补的绝对上限）。
- **`INT 10h AH=09h/0Ah` 根本没有写显存** —— 它把字符塞进控制台流、再用空格和换行去
  “凑”位置；随后屏幕镜像又用空的显存把它重画掉，于是任何自己画屏的程序都是黑屏：
  内置的 2.11 编辑器 `TE.COM` 明明已经正确读到文件，看起来却是“打不开文档”。
  现在全部文本服务（`AH=00h 02h 03h 06h 07h 08h 09h 0Ah 0Eh 13h`）都作用于模拟显存 +
  BIOS 数据区光标（`0040:0050`），并实现了真正的窗口滚动。同时如果固件提供 80×25 模式，
  控制台会切过去，让客户机屏幕占满整屏而不是浮在一个独立控制台区上方；镜像刷新也只重画
  真正变化了的行。
- 指令 `0x62/0x63/0x69/0x6B/0x6C-0x6F`（BOUND、ARPL、IMUL 立即数形式、INSB/W、OUTSB/W）
  缺失，导致内置 `MINIGAME` 合集一启动就撞 `unsupported 8086 opcode 0x6F` 停住。

## 限制与路线

- **还没有 ring 3**：DPMI 客户仍运行在 ring 0。没有 TSS，没有 `CR3` 页表与 `#PF` 分发；
  `AX=0600h`（分页）与 `AX=0B00h`（硬件中断）是桩。
- 只支持 8.3 名（不能创建 LFN）；只有文本模式（未模拟 `INT 10h` 图形模式）。
- `REN` 只在同一目录内；`COPY` 不支持通配符；挂载的 `A:`/`B:` 镜像大小固定。
- FAT32 的 `FSInfo` 空闲簇数不写回（内核内存里维护 `free_count`）。
- 未实现 TSR 常驻（`INT 27h` 只终止程序）。
- 单文件按 DOS 4.0 语义限 32 MB（理由见 `docs/dos4-semantics.md` §4.6）。
- 目前只挂载启动卷（`C:`）：其他 UEFI 块设备（U 盘、其他分区）还没有枚举成
  `D:`/`E:` —— 设计见 [`docs/roadmap-video-usb-lfn.md`](docs/roadmap-video-usb-lfn.md)。
- 原生程序：64 位构建不能运行 32 位镜像（反之亦然）；除 C SDK 外还没有
  C++/Zig/Rust 的接入胶水代码。
- 路线（顺序按设计文档的建议）：把所有块设备枚举成 `D:`/`E:`（半天）→ LFN 读取
  （1 天）→ 经 GOP 实现 VESA 640×480×256（3 天）→ LFN 创建 → ring 3 + TSS/门 →
  `CR3` 分页 + `#PF` → DPMI 0600/0B00 → 扩充兼容性矩阵。

## 许可证

MIT —— 见 [`LICENSE`](LICENSE)。

本仓库**只**分发原创源码、由自带工具生成的测试程序与文档。Microsoft MS-DOS / PC-DOS
系统文件、商业 DOS 应用与游戏、OVMF/edk2 固件、QEMU 二进制都不在此分发；请自行获取，
并不要把它们提交进本仓库。
