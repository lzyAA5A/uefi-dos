# 大文件：32 MB 是测试边界，不是天花板（UEFI-DOS 1.7.0）

本文记录 1.7.0 里"突破 DOS 4.0 的 32 MB 文件上限"这一条到底做了什么、边界在哪、
以及超过文件系统上限时用什么办法绕（切割成 `NAME.001`、`NAME.002` …，由系统当
一个流读出来）。

## 1. 先纠正一个说法

`docs/dos4-semantics.md` 4.6 原先写的是：

> 结论：**保持 `u32` 大小/偏移，按 DOS 4.0 语义把 32 MB 当作实测上限**，不升 `u64`。

那句话描述的是 **1.x 早期"测到 32 MB 就收工"的验证边界**，不是代码里的限制。
真正的限制有三层，而且都不是 32 MB：

| 层 | 上限 | 原因 |
| --- | --- | --- |
| FAT 目录项 | **4 GB − 1** | 目录项里文件大小是 32 位字段，这是格式上限 |
| INT 21h AH=42h（LSEEK） | 2 GB（签名偏移）/ 4 GB−1（当无符号用） | `CX:DX` 是 32 位偏移，DOS 语义按有符号解释 |
| INT 21h AH=3Fh/40h | 单次 `CX` 字节，偏移 32 位 | 句柄里的位置就是 `u32` |
| **UEFI-DOS 实现** | **到 FAT 格式上限为止，没有额外上限** | `dos_handle.pos/size` 就是 `u32`，实测能到 4 GB−1 |

所以 1.7.0 的"突破"分两步走：**先把 32 MB 这个说法用实测打到格式上限**，
再**给 16 位程序一套 64 位偏移的 API**（`INT E0h`），超过 4 GB 的流则用
**切割 + 自动拼接**。

## 2. 实测（`--fat16 … big`）

```
python tools/mkfat16.py build\fat16_2g.img --size-mb 1900 --spc 64
set UEFIDOS_BIG_MB=1500
build\hosttest.exe --fat16 build\fat16_2g.img big
```

`UEFIDOS_BIG_MB` 决定测试文件多大，默认 32（老边界），可以给 64 / 200 / 1500。
脚本会：

1. 用**普通 DOS 调用**（`dos_creat`/`dos_write`/`dos_read`/`dos_lseek`）写满这个大小；
2. 用**目录项**核对大小；
3. 在 `size/2`、`size-4096`、`33 MB` 处定位读回并逐字节比对（模式由绝对偏移生成，
   所以任何位置都能验证）；
4. 再用 **`INT E0h` 大文件层**打开同一个文件，核对大小、`size/3`、末尾读回、seek-to-end；
5. 最后造一个 **三段切割文件**（`SPLIT.DAT.001` 1 MB + `.002` 1 MB + `.003` 512 KB），
   用大文件层把它当 **2.5 MB 一个流**读，跨段边界逐字节验证，并确认拼接流是只读的。

实测结果（host 测试台，NTFS 上的镜像文件，1.9 GB FAT16 卷 / 32 KB 簇）：

| 文件大小 | 写入耗时 | 读回校验 |
| --- | --- | --- |
| 64 MB | 0.44 s（150 MB/s） | 半程 / 末尾 / 33 MB 处全部一致 |
| 200 MB | ~2 s | 同上 |
| 1500 MB | 141.9 s（10.8 MB/s） | 半程 / 末尾 / 33 MB 处一致，删除 908 ms 后 free 回到 60759 |

结论：**32 MB 从来不是限制**。一个 1.5 GB 的文件在 FAT16 1.9 GB 卷上完整写入、
目录项大小正确、任意偏移读回一致，删除后簇全部回收。

## 3. 新 API：INT E0h（UEFI-DOS 扩展服务）

DOS 的 INT 21h 一个字都不改（Windows 1.x、EXEPACK、Turbo Link 全都对 32 位大小
做算术，改了就会坏）。64 位偏移另开一条向量，带一个请求块：

```
INT E0h
  AX = FD00h            探测：返回 AX=5941h，BX=4455h('UD')，CX=能力位，DX=版本(0107h)
  AH = FDh, AL = 功能号, ES:DI -> BFR（大文件请求块，40 字节）
```

BFR 布局（小端）：

| 偏移 | 大小 | 字段 |
| --- | --- | --- |
| +0 | qword | `offset` — READ/WRITE/SEEK 输入；SEEK/SIZE 输出 |
| +8 | dword | `count` — 输入想要的字节数，输出实际传输 |
| +12 | word | `handle` — OPEN 输出，其余输入 |
| +14 | word | `status` — 0 = 成功，否则 DOS 错误码 |
| +16 | dword | `buf_off` — 数据缓冲的远指针偏移 |
| +20 | word | `buf_seg` — 数据缓冲的段 |
| +22 | word | `flags` — OPEN：bit0 = 不拼接 `NAME.001` 段，只开这一个文件 |
| +24 | word | `whence` — SEEK：0 绝对、1 相对、2 相对末尾（偏移按补码解释） |
| +28 | dword | `size_lo` — 输出总大小低 32 位 |
| +32 | dword | `size_hi` — 输出总大小高 32 位 |
| +36 | dword | `parts` — 输出这个流由几个文件拼成 |

功能号：

| AL | 功能 | 说明 |
| --- | --- | --- |
| 01h | OPEN | `DS:DX` = 路径（ASCIIZ）。若 `NAME.001` 存在则自动把 `NAME.001/.002/…` 拼成一个流 |
| 02h | READ | 按 `offset` 读，`count` 不能跨 64 KB 远指针边界也安全（段内回绕，与 DOS 一致） |
| 03h | WRITE | 只对单文件流有效；拼接流是只读的，返回错误 5 |
| 04h | SEEK/TELL | 维护一个流内游标，`offset` 返回新位置 |
| 05h | CLOSE | |
| 06h | SIZE | 返回大小、高低 32 位、段数 |

成功清 CF，失败置 CF 并把 DOS 错误码放进 AX 和 `BFR.status`。

## 4. 切割（"实在不行"的那条路）

FAT32 的单文件上限是 **4 GB − 1**，这是目录项 32 位字段决定的，改代码也突破不了。
超过 4 GB 的流用切割：`NAME.001`、`NAME.002`、… 每段各自 ≤ 4 GB−1，
系统在打开 `NAME` 时把连续编号的段拼成一个流（最多 100 段）。

约定：

* 除最后一段外，每段大小必须相同（`SPLIT` 生成的就是这样）；最后一段可以短。
* 拼接流**只读**（字节分散在多个文件里）；单文件流可写。
* 拼接是按"连续编号"判断的：`NAME.001` 存在就继续找 `NAME.002`，遇到缺失即止；
  如果短段后面还有段，视为损坏（`ECORRUPT`），不猜。

DOS 侧的两个命令：

```
SPLIT BIG.ISO [段大小KB]     把 BIG.ISO 切成 BIG.ISO.001/.002/…（默认 32 MB 一段）
JOIN  BIG.ISO [输出文件]     把 BIG.ISO.001/.002/… 拼回一个文件
```

`SPLIT` 默认按 **32 MB** 切，正是老边界值：这样每一段都能被任何 DOS 读，
而新版 UEFI-DOS 又可以把它们当一个流处理。

## 5. 内核 C API（给原生程序）

`src/kernel/dos.h`：

```c
int  dos_big_open(const char *path, int flags, int *bh, u64 *size);
int  dos_big_read(int bh, u64 off, void *buf, u32 len, u32 *got);
int  dos_big_write(int bh, u64 off, const void *buf, u32 len, u32 *put);
int  dos_big_size(int bh, u64 *size);
int  dos_big_seek(int bh, u64 off, int whence, u64 *newpos);
int  dos_big_tell(int bh, u64 *pos);
int  dos_big_parts(int bh);
int  dos_big_close(int bh);
```

`flags`：bit0 = 不拼接（只开指名文件），bit1 预留读。

实现要点：大文件句柄**不常驻打开所有段**，只在读/写落在某段时把那一段打开
（`big_activate`），所以 100 段也只占一个 DOS 文件句柄。

## 6. 顺手修掉的真实 bug：长文件名删除失败 → 簇链泄漏

做切割文件时踩到的：`dos_unlink` / `dos_rename` 把**调用者拼写**重新算成 8.3 名
再交给 `fat_delete_file`。长文件名（`SPLIT.DAT.001`）在磁盘上实际是别名
`SPLIT~1.DAT`，重新算出来的名字根本不存在，于是：

* 删除"成功"返回但**没删掉**（错误码被调用方忽略）→ 目录项还在；
* 目录项里那串簇**永远不释放** → 每跑一轮泄漏 80 个簇，多跑几轮卷就"烂"了
  （后续写入的簇链标记会读到脏状态）。

修法：删除/改名一律用**解析出来的目录项自带的 8.3 名**（`d.raw[0..10]`），
不再从输入重新推导。验证：同一个 1.9 GB 卷连跑三轮，`free clusters` 三轮都是
60759，不再累积。

## 7. 测试入口

```
build\hosttest.exe --fat16 build\fat16_2g.img big     # 32 MB（默认）
set UEFIDOS_BIG_MB=1500
build\hosttest.exe --fat16 build\fat16_2g.img big     # 1.5 GB
build\hosttest.exe --itest                            # 指令级回归（90 项）
```

调试开关（host 测试台）：

* `UEFIDOS_TRACE_BIG=1` — 每个失败的大文件子步骤一行（段号、段内偏移、错误码）
* `UEFIDOS_TRACE_CHAIN=1` — 读失败时打印 `fat_read_chain_byte` 的入参与退出状态；
  同时打开的话，切割测试会保留 `SPLIT.DAT.00x` 并打印每个段的簇链
* `UEFIDOS_TRACE_FATW=1` — 每次 `set_fat_entry` 写一行到 `build/fatw.txt`

## 8. 已知问题 / 下一步

1. **链式查找是 O(n²)**：`fat_write_chain` / `fat_read_chain_byte` 每次都从链头走，
   所以 1.5 GB 写入要 140 s（64 MB 只要 0.37 s）。下一步给 `fat_vol` 加一个
   按 `(首簇)` 键的链位置缓存，顺序 I/O 就变成 O(n)。
2. **被老 bug 弄脏的卷**：如果卷上已经有泄漏的簇链（旧版删长文件名失败的后果），
   在那种卷上新写入的文件可能呈现"大小对、链断"的假象。用
   `tools/mkfat16.py` 重新格式化即可；这也说明这是老状态的残留，不是新代码的路径。
3. **FAT 镜像副本**：卷上有两份 FAT，UEFI-DOS 只更新第一份（FAT#2 保持出厂值）。
   读自己写的盘没问题，但把盘交给别的系统前要注意。
4. **> 4 GB 单文件**：格式不允许。要么切割（本页 §4），要么引入 exFAT——那是
   另一个完整驱动的工作量。
