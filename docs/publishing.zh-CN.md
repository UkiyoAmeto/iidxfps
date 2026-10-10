# 发布到 GitHub 的步骤

仓库已经准备好（本目录的上一级就是仓库根目录）。**推之前请确认三件事**：

1. 仓库里**没有**你个人的东西：PCBID / 卡号 / 服务器地址 / 本机路径。
2. 不要提交编译产物和玩家用的 zip：`.gitignore` 已经排除 `build/`、`test/out/`、`dist/`、
   `*.dll`、`iidxfps*.ini`、`*_report.txt`、`tools/`。
   玩家要的 dll 走 **GitHub Releases** 附件，而不是仓库文件。
3. 许可证是 GPL-3.0（因为 `src/spicesdk.h` 来自 spice2x），`LICENSE` 已放好。

---

## 方法一（推荐）：一键脚本

在仓库根目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\docs\publish.ps1
```

脚本会自动完成：找 git/gh（也支持放在 `tools\` 里）、**自动读取系统代理并设置 `HTTPS_PROXY`**、
检查待提交文件与敏感串、编译 `iidxfps.dll`、打包 release 附件、用浏览器设备码登录 GitHub、
建仓库、提交并推送、打 tag 建 Release。

常用参数：

```powershell
powershell -ExecutionPolicy Bypass -File .\docs\publish.ps1 -Check     # 只检查，不登录不推送
powershell -ExecutionPolicy Bypass -File .\docs\publish.ps1 -Repo myname -Private
powershell -ExecutionPolicy Bypass -File .\docs\publish.ps1 -SkipRelease
```

需要的三个工具（都是免安装 zip，解压到 `<仓库>\tools\` 或 `<仓库>\..\tools\` 即可，脚本会自动找）：

| 工具 | 下载 | 解压后路径 |
|---|---|---|
| git | <https://github.com/git-for-windows/git/releases> → `MinGit-*-64-bit.zip` | `tools\mingit\cmd\git.exe` |
| gh | <https://github.com/cli/cli/releases> → `gh_*_windows_amd64.zip` | `tools\gh\bin\gh.exe` |
| zig | <https://ziglang.org/download/> → `zig-x86_64-windows-*.zip` | `tools\zig\...\zig.exe` |

> **代理必看**：`gh` 和 `git` 是 Go/C 程序，**不读 Windows 系统代理**。国内直连 github.com
> 会卡住然后超时（表现为 `context deadline exceeded` 或 `dial tcp ... timeout`）。
> 脚本会自动从注册表读取系统代理并设置 `HTTP_PROXY`/`HTTPS_PROXY`；
> 如果自动读取不对，用 `-Proxy http://127.0.0.1:10808` 显式指定。
> 自己手动敲命令时也要先设：
> ```powershell
> $env:HTTPS_PROXY='http://127.0.0.1:10808'; $env:HTTP_PROXY=$env:HTTPS_PROXY
> ```

---

## 方法二：手动敲命令

```bash
cd iidxfps_repo
git init
git add .
git status                 # 再确认一遍里面没有个人文件
git commit -m "iidxfps v1.0.0: lock beatmania IIDX to its native 60/120 fps (spice2x module)"
git branch -M main
```

先在网页上新建一个空仓库（**不要**勾选 Add README / .gitignore / license），
仓库名建议 `iidxfps`，描述建议：

```
spice2x module that locks beatmania IIDX to its native 60/120 fps using QueryPerformanceCounter, with monitor-check refresh spoofing. Version independent, no bm2dx.dll patching.
```

然后：

```bash
git remote add origin https://github.com/<你的用户名>/iidxfps.git
gh auth login                     # 或直接用 https + PAT
gh auth setup-git
git push -u origin main
```

## 发 Release（把编译好的 dll 给玩家）

```bash
build.bat
gh release create v1.0.0 iidxfps_v1.0.0.zip --title "iidxfps v1.0.0" --notes "..."
```

`iidxfps_v1.0.0.zip` 里应包含：

```
iidxfps.dll
iidxfps.ini
玩家说明.txt          (docs\player-guide.zh-CN.txt)
原理与逆向笔记.md      (docs\how-it-works.zh-CN.md)
```

`publish.ps1` 会自动这样打包（输出在仓库上一级目录）。

## 以后改代码

```bash
git add -A
git commit -m "..."
git push
```

改了源码记得在 `CHANGELOG.md` 记一笔，并重新 `build.bat` 后更新 Release 附件。
