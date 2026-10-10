# 独立测试台（不需要启动游戏）

`test\` 里的两个文件构成一个最小复现环境，用来在不启动 IIDX 的情况下验证限速逻辑：

| 文件 | 作用 |
|---|---|
| `bm2dx_stub.cpp` | 伪装成 `bm2dx.dll`：导入 `Direct3DCreate9Ex` 和 `EnumDisplaySettingsA`，用一个请求垂直同步的 D3D9 设备，跑"一帧一次 Present"的主循环（和真游戏的循环结构一致） |
| `host.c` | 一个极简的 spice2x SDK 宿主机：加载上面那个假 dll，再把模块 `spice_sdk_entry_point` 调起来，最后运行主循环并打印实测帧率 |

## 用法

```
cd test
build_test.bat
cd out
copy ..\..\docs\iidxfps.ini iidxfps.ini      :: 参考配置（也可以直接用发布包里的）

host.exe 4                    :: 跑 4 秒，打印 measured loop rate
set IIDXFPS_TEST_SHOW=1       :: 让窗口可见，用来复现真实的垂直同步场景
```

## 在 120Hz 桌面上应该看到

| 配置 | 结果 |
|---|---|
| 不加载模块（或 `Enabled=0`），窗口可见 | ≈120（= 显示器刷新率，说明"循环跟着刷新率跑"） |
| 模块生效，`FPS=60`，隐藏窗口 | 稳态 ≈60（原生 45000fps 也会被压到 60） |
| 模块生效，`FPS=120`，隐藏窗口 | 稳态 ≈120 |

稳态请看报告里的 `frames/s` 或日志里的 `fps(present)`。

## 关于 `host.exe` 打印的 "measured loop rate"

那个数字是**整段运行的平均值**，包含了最前面约 20ms 的"还没决定限速点、没有限速"的窗口。
测试台里的假游戏原生跑 45000fps（真游戏只有 125fps），所以这 20ms 会把平均值拉高
（比如看到 184 而不是 60）。**要看稳态请用报告里的 `frames/s` 或日志里的 `fps(present)`**，
它们会稳定在目标值上（±1%）。

如果实测值明显偏离目标，请把 `out\iidxfps_report.txt` 一起发出来。
