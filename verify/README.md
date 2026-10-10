# verify/ - 证明插件没有改动游戏代码

这里的三个文件用来**独立验证**"iidxfps 不修改 `bm2dx.dll` 的任何代码字节"这个说法。
结论与完整数据见 [../docs/no-effect-on-judgement.zh-CN.md](../docs/no-effect-on-judgement.zh-CN.md)。

| 文件 | 作用 |
|---|---|
| `verifydump.c` | 一个**只读** spice2x 模块：不装钩子、不写内存，只把 `bm2dx.dll` / `d3d9.dll` 的 `.text` / `.rdata` / `.pdata` 从运行中的进程里转储出来，并记录 sha256 |
| `compare_dumps.py` | 比较两次运行的转储，先屏蔽掉 Windows 加载器必然写入的字节（重定位 + 导入表），剩下的差异才是有意义的 |
| `dump_module_sections.ps1` | 从外部进程转储某个模块的段（需要与游戏同权限；spice2x 通常以管理员运行，所以它一般需要提权，仅作备用） |

## 怎么用

```bat
rem 1) 编译观察者模块（与主模块相同的工具链）
cd ..\..
zig cc -target x86_64-windows-gnu -O2 -shared -o verify\verifydump.dll ^
    verify\verifydump.c src\iidxfps.def -Isrc -lbcrypt
copy verify\verifydump.dll <游戏目录>\

rem 2) 对照运行：只加载观察者
spice64.exe -w -k verifydump.dll
rem    等游戏稳定后退出，收集 verifydump*.bin 与 verifydump.txt

rem 3) 检查运行：同时加载被检查的插件
spice64.exe -w -k verifydump.dll -k iidxfps.dll

rem 4) 比较（python 3，不需要额外库）
python verify\compare_dumps.py <游戏目录>\bm2dx.dll ^
    A_verifydump_text.bin B_verifydump_text.bin ^
    A_verifydump_rdata.bin B_verifydump_rdata.bin ^
    A_verifydump_pdata.bin B_verifydump_pdata.bin
```

**重要**：ASLR 会让两次运行的加载基址不同，加载器会把镜像里所有重定位目标改写一遍
（`bm2dx.dll` 有 12 万条重定位）。不要直接比较原始字节，否则会把加载器的正常行为误判成
"插件改了代码"——这也是 `compare_dumps.py` 存在的原因。

如果你想更直观地比较，还有一个更简单的办法：**跑两次对照运行**（都不加载插件），
你会发现 `.text` 里同样有 1~2 个字节的差异（那是 spice2x 自己 inline hook 的跳板地址），
位置和"加载插件"时的差异位置完全相同。这说明那点差异与插件无关。
