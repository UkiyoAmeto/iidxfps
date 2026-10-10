# 原理、逆向依据与"为什么做不到"的那件事

本文记录这个模块背后的实测结论和反汇编依据，方便后来的人（以及未来的我）不用重新查一遍。
测试环境：`bm2dx.dll` 构建时间 2026-08-24（IIDX 33 LDJ），桌面 1920x1080 @ 120Hz，
spice2x 2026 版，Windows 11。

---

## 1. 症状的机制：游戏逻辑 = 每次主循环推进一帧

spice2x 的插件 SDK 提供 `register_d3d9`：它会在**每次游戏主循环**里回调一次
（`SPICE_SDK_D3D9_DRAW`，在真正的 Present 之前、抢占了 backbuffer 的场景里）。
所以日志里的 `fps(draw)` 就等于**游戏的逻辑帧率**。

实测（`MeasureOnly=1`，不装任何钩子、不改任何东西）：

| 指标 | 值 |
|---|---|
| `fps(draw)`（= 逻辑帧率） | ≈ 125 |
| `fps(swapchain)`（交换链 Present 次数） | ≈ 250 |
| 桌面刷新率 | 120Hz |
| 游戏原生目标（LDJ） | 60 |

也就是说：**不干预时 LDJ 游戏跑了两倍速**，而且 **一帧会 Present 两个交换链**（主画面 + 副画面）。

前者的含义是：游戏状态推进完全由"主循环跑了几次"决定，没有任何基于真实时间的增量
（否则帧率再高也不会变速）。后者的含义是：**把限速点放在交换链 Present 上会一帧等两次**，
游戏变成半速——这一条第一次实测时确实踩到了（`fps(swapchain)=60` 而 `fps(draw)=30`）。

⇒ 结论：**在进程外，能控制"游戏速度"的唯一杠杆就是主循环每秒跑多少次。**

---

## 2. 为什么"渲染 144 帧、逻辑 60 帧"从进程外做不到

既然逻辑是"每次循环 +1 帧"，那么"逻辑 60 帧/秒"就意味着"循环 60 次/秒"。
想让循环跑 144 次/秒而逻辑仍然 60，就必须让游戏**每次循环只推进 0.4167 帧**。
而游戏推进的是**整数帧计数**（音符位置、动画帧都是这么算的），小数帧无处安放：

* 从外部"跳过一次 update"→ 画面会出现"同一个状态重复 2~3 次、间隔不均"（judder），
  对节奏游戏比 60fps 稳定输出更糟；
* 真正做到"逻辑 60 + 渲染 144 且画面平滑"，必须在游戏内部插入时间插值，
  或者把每帧的增量改成基于真实时间——那就要按版本定位游戏内部代码（AOT 特征码 + 补丁），
  也就放弃了"版本无关"这个前提。

顺带说明另一个常见猜想：**游戏并不会根据"适配器刷新率 / timing fps"自动缩放逻辑**。
证据：把 `EnumDisplaySettingsA` 的返回改成 60Hz（SpoofDisplayRefresh=1）但**不限速**时，
游戏的逻辑帧率依旧是 ≈125 —— 如果它会按 60/120 缩放，那时就应该变回 60 了。

---

## 3. 三个钩子分别是怎么起作用的

### 3.1 交换链必须关掉 vsync

`bm2dx.dll` 导入 `d3d9.dll!Direct3DCreate9Ex`（导入表里能看到，spice2x 也会钩它）。
我们挂钩 bm2dx.dll 的这个导入项 → 拿到 `IDirect3D9Ex` 对象 → 给**这个对象**的 vtable
打补丁（`CreateDevice` = 16、`CreateDeviceEx` = 20）→ 在真正创建前把
`D3DPRESENT_PARAMETERS.PresentationInterval` 改成 `D3DPRESENT_INTERVAL_IMMEDIATE`。

* `D3DPRESENT_PARAMETERS` 是 64 字节，`PresentationInterval` 在偏移 0x3C —— 代码里有编译期断言。
* 实测游戏请求的是 `interval=0x00000000`（DEFAULT，即 vsync 开），
  在 120Hz 桌面上就是 120 次/秒；不关掉它，QPC 限速会被 vsync 顶住。
* 如果创建失败会带原始参数重试一次，避免把游戏搞崩。

**踩过的坑（很重要）**：一开始是给设备对象**复制一份 vtable 然后替换对象自己的 vtable 指针**，
结果 d3d9 内部会顺着 vtable 附近取元数据/缓存指针，直接跳到 NULL 崩掉
（异常：`rip=0, rcx=设备指针`）。改成**直接给共享 vtable 的对应槽位打补丁**
（VirtualProtect → 写 → 还原）之后就完全正常了。

### 3.2 限速点要选"一帧恰好一次"的那个

三个候选点：

| 点 | 一帧调用次数（本游戏） | 说明 |
|---|---|---|
| spice 绘制回调 `DRAW` | 1 | 最合适 |
| `IDirect3DSwapChain9::Present` | 2（主+副画面） | 每帧限速两次 = 半速 |
| `IDirect3DDevice9(Ex)::Present(Ex)` | 0 | 本游戏不从这里走 |

所以模块会在运行时统计三个点各自的实际调用次数（每 20ms 采样一次），自动选"真正在被调用"
的那个，并把结果写进日志/报告（`pacePoint=`）。这也是"版本无关"的一部分：
换版本如果调用路径变了，它会自己重新选。

限速本身是**绝对时间表**：每次限速点用 QPC 算出一个 deadline 并等到它，迟到超过一帧就重新对表
（不做"追赶"，追赶会让游戏跑快）。等待用高精度 waitable timer（`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`）
+ 最后 ~2ms 自旋，实测抖动 ±1% 以内。

### 3.3 时钟与刷新率欺骗

* `winmm.dll!timeGetTime`、`kernel32.dll!GetTickCount(64)`：改成
  `(QPC - 起始) * 1000 / QPC频率`，截断成 32 位（回绕行为与原来一致）。
  单位语义完全不变，只是分辨率从 1ms 变成高精度计时器分辨率。
* `user32.dll!EnumDisplaySettings(A/W/ExA/ExW)`：游戏用它读当前显示模式和模式列表
  （反汇编里能看到它对每个模式循环 10000 次取 `dmBitsPerPel/dmPelsWidth/dmPelsHeight/dmDisplayFrequency`，
  在 `DEVMODEA` 偏移 0x68/0x6C/0x70/0x78）。
  我们把 `dmDisplayFrequency` 改成目标帧率，于是游戏看到的"适配器"就是 60/120Hz。

---

## 4. "游戏原本 125fps"是怎么测出来的

`iidxfps.ini` 里 `MeasureOnly=1` 时不装任何钩子，只统计帧率。
在 120Hz 桌面上得到 `fps(draw) ≈ 125`（比 120 略高是 DWM 合成导致的窗口化特性，全屏会更接近 120）。
这一项也是所有人反馈问题时最有用的诊断开关。

---

## 5. TDJ / LDJ 是怎么判定的（踩坑记录）

一开始用 SDK 的 `get_avs_info().model` 判断机台：`model == "TDJ"` 就算 TDJ。**这是错的。**

`avs::game::MODEL` 来自游戏自己的 `prop/ea3-config.xml` 的 `<model>` 节点，而开 TDJ 模式时
spice2x 并不会改这个文件（用户装的就是 LDJ 的配置，节点一直是 `LDJ`），spice2x 干的是：

```cpp
// games/iidx/iidx.cpp
TDJ_MODE |= fileutils::text_read("C:\\000rom.txt") == "TDJ-JA";
TDJ_MODE |= fileutils::text_read("D:\\001rom.txt") == "TDJ";
if (TDJ_MODE) {
    hooks::avs::set_rom("/c_drv/000rom.txt", "TDJ-JA");   // ← 注入到内存
    hooks::avs::set_rom("/d_drv/001rom.txt", "TDJ");
    ...
}
```

`set_rom` 只是往 spice2x 的一个 `std::map` 里塞内容，游戏通过 AVS 文件系统读
`/c_drv/000rom.txt` 时被 spice2x 的 `avs_fs_open` 钩子拦下来，返回一个假的 fd（1337），
再读就是 `"TDJ-JA"`。**硬盘上根本没有这个文件**，所以看文件是看不出来的。

于是正确的判定方式是：**从命令行、从 spice2x 自己读的那两个文件、以及用 `avs2-core.dll`
调同一个 AVS 文件系统去读游戏读的那份 ROM**。后者要注意：

* `avs2-core.dll` 的导出名是混淆的（`XCgsqzn000004e` 之类）且随版本变化，
  所以要把 spice2x 源码里所有版本的命名都试一遍；
* **失败的 `avs_fs_open` 返回的不是 0，而是 HRESULT 风格错误码（文件不存在是 `0x80070002`）**。
  把它当句柄传给 `avs_fs_read`/`avs_fs_close` 会破坏游戏 VFS 状态并直接崩进程
  （日志里表现为 `W:vfs: read: desc 80070002 != AVS_DTYPE_FS` 然后 `EXCEPTION_ACCESS_VIOLATION`）；
  而且 `avs_fs_read` 出错时会返回一个很大的值，当成读取长度就会越界。两处都必须防。
* 这个调用**必须在游戏自己的线程上做**：在 worker 线程上调用真实（非注入）文件系统的 open
  会挂住；在启动早期（例如游戏刚开始枚举显示模式时）调用也可能阻塞。放在
  `EnumDisplaySettings` / `CreateDevice` / 首帧绘制回调里最稳。

还有两个实测结论：

* 一帧在窗口化下会 Present 两个交换链（见第 1 节），**限速点必须选"一帧恰好一次"的那个**；
* 游戏在首帧之后会阻塞 4 秒左右（自检/资源加载），期间没有任何绘制。
  所以"等几帧再判定机台"会让判定落到自检之后——一定要在 `CreateDevice` 之前就把机台定下来。

---

## 6. 硬件/驱动层面的补充观察

* **全屏 vs 窗口化**：实测窗口化时一帧 Present 两个交换链（`fps(swapchain) ≈ 2 × fps(draw)`），
  全屏时只有一个（`fps(swapchain) = fps(draw)`）。所以限速点不能写死。
* **隐藏/被遮挡的窗口**：DWM 会把 vsync 的 Present 压到 ~33fps（测试台里能看到），
  这不是模块的问题，游戏在前台时不受影响。
* **显示器模式表**：测试机的显示器支持 120/144/165/240Hz；游戏会枚举全部模式。
  这也解释了为什么"高刷屏反而更容易触发这个 bug"。
