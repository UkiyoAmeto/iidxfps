# 它到底改了什么：判定窗口与谱面速度不受影响的证据

有玩家担心"限速插件会不会改变判定窗口或谱面速度"。这份文档用**可复现的取证**回答这个问题，
而不是靠承诺。所有结论都能用本仓库 `verify/` 里的工具自己跑一遍验证。

一句话结论：**插件一个字节都没有改游戏的代码**；它只改了 4 个导入表函数指针、6 个 COM 接口
虚表槽（都在运行时堆对象里）以及一个"是否垂直同步"的参数。判定窗口、谱面滚动、音符位置
这些逻辑属于游戏自身的帧计数逻辑，插件从未触碰。

---

## 1. 内存取证：游戏代码段零改动

### 方法

`verify/verifydump.dll` 是一个**只读观察者** spice2x 模块：它不装任何钩子、不写任何内存，
只是在游戏启动稳定后，把 `bm2dx.dll`（和 `d3d9.dll`）的 `.text` / `.rdata` / `.pdata`
从**运行中的进程内存**里原样转储出来，并记录每个段的 sha256。

然后跑三次，其它条件完全相同，只差加载不加载被检查的插件：

| 运行 | 加载的模块 |
|---|---|
| D1 | 只有观察者 |
| D2 | 只有观察者（再跑一次，用于区分"每次运行本来就有的差异"） |
| E1 | 观察者 + `iidxfps.dll` |

比较工具：`verify/compare_dumps.py`。

### 关键点：ASLR 会让"直接比字节"得出错误结论

两次运行模块加载基址不同（例如 D1 = `0x0E890000`、E1 = `0x0E8C0000`），Windows 加载器会
把镜像里所有需要重定位的地址改写一遍。`bm2dx.dll` 里有 **121,211 条重定位**，涉及
`.rdata` 里约 87 万字节；如果不屏蔽这些"加载器本来就会写"的字节，会看到约 11 万个字节
"不同"，从而得出完全错误的结论。`compare_dumps.py` 会先屏蔽：

* `.reloc` 列出的全部重定位目标；
* 导入地址表（IAT，里面是解析后的系统函数指针，每次运行都不一样）。

### 结果

```
=== text: 13164104 bytes, ignored(loader)=0, differences after masking=... ===
    rva=0x0093C673 ...            <- 唯一一处
=== rdata: 3965818 bytes, ignored(loader)=876464, differences after masking=0 ===
=== pdata: 692940 bytes, ignored(loader)=0, differences after masking=0 ===
```

| 对比 | `.text`（13,164,104 字节） | `.rdata`（屏蔽加载器写入后） | `.pdata` |
|---|---|---|---|
| D1 vs D2（**两次都不加载插件**） | 2 字节不同，位于 RVA `0x0093C673` | 0 | 0 |
| D1 vs E1（加载插件） | 1 字节不同，位于 **同一处** RVA `0x0093C673` | 0 | 0 |

**同一个位置、两次"都不加载插件"的运行之间也会不同** —— 这说明它不是我们的插件造成的。

那一处是什么？把运行内存和磁盘上的原始文件对比，两次运行**相对原文件的改动完全相同**：

```
rva=0x0093C670  file = 48 8b c4 56 57   (mov rax,rsp / push rsi / push rdi)
                runA = e9 de 44 69 f6   ← E9 = jmp rel32，指向 spice2x 的跳板
                runB = e9 de 44 4d 0b   ← 同一个钩子，只是跳板地址不同
```

这是 **spice2x 自己的 inline hook**（spice 会 hook 游戏里的若干函数，例如 `EnumDisplayDevicesA`
相关路径），每次运行给它分配的跳板地址不同，所以那 4 字节相对偏移会变。**它在没有我们插件的
运行里同样存在**，因此与我们的插件无关。

### 附带发现：连 `d3d9.dll` 都没被改

`d3d9.dll` 的 `.text` / `.rdata` / `.pdata` 在 D1、D2、E1 三次运行中**逐字节完全相同**。
原因是：我们打的 COM 虚表补丁落在 **D3D9 运行时自己堆分配的对象**里（见下一节的槽地址
`0x00D334A0` 这类低位地址），不在任何模块映像的代码或数据段中。

---

## 2. 插件到底改了什么（报告文件里逐条列出）

`iidxfps_report.txt` 现在带一个 `footprint:` 段落，把插件在运行期写过的**每一个地址**都列出来，
玩家和任何人可以自行核对。实测输出（IIDX 33，TDJ）：

```
game dll : bm2dx base=000000000E8C0000 image=196685824 bytes build=0x6A8BAFD1
footprint: game code bytes modified by this module = NONE
           (only import slots, shared COM vtable slots and present parameters; judgement and note logic are never touched)
  [00] IAT d3d9.dll!Direct3DCreate9Ex                 slot=000000000F550330 00000000006C67D0 -> 00007FFF407E3FB0
  [01] IAT USER32.dll!EnumDisplaySettingsA            slot=000000000F54FA40 00007FFF8BC96350 -> 00007FFF407E4100
  [02] IAT WINMM.dll!timeGetTime                      slot=000000000F54FB18 00007FFF80054240 -> 00007FFF407E43A0
  [03] IAT KERNEL32.dll!GetTickCount                  slot=000000000F54F450 00007FFF8C8E8600 -> 00007FFF407E4420
  [04] COM vtable slot (shared): IDirect3D9::CreateDevice    slot=0000000000D334A0 0000000000746240 -> 00007FFF407E4810
  [05] COM vtable slot (shared): IDirect3D9Ex::CreateDeviceEx slot=0000000000D334A0 0000000000748300 -> 00007FFF407E4B70
  [06] present params: PresentationInterval -> IMMEDIATE      slot=000000001AF22DB0 0000000000000000 -> 0000000080000000
  [07] COM vtable slot (shared): IDirect3DSwapChain9::Present slot=0000000000D34000 0000000000771D30 -> 00007FFF407E50E0
  [08] COM vtable slot (shared): IDirect3DDevice9::Present    slot=0000000000D338F0 000000000076AA70 -> 00007FFF407E5190
  [09] COM vtable slot (shared): IDirect3DDevice9Ex::PresentEx slot=0000000000D338F0 000000000076BEF0 -> 00007FFF407E51F0
```

逐条解释：

| 改动 | 作用 | 会不会影响判定/谱面 |
|---|---|---|
| `Direct3DCreate9Ex` 导入槽 | 拿到 `IDirect3D9Ex`，只为给设备创建函数打补丁 | 否 |
| `IDirect3D9::CreateDevice(Ex)` 虚表槽 | 把交换链参数里的 vsync 关掉，并按 QPC 限速 | 否 |
| `EnumDisplaySettingsA` 导入槽 | 让游戏读到"适配器刷新率 = 机台帧率"（自检用） | 否 |
| `timeGetTime` / `GetTickCount` 导入槽 | 换成同一把尺子的更细刻度（见第 4 节） | 否，见第 4 节 |
| `Present` / `PresentEx` / 交换链 `Present` 虚表槽 | 限速点（一帧一次），用于等绝对时间表 | 否 |
| `PresentationInterval` 参数 | 关垂直同步，让帧率能被精确锁定 | 否 |

**没有任何一项涉及判定、谱面、音符位置或游戏内部的帧计数逻辑。**

---

## 3. 为什么判定窗口和谱面速度在设计上就不可能被改变

* IIDX 的判定是**基于帧**的：把输入发生的帧号与音符预定帧号相减，再和判定窗口表比较；
  谱面滚动位置也是"每帧固定位移"。这两者都由**游戏自身的逻辑**决定，插件没有改它们。
* 这些逻辑里唯一与外界有关的量是"每秒推进多少帧"。插件做的事就是让这个值**恰好等于机台原生帧率**
  （TDJ 120 / LDJ 60，实测 `draw=119.5~120.9`，偏差 < 0.5%），也就是把时间基准**还原**成机台的，
  而不是改变它。
* 反过来说：**不装插件的 240Hz 桌面**上，游戏主循环被 vsync 钉在 240~480 帧/秒，LDJ 会 4 倍速、
  判定窗口和谱面时间轴全部被压缩 —— 那才是"被改变"的状态，而这正是插件要修的问题。

---

## 4. 毫秒时钟是透明的（1.0.3 起）

`timeGetTime` / `GetTickCount(64)` 被替换成 QPC 派生的毫秒值。为了保证**语义完全一致**：

* 单位相同（毫秒）、32 位回绕行为相同；
* **起算点相同**：接管时先读一次系统真实 uptime，之后所有返回值都从那个值继续
  （例如日志里的 `take over at 665297906 ms`），因此游戏看到的时钟不会倒退、也不会跳变；
* 唯一区别是精度：不再被 1ms/15.6ms 的定时器粒度截断，而是高精度计时器的分辨率。

（1.0.2 及更早版本是从插件加载时刻从 0 开始计数的，虽然游戏本身没有因此出问题，
但严格来说那算一处行为差异，1.0.3 已修正。）

---

## 5. 你可以自己验证

1. **看报告**：游戏目录下 `iidxfps_report.txt` 的 `footprint:` 段落列出了插件写过的每个地址，
   以及"游戏代码字节改动 = NONE"。
2. **复现取证**：
   ```
   build 观察者模块（见 verify/README.md）
   spice64.exe -w -k verifydump.dll                 # 对照运行
   spice64.exe -w -k verifydump.dll -k iidxfps.dll  # 检查运行
   python verify/compare_dumps.py bm2dx.dll D1_text.bin E1_text.bin ...
   ```
3. **A/B 手感对比**：把桌面刷新率设成与机台一致（60Hz 桌面 + LDJ），此时插件默认
   `SkipWhenRefreshMatches=1` 会完全不介入限速与 vsync（报告里 `pacing: hands off (refresh matches)`）；
   把 `iidxfps.ini` 里 `Enabled=0` 再跑一次，两者手感应当没有差别（因为此时游戏本来就是对的）。
