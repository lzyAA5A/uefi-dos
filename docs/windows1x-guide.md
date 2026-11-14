# Windows 1.01 使用指南（在 UEFI-DOS 上）

这份指南讲的是**跑起来之后怎么用**：鼠标、双击、菜单、文件列表、退出，以及常见问题。
安装 / 起机步骤见 `docs/windows1x.md` 与 `run_win1x.ps1`。

---

## 1. 先确认三件事

| 项目 | 要求 | 怎么确认 |
| --- | --- | --- |
| UEFI-DOS 版本 | 1.1.18 或更新 | 开机横幅 `UEFI-DOS vX.Y.Z`，或命令行 `VER` |
| Windows 目录树 | 用 **总线鼠标安装** 的那一份（推荐 `F:\1231\_tmp\win\w101mouse`） | `WINDOWS\WIN100.OVL` 里能找到 `Bus/Serial` |
| 字体齐全 | `COURD.FON`、`HELVD.FON`、`TMSRD.FON` 必须在 `WINDOWS\` 里 | `DIR WINDOWS\*.FON` |

为什么这三件事重要：

- Windows 1.x **不使用** DOS 的 `INT 33h` 鼠标驱动。它的指点设备驱动是 **安装时** 由
  `SETUP` 链接进 `WIN100.OVL` 的。用"没有指点设备"装出来的树，无论怎样都出不来指针。
- 缺 `COURD.FON`（或 `HELVD.FON`/`TMSRD.FON`）时，桌面出来了也会弹
  `Change Disk — Insert COURD.FON disk in drive A:`，然后自己退出。

启动方式（任选其一，都在 `F:\1231\UEFI-DOS` 里执行）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\runwin1.bat
# 或
powershell -NoProfile -ExecutionPolicy Bypass -File .\run_win1x.ps1 -Installed F:\1231\_tmp\win\w101mouse
```

---

## 2. 鼠标

1. **先点一下 QEMU 窗口内部**，把鼠标"抓"进虚拟机（窗口标题会提示 grab 状态）。
   不抓的时候，指针属于 Windows 桌面（宿主），虚拟机里的 Windows 收不到移动与按键。
2. 释放鼠标：`Ctrl+Alt+G`。
3. 指针是 Windows 自己的 CGA 光标（中间那个带方框的箭头是"忙"光标，说明 Windows
   正在做磁盘操作，等一两秒它自己会变回普通箭头）。
4. 指针速度：写进 `C:\CONFIG.SYS`

   ```ini
   MOUSESPEED=2      ; 1 = 最快（约原来的两倍），2 = 默认，4 = 慢一半
   ```

   改完重新起机生效。老版本请在命令行临时用宿主环境变量 `UEFIDOS_MOUSE_SCALE`。

---

## 3. 桌面（MS-DOS Executive）

起机后看到的就是 **MS-DOS Executive**，它是 Windows 1.01 的文件管理器兼"桌面"：

```
 MS-DOS Executive                                  ← 标题栏（左上角方块 = 系统菜单）
 File  View  Special                               ← 菜单栏
 A ▭   C ▭▭   D ▭      C: \WINDOWS                 ← 驱动器按钮 + 当前路径
 PIF        PAINT.EXE      WRITE.EXE               ← 文件列表（多列）
 ABC.TXT    PRACTICE.DOC
 ...
                                                   ← 底部灰色抖动区 = 桌面空区，可以把窗口拖过去
```

- **单击**：选中一个条目（反白）。
- **双击**：打开 —— 目录会进去，`.EXE`/`.COM`/`.BAT` 会运行，`.PIF` 会按 PIF 设置运行程序。
- 双击 `.FON`、`.TXT` 这类不可执行文件，Windows 会弹
  `MS-DOS — Cannot run XXX` 并给一个 `Ok` 按钮，这是正常的。
- 窗口**不能重叠**（Windows 1.x 的平铺模型）：打开的窗口会互相挤占，可以被拖到
  底部灰色区里当作"最小化"。

### 菜单

| 菜单 | 命令 |
| --- | --- |
| **File** | `Run...`（输入命令运行）、`Load...`（把程序装进内存备用）、`Copy...`、`Get Info`（选中项的信息）、`Delete...`、`Print...`、`Rename...` |
| **View** | `Short`（短格式）/ `Long`（长格式：大小、日期）/ `Partial...`（按通配符筛）/ `By Name` / `By Date` / `By Size` / `By Kind`（排序） |
| **Special** | `End Session`（结束 Windows 会话）、`Create Directory...`、`Change Directory...`、`Format Data Disk...`、`Make System Disk...`、`Set Volume Name...` |
| 系统菜单（标题栏左上角小方块） | `About...`（版本信息） |

用法（Windows 1.0 的菜单是**按住不放**才展开的，和我们后来习惯的"点一下就弹开"不同）：

1. 把指针移到菜单标题（`File` / `View` / `Special`）上，**按住**左键 —— 菜单立刻展开；
2. 保持按住，把指针拖到要用的命令上（高亮跟随），然后**松开**左键执行；
3. 想取消：按住时把指针拖到菜单外面再松开。

只"点一下立刻松开"的话，菜单会展开又收起，看起来像什么都没发生 —— 这是 1.0 的正常行为，
不是鼠标坏了。菜单项右侧带 `...` 的会弹对话框。

### 键盘

- 方向键：在文件列表里移动选中项（`↑` `↓` `←` `→`）。
- `Enter`：**不会**打开选中项 —— Windows 1.01 的列表控件只认双击。
  想用键盘启动程序，请用 **File → Run...**，输入 `CALC` 之类再回车。
- 所有对话框都能用 `Tab` 在按钮之间切换、`Enter` 确认、`Esc` 取消。

---

## 4. 附带的程序

`WINDOWS\` 里这一批都可以双击运行：

| 程序 | 用途 |
| --- | --- |
| `CALC.EXE` | 计算器（标准 / 科学） |
| `CALENDAR.EXE` | 日历与约会（我们验证过它能正常打开） |
| `CARDFILE.EXE` | 卡片文件（小型数据库） |
| `CLIPBRD.EXE` | 剪贴板查看器 |
| `CLOCK.EXE` | 时钟（模拟/数字） |
| `CONTROL.EXE` | 控制面板：颜色、字体、鼠标双击速度、打印机、端口… |
| `NOTEPAD.EXE` | 记事本（纯文本，`.TXT`） |
| `PAINT.EXE` | 画笔 |
| `REVERSI.EXE` | 黑白棋 |
| `TERMINAL.EXE` | 终端（串口通讯） |
| `WRITE.EXE` | 书写器（带格式的文档，`.WRI`） |

`PIF\` 目录里是给 DOS 程序用的 `.PIF` 启动配置。

---

## 5. 退出

- 关掉自己开的窗口（窗口左上角的系统菜单里有 `Close`）。
- 在 Executive 里选 **Special → End Session**，确认后回到 DOS 提示符。
- 直接关机也可以，但那样 `WIN.INI` 里的改动不会保存。

---

## 6. 常见问题

| 现象 | 原因 / 处理 |
| --- | --- |
| 指针完全不动 | 没点进 QEMU 窗口（鼠标没被抓）；或者这棵树是"没有指点设备"装出来的（见第 1 节） |
| 双击没反应，只是把行选中了 | 1.1.17 及更早版本存在这个问题（模拟器的定时器按"指令数"而不是真实时间走，Windows 的 500ms 双击窗口在两次按键之间就过去了）。1.1.18 起已按真实时间校准，正常速度的双击即可 |
| 双击很吃力 | 临时办法：`WINDOWS\WIN.INI` 的 `[windows]` 段把 `DoubleClickSpeed=500` 改大（例如 `2000`），或在 Control Panel 里调双击速度 |
| 弹出 `Change Disk — Insert COURD.FON disk in drive A:` | `WINDOWS\` 缺字体，把 1.01 的 17 个 `.FON` 补齐 |
| 一会儿自己退出，回到 `C:\WINDOWS>` 并打印 `FatalExit code = stack overflow` | 已知的遗留问题，正在查（`docs/windows1x.md` 有全部证据） |
| 画面上下有黑边 / 比例被压扁 | 1.1.11 起按 4:3 输出，不会再压缩；若仍有黑边说明 GOP 模式比 4:3 更宽，属正常留边 |
| 提示 `Not enough memory to display entire directory` | 目录太大（文件太多）。先用 `View → Partial...` 过滤，或把不用的文件移出目录 |
| 想换机器/换树 | 换掉 `-Installed` 指向的目录即可，前提仍是那份目录里 `WIN100.OVL` 带鼠标驱动、字体齐全 |

---

## 7. 一句话总结

**点一下窗口抓住鼠标 → 单击选中、双击打开 → 菜单用鼠标点 → 退出用 Special → End Session。**
键盘只能移动选中项，启动程序请用 `File → Run...`。
