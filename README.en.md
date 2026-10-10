# iidxfps

A [spice2x](https://github.com/spice2x/spice2x.github.io) module that fixes the classic
beatmania IIDX problem of **the game speed following the monitor refresh rate**. It pins the
game logic to the cabinet's native rate (**120 fps for TDJ, 60 fps for LDJ**) using the Windows
high precision timer, and makes the game believe the adapter really runs at that rate so the
self test (Monitor Check) stops complaining.

**It never modifies `bm2dx.dll` or any other game file.** Every hook is based on import names and
the fixed COM vtable layout of D3D9, so it is **independent of the bm2dx.dll build**.

> 中文文档：[README.md](README.md)

---

## The problem

IIDX advances its game state **once per main loop iteration** — there is no wall-clock delta time
anywhere — and the loop is pinned to the display refresh rate by vsync. Therefore:

* a 120Hz screen running the LDJ (60fps cabinet) build runs the game at **2x speed**
  (144/165/240Hz screens are worse);
* a 60Hz screen running the TDJ (120fps cabinet) build runs it at **half speed**;
* an unstable frame rate makes the boot time Monitor Check fail with `MONITOR ERROR`.

Measured on a 120Hz desktop with no module loaded: `fps(draw) ≈ 125`, i.e. exactly that 2x speed.

## What it does

| # | Feature | How |
|---|---|---|
| 1 | Drive the loop from QueryPerformanceCounter | Hook the game's own `d3d9.dll!Direct3DCreate9Ex` import to intercept `IDirect3D9::CreateDevice(Ex)`, force the swap chain to `D3DPRESENT_INTERVAL_IMMEDIATE` (no vsync), then hold the loop to exactly N frames per second in `Present`/`PresentEx` / `IDirect3DSwapChain9::Present` / spice's D3D9 draw callback |
| 2 | Replace the game's millisecond clocks | Hook the `winmm.dll!timeGetTime` and `kernel32.dll!GetTickCount(64)` imports of bm2dx.dll and return QPC derived values — same unit, same 32 bit wrap-around, just a much finer resolution |
| 3 | Spoof the adapter refresh rate | Hook the `user32.dll!EnumDisplaySettings(A/W/ExA/ExW)` imports of bm2dx.dll and rewrite `dmDisplayFrequency` to the target rate |

Extras: a self contained status report (`iidxfps_report.txt`), per-second frame rate logging,
a measure-only diagnostics mode and in-game hotkeys.

## Install

1. Copy `iidxfps.dll` and `iidxfps.ini` into the game's `contents` directory
   (next to `spice64.exe` and `bm2dx.dll`).
2. Add `-k iidxfps.dll` to the `spice64.exe` command line in your own start script:

   ```
   spice64.exe -url http://<your server> -p <your PCBID> -k iidxfps.dll -k 2dx-gsm.dll -k IIDXScore.dll
   ```

3. Start the game. Nothing else to configure.

The position of `-k iidxfps.dll` in the list does not matter: the module waits on its own thread
until `bm2dx.dll` is loaded and only then installs its hooks.
To turn it off, either set `Enabled=0` in the ini or drop `-k iidxfps.dll`.

## Configuration

See the (Chinese commented) reference config [docs/iidxfps.ini](docs/iidxfps.ini) — copy it next to
the DLL, keeping the name `iidxfps.ini`. The important keys:

| Key | Meaning |
|---|---|
| `FPS=auto\|60\|120` | Target rate. `auto` detects the TDJ/LDJ cabinet the same way spice2x does. **If your LDJ build has the `Force LDJ Custom Timing/Adapter FPS = 120` patch, set `FPS=120`**, otherwise the module will hold it to 60 and the game runs at half speed |
| `PaceAt=auto\|draw\|swapchain\|present` | Where the loop is held back; `auto` picks whichever point is actually being called (for this game: spice's draw callback, once per game frame) |
| `ForceImmediatePresent=1` | Required, otherwise vsync keeps the loop at the display refresh rate |
| `SkipWhenRefreshMatches=1` | Do nothing at all when the refresh rate already equals the target (avoids introducing tearing on setups that were fine) |
| `MeasureOnly=1` | Diagnostics: install no hooks, only measure what the game does on its own |

Hotkeys (hold Ctrl+Shift while the game window is focused): `F9` switch 120/60, `F10` toggle the
refresh spoof, `F11` dump a status line.

## Verifying it works / reporting a problem

The module rewrites `iidxfps_report.txt` next to the game once per second. It is self contained
(see [docs/iidxfps_report.sample.txt](docs/iidxfps_report.sample.txt)):

```
iidxfps 1.0.0 - beatmania IIDX frame timing report
config   : Enabled=1 FPS=auto target=60 PaceAt=draw ForceImmediatePresent=1
           ...
desktop  : 120Hz   game is told 60Hz
pacing   : ENABLED (QPC schedule, vsync off)   pacePoint=draw   deviceHooked=1
frames/s : draw=60.40 swapchain=119.82 present=0.00 paced=60 late=0 skipped=0
frame gap: 14.574 .. 18.372 ms
```

`draw` **is the game logic frame rate** — it should sit at 120 or 60 (±1%).
When reporting an issue, send that single file instead of spice's `log.txt` (which is large and
may contain personal information).

## Building

You need a C compiler; [zig](https://ziglang.org/download/) is recommended (no installation, no
MSVC required):

1. Download `zig-x86_64-windows-<version>.zip` and either put `zig.exe` on `PATH`,
   set `ZIG=C:\path\to\zig.exe`, or extract it to `tools\zig\zig.exe`.
2. Run `build.bat` → produces `build\iidxfps.dll`.

## Standalone test harness (no game needed)

The `test\` directory contains a minimal reproduction so you can verify the pacing logic without
starting IIDX:

* `bm2dx_stub.cpp` — pretends to be `bm2dx.dll`: imports `Direct3DCreate9Ex` and
  `EnumDisplaySettingsA`, creates a D3D9 device that asks for vsync, and runs the same naive
  "one logic tick per present" loop.
* `host.c` — a tiny spice2x SDK host that loads that fake game dll plus the module.

```
cd test
build_test.bat
cd out
copy ..\..\docs\iidxfps.ini iidxfps.ini
host.exe 4                 :: watch the steady state in the report / log
set IIDXFPS_TEST_SHOW=1    :: show the window, to reproduce a real vsync scenario
```

On a 120Hz desktop: target 60 → steady state 60.2, target 120 → 120.3, while the same harness
without the module runs at the display refresh rate (120.35).

## Measured results (test machine: 1920x1080 @ 120Hz)

| Scenario | Result |
|---|---|
| No module (`MeasureOnly=1`) | `fps(draw) ≈ 125` — the LDJ game runs at 2x speed |
| Module active, target 60 | `fps(draw) = 59.5 … 60.8`, `paced=60/s`, frame gap 15–18 ms |
| Module active, target 120 | `fps(draw) = 119.3 … 120.9`, `fps(swapchain) = 240`, frame gap 7–10 ms |
| Fullscreen | holds 60, display mode untouched |
| Standalone harness (hidden window, 45000fps native) | target 120 → 120.33; target 60 → 60.25 |

## Known limitations

* **The full Monitor Check flow has never been walked through** (it needs a real server, a coin
  and the TEST menu). What is verified is that the game is told the target refresh rate and that
  the real frame rate is locked to the same value. If it still fails, please send the report file.
* To hit a rate the display cannot be driven at, the module disables vsync, so **tearing is
  possible**. That is the price of an exact frame rate.
* Do not combine it with RTSS or driver level frame limiters (the slowest one wins).
* "Render at 144 while the logic runs at 60" cannot be done by this module — see
  [docs/how-it-works.zh-CN.md](docs/how-it-works.zh-CN.md) (Chinese). In one line: IIDX counts
  integer frames, so from outside the game the only lever on the game speed is the loop rate.

## How it works / reverse engineering notes

See [docs/how-it-works.zh-CN.md](docs/how-it-works.zh-CN.md) (Chinese) for the measurements and
disassembly evidence behind: why `fps(draw)` equals the logic frame rate, why the game presents two
swap chains per frame, and why "render at 144 while the logic runs at 60" is impossible from
outside the game.

## License

**GPL-3.0** — see [LICENSE](LICENSE).

`src/spicesdk.h` is the spice2x plugin SDK header, taken verbatim from the spice2x project, which
is licensed under the GPL-3.0. This module is meant to be loaded by spice2x as well, so the whole
repository is released under the GPL-3.0 to avoid any license conflict.

## Disclaimer

For **offline home use** with the spice2x emulator only. Not affiliated with KONAMI in any way and
contains no game files. Do not use it with official networks or for any commercial purpose.
