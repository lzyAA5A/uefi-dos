# 迁移指南与坑清单（阶段 6.2）

把 16 位 DOS 程序搬到 UEFI-DOS、或把程序从 INT 21h 迁到原生 32/64 位 API
（`src/api/dosapi.h`，`g_dosapi_exports`）时最常踩的五个坑。每条都给最小复现。

---

## 坑 1：far 指针 —— 段:偏移 ≠ 线性地址

16 位侧的指针是 `seg:off`（物理 = `seg<<4 + off`），原生 API 侧的指针是真指针
（64 位下 8 字节）。两者**不能互传**。

异常 1：`dosapi` 里所有路径参数都是 `const char *`（宿主线性指针），不能把
`DS:DX` 直接转成指针丢进去。

异常 2：INT 21h AH=4Bh（EXEC）的参数块里，命令行的段值为 0 表示“沿用调用者的 DS”。
传一个非 0 段就等于要求对方用你的段——DPMI 客户程序经常在这里写错。

```c
/* 错误：把 16 位段值当指针 */
dosapi_open((const char *)0x0D00, 0, &h);      /* 打开的是线性 0x0D00 的垃圾 */

/* 正确：先把自己手里的 seg:off 拷进一段本地缓冲区 */
char path[64];
memcpy(path, MK_FP(seg, off), sizeof(path) - 1);
path[sizeof(path) - 1] = 0;
dosapi_open(path, 0, &h);
```

```asm
; 错误：EXEC 参数块里给了调用者的段（子程序读不到命令行）
        mov     word ptr [pb+4], ds
; 正确：0 表示“使用调用者的 DS”，DOS 语义如此，UEFI-DOS 照此实现
        mov     word ptr [pb+4], 0
```

验证：`UPCALL.COM`（INT 2Fh AX=5544h 探测）与 `RUNNER.COM`（EXEC）都在
`run_tests.ps1` 里；后者考的就是参数块。

---

## 坑 2：DTA 是全局单例

INT 21h 的 DTA（Disk Transfer Area）是**全进程唯一**的：
`AH=1Ah` 设定，`AH=11h/12h/4Eh/4Fh` 写入，**FCB 的记录 I/O（14h/15h/21h/22h/27h/28h）
也从 DTA 取/放数据**。嵌套调用（EXEC、shell 内建命令、TSR）必须自己存/恢复。

UEFI-DOS 的做法：`emu_exec_program()` 在派生之前快照 `g_dos.dta_seg/dta_off` 与整张句柄表，
给子进程设 DTA = `psp:0x80`，子进程退出后原样恢复。

```c
/* 错误：设了 DTA 就去 EXEC，回来继续用旧 DTA —— 已被子进程换掉 */
dos_set_dta(my_seg, my_off);
system("CHILD");
find_next(&f);              /* DTA 现在是子进程留下的 psp:0x80 */

/* 正确：EXEC 前后自己保存/恢复（或重新 AH=1Ah） */
u16 s = get_dta_seg(), o = get_dta_off();
system("CHILD");
dos_set_dta(s, o);
```

FCB + 查找混用时同样要重设 DTA：`AH=11h` 会把 DTA 填成 FCB 记录格式，
`AH=4Eh` 填的是 43 字节的 DOS 4.0 格式，两者**不兼容**。

验证：`--phase46` 的 4.3 段（FCB 记录 I/O 走 DTA）、`DTATEST.COM`（默认 DTA = psp:0x80）。

---

## 坑 3：console 语义 ≠ 字节流

INT 21h 的字符输出有三套语义，原生 API 的 `write(1, ...)` 只有一套：

| 入口 | 语义 |
| --- | --- |
| `AH=02h` | 写一个字符，`DL`；`\n` 不自动补 `\r` |
| `AH=09h` | 写 `DS:DX` 起、到 `'$'` 为止的字符串（`$` 不输出） |
| `AH=40h` / 原生 `write` | **二进制**：写 `CX` 字节，不做 `$` 终止、不做 `\n` 转换 |
| `AH=0Ah` | 行输入，缓冲区格式 `[max][len][data...]`，带回显 |

原生 API 的 `read`/`write` 对句柄 0/1 同样是二进制，所以把 `AH=09h` 的字符串直接丢给
`write` 会把 `$` 也写出去，把 `printf` 风格的字符串交给 `AH=40h` 会写出 NUL。

```c
/* 错误：把 '$' 结尾的串按长度写给控制台 */
dosapi_write(1, msg, strlen(msg) + 1, &n);   /* NUL 也被写出去了 */

/* 正确 */
dosapi_write(1, msg, strlen(msg), &n);
```

验证：`--phase46` 的 6.1 段用 `write` 写 17 字节并逐字节比对。

---

## 坑 4：tick —— 别假设“读两次就会变”

DOS 的 tick 来自 8253/8254 的 18.2 Hz 中断（`0040:006C`，INT 1Ah AH=00 读 CX:DX）。
UEFI 侧没有 PIT 中断，`plat_ticks_ms()` 用 TSC（一次校准，参考 `Stall(50ms)`），
`int1a_handler` 与 `emu_kbd_pump` 会同步刷新 `0040:006C`。

历史坑：早期实现用 `GetNextMonotonicCount`，但 EDK2 的实现是“每调一次加一”，
校准测得 1 unit/ms，于是所有等待循环变成瞬间通过，`MINE` 显示 `TIME 65798144`。
换成 TSC 之后正常。

```c
/* 危险：同一毫秒内两次读相等 → 死循环或瞬间通过 */
do { t = plat_ticks_ms(); } while (t == last);

/* 安全：等待目标时刻，而不是等待“值变化” */
u64 deadline = plat_ticks_ms() + 100;
while (plat_ticks_ms() < deadline) ;
```

验证：`TICKS-OK`（`--shell` 用例里断言）、`CP86TEST.COM` 的 RTC/BIOS tick 子测试。

---

## 坑 5：32 位偏移的两种“错位”

1. **INT 21h AH=42h 是 CX:DX**（高:低），不是 DX 单寄存器；`whence=1` 的相对偏移是
   **有符号**的，负偏移必须用补码给出。原生 API `seek(handle, i32 off, u8 whence, u32 *newpos)`
   的 `off` 同样是 `i32`，所以“往回退 4 KB”要写 `-4096` 而不是 `0xFFFFF000` 的 `u32` 语义。
2. **FCB 的随机记录是“记录数”不是字节偏移**：`AH=21h` 的位置 = 随机记录 × 记录长，
   `AH=23h` 返回的是 ⌈size / 记录长⌉。把它当字节偏移会让程序读到文件尾部之外。

```c
/* 错误：把 -100 当无符号数传进去（会跑到 4 GB 附近） */
dosapi_seek(h, (i32)0xFFFFFF9C, 1, &pos);

/* 正确 */
dosapi_seek(h, -100, 1, &pos);

/* FCB：想读第 3 条记录（记录长 128），随机记录写 3，不是 384 */
```

文件大小/偏移在 UEFI-DOS 里是 `u32`，因此**单文件上限按 DOS 4.0 语义取 32 MB**
（1.9 GB FAT16 卷上写 32 MB 文件实测通过，见 `docs/dos4-semantics.md` 4.6）。

验证：`--phase46` 的 6.1 段（seek 一致性）、`--fat16 ... big`（32 MB 文件）、
`AH=42h` 的负偏移由 shell `TYPE`/`COPY` 的续读路径覆盖。

---

## 附：迁移时的检查清单

- [ ] 路径字符串：`seg:off` → 线性缓冲区，**不要**强转指针
- [ ] DTA：EXEC / 查找 / FCB 前后自己保存恢复
- [ ] 输出：确认走 `AH=09h`（`$` 终止）还是 `write`（二进制长度）
- [ ] 时间：用 deadline 而不是“值变化”；不要假设 tick 精度优于 1 ms
- [ ] 偏移：AH=42h 的 CX:DX + 有符号相对偏移；FCB 用记录号
- [ ] 句柄数：默认 `FILES=8`（3 个空闲），需要更多就在 `CONFIG.SYS` 里加 `FILES=` 或调 AH=67h
- [ ] 一致性：同一操作若 INT 21h 与原生 API 都实现，用 `--phase46` 的方式双向断言
