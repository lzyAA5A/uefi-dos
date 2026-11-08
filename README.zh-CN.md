# UEFI-DOS

[English](README.md) | **简体中文**

**用 UEFI 重写的 DOS。** 一个可在 x86_64 与 IA32 固件上以 UEFI 应用形态启动的小型
DOS 兼容运行环境：自己挂载 FAT32、自带类 `COMMAND.COM` 的命令解释器，并通过内置
8086 解释器运行 16 位实模式 DOS 程序 —— 一套"类似 V86"的软件兼容层，**不依赖 BIOS，
也不依赖硬件虚拟化**。

兼容基线是 **MS-DOS 2.11** 语义（程序看到的版本就是它），在此基础上补上 2.11 从未
有过的东西：FAT32、64 位 UEFI 启动、FAT16 大容量卷、DOS 4.0 的文件共享/加锁/FCB
语义、原生 32/64 位 DOS API，以及 386 保护模式与 DPMI 0.9 宿主。

当前版本 0.5.0 —— 13 项自动化检查全部通过（Windows 宿主测试器，以及 QEMU + OVMF
的 x86_64 与 IA32 实机启动）。

## 功能状态

| 领域 | 状态 |
| --- | --- |
| UEFI 启动（x86_64） | 可用 — PE32+ `BOOTX64.EFI`，从磁盘启动项自动进入 |
| UEFI 启动（IA32） | 可用 — PE32 `BOOTIA32.EFI`，同一镜像同时携带两个文件 |
| 启动卷定位 | LoadedImage → `DeviceHandle` → `BlockIo` → MBR 分区（兼容无分区超级软盘） |
| FAT32 | 读 + 写（建/删/改名/建目录/删目录，簇分配与回收） |
| FAT12 / FAT16 | 读 + 写 |
| 软盘/磁盘镜像挂载 | `MOUNT A: IMAGE.IMG` 把 `C:` 上的镜像文件变成可读写驱动器 |
| 命令解释器 | `DIR CD MD RD DEL REN TYPE COPY CLS ECHO VER VOL DATE TIME PROMPT PATH SET MEM HELP APITEST MOUNT UNMOUNT CONFIG FCB EXIT SHUTDOWN`、批处理、`AUTOEXEC.BAT` |
| DOS 内核 | 8.3 文件名、按驱动器维护当前目录、DOS 错误码、**DOS 4.0 语义**（见下） |
| 8086 模拟器 | 8086/80186 实模式解释器、16 MiB 模拟内存、PSP、IVT、带重定位的 `.COM` 与 MZ `.EXE` 加载器、8087 转义按 8086 空转处理 |
| INT 21h | 控制台、文件 I/O、查找首/次 + DTA、目录、日期时间、版本、中断向量读写、IOCTL、句柄复制、PSP、内存桩、**EXEC（AH=4Bh）**、**AH=67h**、**AH=5Ch** 字节锁、**SHARE 模式** |
| FCB 调用族 | AH=0Fh 10h 11h 12h 13h 14h 15h 16h 17h 21h 22h 23h 27h 28h（`src/emu/fcb.c`） |
| INT 10h / 16h / 1Ah / 13h | 电传输出与光标、阻塞键盘、BIOS 计时、磁盘读/写/几何参数 |
| 显示 | 直接写入 `B800:0000` 的 80×25 文本屏会被镜像到 UEFI 控制台 |
| 原生 32/64 位 API | `g_dosapi` 服务表 —— 操作与错误码与 INT 21h 完全一致，原生指针，同一份源码产出两种位宽的固件 |
| 16 位桥 | `exec16()` 下行调用（原生 → 实模式）与 `INT 2Fh AX=5544h` 上行调用（实模式 → 原生） |
| 386 保护模式 | 实模式 → 保护模式切换、GDT/LDT 解析、带限长检查的隐藏段缓存、`MOV CRn`/`LGDT`/`LIDT`/`LLDT`/`CLTS`、32 位寻址（SIB）、`0F xx` 指令族、保护模式 IDT 分发 |
| DPMI 0.9 宿主 | `INT 2Fh AX=1687h` 引导、`INT 31h` 描述符/DOS 内存/实模式反射 —— 见 [`docs/protected-mode.md`](docs/protected-mode.md) |

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
APITEST: DOS version 2.10, native pointer size 8
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

## 回归测试

`run_tests.ps1` 会先完整构建，再跑 13 项检查：FAT32 读/写自检；宿主命令解释器
DIR/TYPE/VER/COM/EXE/EXEC/显示 + 原生 API 与上行调用演示；宿主写路径；软盘镜像挂载
为 `A:`；在 2.11 镜像上写 FAT12；读取 2.11 与 MS-DOS 5 镜像；两项 QEMU/OVMF 启动
冒烟测试（x86_64 与 IA32）；阶段 4/6 语义与双路径对比；32 MB 与 1.9 GB FAT16 卷写入；
以及由真实 16 位代码调用 FCB 的 `FCBTEST.COM`。

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
| `UEFIDOS_MAX_STEPS=n` | 单个程序的指令预算（默认 200000000） |
| `UEFIDOS_KBD=k1,k2,…` | 脚本化按键：单字符或 `ENTER ESC SPACE TAB BACKSPACE UP DOWN LEFT RIGHT` |
| `UEFIDOS_KBD_MAX_POLLS=n` | 轮询多少次后投递 `ESC`，让"等按键"循环结束（默认 100000） |
| `UEFIDOS_PSP_SEG=0x260` | 覆盖加载段，用于按固定 PSP 链接的程序 |
| `UEFIDOS_DUMP_VRAM=1` | 把模拟的 80×25 屏 dump 到 `build/vram_dump.txt`（带列标尺） |
| `UEFIDOS_TRACE_INTS=1`、`UEFIDOS_TRACE_VRAM=n`、`UEFIDOS_TRACE_STEPS=n`、`UEFIDOS_TRACE_WINDOW=CS:LO-HI`、`UEFIDOS_TRACE_PORTS=1` | 中断、显存写入、单步、地址窗口、端口 I/O 跟踪，输出在 `build/` |

程序撞到指令上限时，最后 64 步会 dump 到 `build/limit_dump.txt`。

## 与真实 DOS 软件的兼容性

本仓库**不包含**任何 DOS 系统文件、商业应用或游戏 —— 测试镜像里只有
`tools/*.py` 生成的程序。要验证真实软件兼容性，请自备磁盘镜像
（`UEFIDOS_DOS211`、`UEFIDOS_DOS5`）或把程序拷进 FAT32 镜像。

`docs/compat.md` 记录了一次 CardPuter86 时期 DOS 程序（游戏、终端、汇编器、硬件
自检）的兼容性扫描（镜像由作者自备），包括这次扫描暴露的五个模拟器缺陷：ALU 方向位、
移/循环选择子编号、`0040:001A/001C/001E` 处缺失的 BIOS 键盘缓冲区、中断向量不可调用、
以及缺失的 `INT 1Ah` 计时。这些程序是第三方作品，不属于本仓库。

另外注意：`INT 21h AH=30h` 报告 DOS **2.10**（`AX=0x0A02`），因为 2.11 时期的工具
（包括 `SYS.COM`）会拒绝大于 10 的次版本号；`VER` 仍显示 2.11，因为内核实现的语义
就是 2.11。

## 目录结构

```
src/
  uefi/main.c        入口、启动横幅、挂载 C:、发布 API 表、跑命令解释器
  uefi/efi_plat.c    UEFI 平台层：ConOut/ConIn、BlockIo、RTC、计时、关机
  plat.h             平台抽象（Windows 宿主也有实现）
  common/util.c      自立 mem/str 例程（不用 libc）
  common/divdi3.c    64 位除法助手（-m32 -nostdlib 需要）
  common/cp437.c     CP437 → Unicode 表，供控制台镜像使用
  fs/fat.c|h         块设备上的 FAT12/16/32 驱动（三者都有写路径）
  kernel/dos.c|h     驱动器、路径解析、文件句柄、find/DTA、CONFIG.SYS、
                     SHARE 模式、字节区间锁、只读删除保护
  api/dosapi.c|h     原生 32/64 位 DOS API（与 INT 21h 对齐的服务表 + 导出）
  shell/shell.c|h    类 COMMAND.COM 解释器与批处理执行
  emu/emu.c          8086 实模式解释器、PSP/IVT、.COM/.EXE 加载器、显示镜像
  emu/int21.c        INT 21h / INT 10h / INT 13h / INT 16h / INT 1Ah 服务
  emu/fcb.c          DOS 4.0 FCB 调用族（AH=0Fh..28h）
  emu/int2f.c        INT 2Fh 原生命令复用（16 位 → 原生上行调用）
  emu/dpmi.c         DPMI 0.9 宿主：INT 2Fh/1687h、INT 31h、INT 60h 入口
  host/              Windows 平台 + 自检/命令解释器/fatlist/phase46/fat16 测试器
tools/
  fat32.py           FAT32 格式化器（无依赖）
  make_image.py      镜像生成器（启动文件、文本文件、AUTOEXEC.BAT）
  make_hello.py      生成 HELLO.COM / TELETYPE.COM / MZEXE.EXE / RUNNER.COM /
                     VRAM.COM / UPCALL.COM
  make_pm_tests.py   手汇编 DPMITEST.COM（实模式 → PM16 → PM32 → 反射）
  make_fcb_test.py   手汇编 FCBTEST.COM（FCB 建/写/读/删）
  mkfat16.py         稀疏 FAT16 格式化器（32 MB .. 1.9 GB 测试卷）
  fat_extract.py     只读 FAT12/16/32 提取器（list/get/extract）
  extract_dos_file.py  按簇链从裸 FAT 镜像里取出单个文件
  qemu_probe.py      通过 QEMU monitor 驱动 UEFI shell
  qmon.py qtype.py screenshot.py ppm2png.py   QEMU monitor/键盘/截图助手
docs/                兼容性报告、DOS 4.0 语义、迁移坑、保护模式/DPMI 笔记
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

## 限制与路线

- **还没有 ring 3**：DPMI 客户仍运行在 ring 0。没有 TSS，没有 `CR3` 页表与 `#PF` 分发；
  `AX=0600h`（分页）与 `AX=0B00h`（硬件中断）是桩。
- 只支持 8.3 名（不能创建 LFN）；只有文本模式（未模拟 `INT 10h` 图形模式）。
- `REN` 只在同一目录内；`COPY` 不支持通配符；挂载的 `A:`/`B:` 镜像大小固定。
- FAT32 的 `FSInfo` 空闲簇数不写回（内核内存里维护 `free_count`）。
- 未实现 TSR 常驻（`INT 27h` 只终止程序）。
- 单文件按 DOS 4.0 语义限 32 MB（理由见 `docs/dos4-semantics.md` §4.6）。
- 路线：ring 3 + TSS/门 → `CR3` 分页 + `#PF` → DPMI 0600/0B00 → LFN 读取 → 图形模式 →
  用 DOS 2.11 时期源码重建部分工具 → 扩充兼容性矩阵。

## 许可证

MIT —— 见 [`LICENSE`](LICENSE)。

本仓库**只**分发原创源码、由自带工具生成的测试程序与文档。Microsoft MS-DOS / PC-DOS
系统文件、商业 DOS 应用与游戏、OVMF/edk2 固件、QEMU 二进制都不在此分发；请自行获取，
并不要把它们提交进本仓库。
