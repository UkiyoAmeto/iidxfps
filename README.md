# iidxfps

一个 [spice2x](https://github.com/spice2x/spice2x.github.io) 模块，用来解决 beatmania IIDX
**"游戏速度跟着显示器刷新率跑"** 的问题：让游戏逻辑稳定在机台原生的
**120 帧（TDJ）/ 60 帧（LDJ）**，同时把 Monitor Check 看到的"适配器帧率"也改成同一个值。

**不修改 `bm2dx.dll` 的任何一个字节，也不改任何游戏文件。** 全部钩子只依赖导入函数名和
COM 接口的固定 vtable 下标，因此**与 bm2dx.dll 版本无关**。

> English documentation: [README.en.md](README.en.md)

---

## 症状与原因

IIDX 的游戏逻辑是**"每次主循环推进一帧"**，没有任何基于真实时间的增量，主循环又被 vsync
钉在显示器刷新率上。于是：

* 显示器 120Hz 跑 LDJ（60 帧机台）→ 游戏 **2 倍速**；144/165/240Hz 更快；
* 显示器 60Hz 跑 TDJ（120 帧机台）→ 游戏 **半速**；
* 帧率不稳时自检的 Monitor Check 报 `MONITOR ERROR`。

实测数据（120Hz 桌面、不加载任何模块）：`fps(draw) ≈ 125`，游戏确实是两倍速。

## 它做了什么

| # | 功能 | 实现 |
|---|---|---|
| 1 | 让主循环按 QPC 精确走帧 | 挂钩游戏自己导入的 `d3d9.dll!Direct3DCreate9Ex` → 拦 `IDirect3D9::CreateDevice(Ex)` 把交换链改成无 vsync，再在 `Present` 家族 / `IDirect3DSwapChain9::Present` / spice 的 D3D9 绘制回调里用 QueryPerformanceCounter 把主循环按住到"每秒正好 N 帧" |
| 2 | 把 `timeGetTime` / `GetTickCount` 换成 QPC 实现 | 挂钩 bm2dx.dll 的导入项；单位与 32 位回绕行为完全一致，只是分辨率从 1ms 变成高精度计时器 |
| 3 | 欺骗 Monitor Check / 适配器帧率 | 挂钩 bm2dx.dll 的 `user32.dll!EnumDisplaySettings(A/W/ExA/ExW)`，把返回的 `dmDisplayFrequency` 改成目标帧率 |

附带：状态报告 `iidxfps_report.txt`（见下）、实测帧率日志、诊断模式、运行中快捷键。

## 安装

1. 把 `iidxfps.dll` 和 `iidxfps.ini` 放到游戏的 `contents` 目录（和 `spice64.exe`、`bm2dx.dll` 同级）。
2. 在你自己启动游戏的 bat 里，给 `spice64.exe` 的参数加上：

   ```
   -k iidxfps.dll
   ```

   例如：

   ```
   spice64.exe -url http://<你的服务器> -p <你的PCBID> -k iidxfps.dll -k 2dx-gsm.dll -k IIDXScore.dll
   ```

3. 启动游戏即可，不需要其它设置。

**注意 `-k` 的顺序无所谓**：模块在自己的线程里等待 `bm2dx.dll` 加载完成后再装钩子。
不想用它时：把 ini 里 `Enabled=0`，或去掉 `-k iidxfps.dll`。

## 配置

参考配置见 [docs/iidxfps.ini](docs/iidxfps.ini)（安装时复制到游戏目录，文件名保持 `iidxfps.ini`）。
几个重点：

| 键 | 说明 |
|---|---|
| `FPS=auto\|60\|120` | 目标帧率。`auto` 会自动判断机台（见下）。**如果 LDJ 打了 `Force LDJ Custom Timing/Adapter FPS = 120` 这类补丁，必须写 `FPS=120`**，否则会变半速 |
| `PaceAt=auto\|draw\|swapchain\|present` | 限速点，`auto` 会自动挑实际被调用的那个（本游戏是 spice 的绘制回调，一帧一次） |
| `ForceImmediatePresent=1` | 必须为 1，否则在高于目标帧率的显示器上压不下来 |
| `SkipWhenRefreshMatches=1` | 刷新率本来就等于目标帧率时什么都不做（避免把正常机器弄出撕裂） |
| `MeasureOnly=1` | 只测量、不干预，用来确认游戏原本跑多少帧 |

### `FPS=auto` 是怎么判断 TDJ / LDJ 的

按顺序试四个信号，任意一个成立就算 TDJ（120 帧）：

1. **启动命令行**里有没有 TDJ 模式开关（`-iidxtdj` / `-iidxtdjw` / `-sp2x-iidxtdjw`）。
   只认这几个模式开关本身，像 `-iidxtdjcamhook`、`-iidxtdjsubsize` 这种"名字里带 tdj 的设置项"
   不算（它们关掉 TDJ 之后还会留在配置里）。
2. **spicecfg 配置文件**（`%APPDATA%\spicetools.xml`，可用 ini 里的 `SpiceCfgPath` 指定）。
   在 spicecfg 界面里勾选 TDJ 就存在这里。注意 spice2x 的布尔选项是**值非空才算启用**：
   `<option name="iidxtdj" value="1"/>` 是开，`value=""` 是关。
   （这一条和命令行一样是**立刻**可得的，所以游戏枚举显示模式之前就已经确定，
   整张模式表和刷新率欺骗从第一帧起就是一致的。）
3. `C:\000rom.txt` 内容为 `TDJ-JA`，或 `D:\001rom.txt` 内容为 `TDJ`（spice2x 自己也读这两个文件）。
4. **游戏自己读的机台 ROM**（`/c_drv/000rom.txt`）——通过 `avs2-core.dll` 调同一个 AVS 文件系统
   去读（spice2x 在 TDJ 模式下是把这份 ROM 注入到内存里的，硬盘上没有文件；`avs2-core.dll`
   的导出名是混淆的且随版本变化，模块会把 spice2x 认识的所有命名都试一遍）。
   这一步在游戏自己的线程上做（`EnumDisplaySettings`、`CreateDevice`、第一帧绘制回调），
   读到之后会重新评估"桌面刷新率已经匹配就不介入"的判断。

报告文件里的 `cabinet:` 行会把四个信号的实际结果都列出来，方便排查：

```
cabinet  : auto -> cmdline=TDJ spicecfg=no romfiles=no game rom="TDJ-JA"
```


游戏内快捷键（按住 Ctrl+Shift）：`F9` 切换 120/60，`F10` 开关刷新率欺骗，`F11` 输出状态。

## 怎么确认它在工作 / 怎么反馈问题

模块每秒刷新游戏目录下的 `iidxfps_report.txt`，内容自包含（示例见
[docs/iidxfps_report.sample.txt](docs/iidxfps_report.sample.txt)）：

```
iidxfps 1.0.0 - beatmania IIDX frame timing report
config   : Enabled=1 FPS=auto target=60 PaceAt=draw ForceImmediatePresent=1
           SpoofDisplayRefresh=1 SpoofAllModes=1 HighPrecisionTime=1
           MeasureOnly=0 SkipWhenRefreshMatches=1 ForcePacing=0
           HookPresent=1 HookPresentEx=1 register_d3d9=yes
desktop  : 120Hz   game is told 60Hz
pacing   : ENABLED (QPC schedule, vsync off)   pacePoint=draw   deviceHooked=1
frames/s : draw=60.40 swapchain=119.82 present=0.00 paced=60 late=0 skipped=0
frame gap: 14.574 .. 18.372 ms
```

`draw` 就是**游戏逻辑帧率**，应该稳定在 120 或 60（±1%）。
报告问题时只要发这一个文件，不要发 spice 的 `log.txt`（又大又可能含个人信息）。

## 构建

需要 C 编译器。推荐 zig（免安装、不需要 MSVC）：

1. 从 <https://ziglang.org/download/> 下载 `zig-x86_64-windows-<版本>.zip`
2. 三种方式任选：放到 PATH、设 `ZIG=C:\path\to\zig.exe`、或解压到 `tools\zig\zig.exe`
3. 运行：

   ```
   build.bat
   ```

   产物为 `build\iidxfps.dll`。

## 不用游戏也能验证：独立测试台

`test\` 里有一个最小复现环境，能在不启动游戏的情况下验证限速逻辑：

* `bm2dx_stub.cpp` → 伪装成 `bm2dx.dll`，导入 `Direct3DCreate9Ex` 和 `EnumDisplaySettingsA`，
  建一个请求 vsync 的 D3D9 设备，然后跑"一帧一次 Present"的主循环；
* `host.c` → 一个极简 spice2x SDK 宿主机，加载上面那个假 dll 和模块。

```
cd test
build_test.bat
cd out
copy ..\..\docs\iidxfps.ini iidxfps.ini
host.exe 4                 :: 目标 60 → 看 "measured loop rate" / report
set IIDXFPS_TEST_SHOW=1    :: 让窗口可见，用于复现真实的 vsync 场景
```

在 120Hz 桌面上：目标 60 → 稳态 60.2；目标 120 → 稳态 120.3；
不加载模块时同样场景是显示刷新率（120.35）。

## 实测数据（测试机：1920x1080 @ 120Hz）

| 场景 | 结果 |
|---|---|
| 不干预（`MeasureOnly=1`） | `fps(draw) ≈ 125` → LDJ 游戏两倍速 |
| 模块生效（目标 60） | `fps(draw) = 59.5 ~ 60.8`、`paced=60/s`、帧间隔 15~18ms |
| 目标 120 | `fps(draw) = 119.3 ~ 120.9`、`fps(swapchain) = 240`、帧间隔 7~10ms |
| 全屏 | 稳定 60，显示器模式未被改动 |
| 独立测试台（隐藏窗口，原生 45000fps） | 目标 120 → 120.33；目标 60 → 60.25 |

## 已知限制

* **Monitor Check 的完整流程没有走完过**（需要真实服务器 + 投币 + 进 TEST 菜单）。已确认的是：
  游戏读到的"适配器刷新率"已被改成目标值，实际帧率也被锁在同一个值上。如果仍然报错，
  请带 `iidxfps_report.txt` 反馈。
* 为了把帧率压到 120/60，模块会关掉垂直同步，**可能出现画面撕裂**。这是"锁定一个显示器
  无法原生驱动的帧率"的必要代价。
* 不要和 RTSS / 显卡驱动的帧率限制同时用（会取更慢的那个）。
* "渲染跑 144 帧、逻辑跑 60 帧"这种玩法在本模块里做不到，原因见
  [docs/how-it-works.zh-CN.md](docs/how-it-works.zh-CN.md)（一句话：IIDX 的逻辑是整数帧计数，
  进程外只能控制循环频率）。

## 原理与逆向笔记

想了解 `fps(draw)` 为什么等于逻辑帧率、一帧为什么要 Present 两个交换链、以及
"为什么做不到渲染 144 / 逻辑 60"等内容，见 [docs/how-it-works.zh-CN.md](docs/how-it-works.zh-CN.md)。
那些结论都有实测或反汇编依据。

## 许可证

**GPL-3.0**（见 [LICENSE](LICENSE)）。

原因：`src/spicesdk.h` 是 spice2x 的插件 SDK 头文件，逐字节取自 spice2x 项目，而 spice2x 采用
GPL-3.0；本模块也是设计给 spice2x 加载使用的。为了不产生许可冲突，本仓库整体采用 GPL-3.0。

## 免责声明

本项目仅供**离线家用环境**（配合 spice2x 模拟器）研究与使用，与 KONAMI 没有任何关系，
不包含任何游戏文件。请勿用于官方网络或任何商业用途。
