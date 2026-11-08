# DOS 4.0 real-mode semantics (阶段 4) — UEFI-DOS 0.5.0

阶段 4 把内核从 “能跑 DOS 2.11 程序” 扩展到 “按 DOS 4.0 的语义约束跑”。它与阶段 5
（ring 3 / 分页 / 完整 DPMI）完全解耦：全部改动都在内核 `src/kernel/dos.c`、FCB 层
`src/emu/fcb.c`、`src/emu/int21.c` 和 FAT 层 `src/fs/fat.c` 里，不涉及保护模式路径。

验证入口（不需要 QEMU，直接跑在 Windows 上）：

```
build\hosttest.exe --phase46 build\t_p46.img       # 4.1/4.2/4.3/4.4 + 6.1 双路径
build\hosttest.exe --fat16 build\fat16_32.img small # 4.4/4.5 FAT16 写路径
build\hosttest.exe --fat16 build\fat16_2g.img big   # 4.4/4.6 1.9 GB 卷 + 32 MB 文件
build\hosttest.exe --shell build\UEFIDOS.img FCBTEST  # 真实 16 位代码走 FCB
```

## 4.1 句柄扩 SHARE 字段（DENY_READ / DENY_WRITE / DENY_NONE）

`dos_handle` 新增：

| 字段 | 作用 |
| --- | --- |
| `share_mode` | `DOS_SHARE_COMPAT/DENY_RW/DENY_WRITE/DENY_READ/DENY_NONE`（AH=3Dh AL 的 bit4-6） |
| `open_mode` | 0 读 / 1 写 / 2 读写（AL 的 bit0-2） |
| `psp` | 打开者 PSP；compat 模式只与其他 **PSP** 冲突（DOS 的 compat = 跨进程独占） |
| `lock_count` / `locks[8]` | AH=5Ch 的字节区间锁 |

打开/创建统一走 `dos_open_ex(path, open_mode, share, &h)` 与
`dos_creat_ex(path, attr, share, open_mode, &h)`；老的 `dos_open`/`dos_creat` 是
`compat + r/w` 的薄包装，`g_dosapi` 与 shell 不需要改。

`share_conflict()` 的两条规则（这正是真实 DOS SHARE 的双向判定）：

1. **已打开的句柄** 的 share 模式是否禁止本次请求的访问 —— `deny_rw` 无条件拒绝，
   `deny_write` 拒绝写打开，`deny_read` 拒绝读打开。
2. **本次请求的 share 模式** 是否禁止已授予的访问 —— 新的 `deny_read`/`deny_rw` 与已存在的
   读者冲突，新的 `deny_write`/`deny_rw` 与已存在的写者冲突。

冲突时返回 DOS 错误 5（access denied）。`compat` 只与其他 PSP 的打开冲突，因此同一进程
多次以 AL=0 打开同一文件仍然成功（DOS 程序员依赖这一点）。

实测（`--phase46`）：

```
[4.1] SHARE compatibility modes
  ok   deny-read/write blocks a second open (error 5)
  ok   deny-none allows two opens
  ok   deny-write still allows a read open
  ok   deny-write blocks a write open (error 5)
```

## 4.2 DOS 4.0 默认 FILES= 与 CONFIG.SYS

DOS 4.0（以及 3.3/5.0）的默认是 **FILES=8**，其中句柄 0..4 是预定义设备
（STDIN/STDOUT/STDERR/AUX/PRN），所以默认只有 3 个空闲文件句柄。UEFI-DOS 现在：

* `DOS_HANDLE_MAX 255` 是数组上限，`g_dos.files_limit` 是**生效**上限（默认 8）；
* `alloc_handle()` 只在 `5 .. files_limit-1` 里找空位，超限返回 `FAT_EMFILE` →
  **DOS 错误 4**（too many open files，以前错报成 5）；
* 新增 `dos_read_config_sys()`，在内核挂载 C: 之后解析 `CONFIG.SYS` 的一个子集：

| 指令 | 行为 |
| --- | --- |
| `FILES=n` | 生效句柄上限（clamp 到 5..255） |
| `FCBS=m,n` | 记录 `g_dos.fcb_limit`（FCB 层本身无状态，见 4.3） |
| `LASTDRIVE=x` | 记录 `g_dos.last_drive`，clamp 到 `DOS_MAX_DRIVES-1`（当前 = D:） |
| `BUFFERS/DEVICE/SHELL/STACKS/SWITCHES/REM/;` | 接受并忽略（UEFI 层不需要可配置 BIOS） |

INT 21h **AH=67h** 同时支持（设定本进程句柄上限），实测：

```
[4.2] FILES= limit
  ok   FILES=8 yields 3 free handles (0..4 predefined)
  ok   over the FILES= limit returns error 4 (too many open files)
  ok   AH=67h BX=20 allows 8 concurrent opens
```

QEMU 冒烟测试里的镜像带 `CONFIG.SYS`（`FILES=20` 等），所以 `CONFIG` 内建命令打印
`FILES=20 ... open handles: 5 of 20`。

## 4.3 FCB 调用族（`src/emu/fcb.c`，新文件）

支持 AH=**0Fh 10h 11h 12h 13h 14h 15h 16h 17h 21h 22h 23h 27h 28h**。

设计决定：**FCB 层无状态**。真实 DOS 把驱动器/目录/簇缓存在 FCB 的保留区里，并通过
SHARE 共享打开的 FCB。UEFI-DOS 是单进程系统，所以每次调用都用 8.3 名字重新解析一次
（打开→定位→读/写→关闭）。好处是程序复制 FCB（DOS 4.0 程序确实这么干）也能工作；代价是
每条记录 I/O 多一次目录查找——对 8086 程序的速度完全够用（FCBTEST 里 128 字节记录是本机
瞬时完成）。

调用约定（这里踩过一个真实的坑）：**FCB 地址在 DS:DX**，不是 ES:DI；而 AH=14h/15h/21h/
22h/27h/28h 的**数据缓冲区是当前 DTA**（INT 21h AH=1Ah 设定），不是 DS:DX。最初实现按
ES:DI 取 FCB、把 DS:DX 当缓冲区，导致真实程序全部 0xFF 失败——已修正，并加了回归用例。

各调用语义：

| AH | 行为 |
| --- | --- |
| 0Fh / 16h | 打开已存在 / 创建（FCB 的 +0x0B 是扩展名末字节，**不是属性**；一律按 0x20 建） |
| 10h | 关闭（无状态，直接 AL=0） |
| 11h / 12h | 查找首/次：把 DTA 填成 FCB 风格记录（drive、8.3 名、属性、时间、日期、大小） |
| 13h | 通配删除（展开 `?`/`*` 后逐个 unlink） |
| 14h / 15h | 顺序读/写：位置 = 当前块×记录长 + 当前记录；块内固定 128 记录 |
| 21h / 22h | 随机读/写：位置 = 随机记录 × 记录长 |
| 23h | 文件大小 → 随机记录 = ⌈size/记录长⌉ |
| 27h / 28h | 随机块读/写，CX = 记录数，返回时 CX = 实际记录数 |

状态字节：0 成功、1 EOF（0 字节）、3 EOF（不满一条记录）、0xFF 失败。

QEMU 与 host 都跑真实 16 位代码 `FCBTEST.COM`（`tools/make_fcb_test.py` 手汇编）：
DTA 设到缓冲区 → 创建 → 写一条 128 字节记录 → 关闭 → 重开 → 随机读回 → 比较 → 删除，
打印 `FCB-OK`。这条用例同时覆盖了“CONFIG.SYS 生效 + FCB 族 + 目录写路径”。

## 4.4 FAT16 大分区与分配游标

问题：`alloc_cluster()` 原先每次都从簇 2 线性扫描，FAT16 最多 65524 簇，写一个多簇文件
整体是 O(n²)。

改动（`src/fs/fat.h` / `src/fs/fat.c`）：

* `fat_vol` 新增 `alloc_hint`（分配游标，命中后前进，到顶回绕）与 `free_count`；
* `alloc_cluster()` 从游标开始单趟扫描（最多一整圈），命中即更新游标并递减 `free_count`；
* `fat_free_chain()` 每释放一簇 `free_count++`；`free_count == 0` 直接返回 `FAT_ENOSPC`，
  不再白扫；
* `fat_mount()` 末尾一次性统计空闲簇（`count_free()`）。FAT16 最坏 65524 次 FAT 读取，
  在 8 槽 FAT 扇区缓存下只有几十次真实 I/O。

工具 `tools/mkfat16.py` 生成 FAT16 测试卷（MBR + 单分区，数据区是稀疏空洞，1.9 GB 映像
在 NTFS 上几乎不占空间）：

```
python tools\mkfat16.py build\fat16_32.img  --size-mb 32   --spc 4    # 15825 簇, 2 KB/簇
python tools\mkfat16.py build\fat16_2g.img  --size-mb 1900 --spc 64   # 60759 簇, 32 KB/簇
```

实测（32 MB 卷）：

```
volume: FAT16, 15825 clusters, 15825 free, spc=4
create 100 x 8192 B: 0 ms
ok   created every test file on FAT16
ok   read back a FAT16 file byte-for-byte
delete 100 files: 0 ms
recreate 100 files: 16 ms
ok   recreated the files after deletion
```

## 4.5 FAT12/16 写路径

`set_fat_entry()` 一直有 12/16 位写分支，但此前只被 FAT12（2.11 软盘映像）的写用例覆盖。
现在 FAT16 的创建/写/读回/删除/重建都有用例（上节），FAT12 由 `run_tests.ps1` 里的
“FAT12 write on 2.11 image” 覆盖。FAT12/16 的**固定根目录**不做增长（DOS 也是定长 112/224
项），目录满返回 `FAT_EDIRFULL` → DOS 错误 0x27。

## 4.6 32 MB 文件上限的决策

结论：**保持 `u32` 大小/偏移，按 DOS 4.0 语义把 32 MB 当作实测上限**，不升 `u64`。

理由：

1. DOS 的文件 API 天然是 32 位：INT 21h AH=42h 的 CX:DX 偏移、FCB 的文件大小/随机记录、
   DTA 里的 4 字节大小，都是 32 位。升 `u64` 只会让内核比 DOS 更宽，却又没有 DOS 程序能用。
2. FAT16 单文件上限由簇数决定（65524 × 32 KB ≈ 2 GB），FAT32 才是真正的大文件路径；
   与 “DOS 4.0 兼容” 目标相关的是 DOS 侧语义，不是卷格式上限。
3. 现有 `dos_read/write/lseek` 全是 `u32`，一次改动会波及 DPMI 反射、dosapi 与 FCB 层；
   收益是“宣传支持大文件”，而当前没有程序需要。

实测在 1.9 GB FAT16 卷上写 32 MB 单文件（即决策所依据的边界值）：

```
volume: FAT16, 60759 clusters, 60759 free, spc=64, free MB=1898
wrote 33554432 bytes in 156 ms
ok   wrote a 32 MB file through the u32 file-size path
ok   directory entry reports the full 32 MB size
ok   freed the 32 MB chain
delete 32 MB file: 16 ms, free clusters now 60759
```

若将来要做 2 GB 以上文件，需要同时改：`dos_handle.size/pos`、AH=42h 的 whence 处理、
FCB 随机记录、`g_dosapi` 的 read/write/seek 签名（并新增 API 版本 0x0200），以及
FAT32 的 `fat_write_chain` 偏移类型。

## 阶段 6.1 双路径一致性（INT 21h ↔ 原生 API）

`--phase46` 最后一段把**同一操作**分别走 INT 21h（寄存器级调用真实 `int21_handler`）与
`g_dosapi`，逐项比较：

| 操作 | 断言 |
| --- | --- |
| 创建 | 两条路都返回有效句柄 |
| 写入 | 两条路返回相同字节数 |
| 定位 | 两条路对新位置一致 |
| 读回 | 两条路返回**逐字节相同**的内容 |
| 查找 | INT 21h 的 DTA 路径与 `find_first` 的文件大小一致 |
| 错误：文件不存在 | 都是 DOS 错误 2 |
| 错误：删只读文件 | 都是 DOS 错误 5 |

```
[6.1] INT 21h vs native API consistency
  ok   create: both paths return a handle
  ok   write: both paths report the same byte count
  ok   seek: both paths agree on the new position
  ok   read: both paths return identical bytes
  ok   find first: INT 21h DTA path and native find_first agree on size
  ok   missing file: both paths return DOS error 2
  ok   read-only delete: both paths return DOS error 5
```

顺带修掉的真实 bug（都由这轮测试暴露）：

1. `dos_fat_name(name, out, ok)` 传 `ok = NULL` 会写空指针（`*ok = 1`）——FCB 找文件时崩溃。
2. FCB 调用把 FCB 当成 ES:DI、把记录缓冲区当成 DS:DX（见 4.3）。
3. `dos_unlink()` 不检查只读属性，删只读文件成功（DOS 必须报 5）。
4. `fat_create_file()` 创建后紧接的重新解析会失败（返回错误 2，文件却已建好）：
   已改为创建后直接使用调用方传入的 parent/name，不再重复解析。
5. `mkfat16.py` 早期版本的 BPB 只写 `total_sectors16`，1.9 GB 卷必须写 `total32`。

## 阶段 5 的现状

阶段 5（TSS + 门 + 跨 ring 切栈、CR3 页表 + #PF、DPMI 分页/硬件中断服务）**本轮未实现**。
当前 DPMI host 是 “ring 0 单特权级 + 分段” 模型：客户程序进入保护模式后仍运行在 ring 0，
`int31_handler` 的 0500h 内存池从 0x1000 段起分配，0300h 反射不处理硬件中断。阶段 5 需要
先落地 5.1（TSS/门/栈切换），否则 5.2/5.3 无从验证。
