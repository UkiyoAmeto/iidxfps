# Changelog

## v1.0.2

* `FPS=auto` additionally reads **spicecfg's options file** (`%APPDATA%\spicetools.xml`, override with
  the new `SpiceCfgPath` ini key). Users who enable TDJ mode in the GUI (rather than with `-iidxtdj`
  on the command line) now get the right target rate *before* the game enumerates the display modes,
  so the whole mode table and the refresh spoof are consistent from the first frame on.
  spice2x treats a bool option as enabled only when its value is non-empty
  (`<option name="iidxtdj" value="1"/>` on, `value=""` off).
* Fixed a false positive in both instant matchers: every option whose name merely *contains*
  `iidxtdj` used to count as TDJ. `-iidxtdjcamhook` and `-iidxtdjsubsize` persist after TDJ is
  switched off, so a plain LDJ cabinet could be mistaken for TDJ and forced to 120 fps.
  Only the actual mode switches (`iidxtdj`, `iidxtdjw`, `sp2x-iidxtdjw`) match now.
* Fixed an out of bounds stack read in the new spicecfg chunked file reader (the carried
  tail was taken from the wrong offset, so up to 2 kB past the read buffer were copied and
  the process could crash right after `Direct3DCreate9Ex`). Verified in fullscreen, which is
  how the reporter runs the game.* The report's `cabinet:` line lists all four signals (`cmdline=`, `spicecfg=`, `romfiles=`, the
  game's own rom).



## v1.0.1

Bug fix release: **TDJ cabinets were held at 60 fps with `FPS=auto`**, which made the game run at
half speed and the boot time monitor check fail.

* TDJ detection reworked. The old code compared the AVS *model* string against `"TDJ"`, but in TDJ
  mode spice2x only injects the cabinet rom in memory and `prop/ea3-config.xml` keeps saying `"LDJ"`,
  so that check never matched a real TDJ cabinet.
* `FPS=auto` now uses three signals, in order:
  1. the launcher command line (`-iidxtdj`, `-iidxtdjw`, ... — every TDJ option contains
     `iidxtdj`, and none of them exists for LDJ). This is instant, so the whole display mode table
     and the refresh spoof are consistent from the first frame on;
  2. `C:\000rom.txt` / `D:\001rom.txt`, the same files spice2x itself reads;
  3. the cabinet rom the game itself reads (`/c_drv/000rom.txt`), asked for through
     `avs2-core.dll` (the export names are obfuscated and change between builds, so every set
     spice2x knows about is tried). This is done from the game's own thread — the
     `EnumDisplaySettings`, `CreateDevice` and first draw callbacks — and re-runs the
     "desktop already matches the target, so hands off" decision afterwards.
* Hardened the AVS call: a failed `avs_fs_open` returns an HRESULT style error (`0x80070002` for a
  missing file) rather than 0, and passing that to `avs_fs_read`/`avs_fs_close` corrupted the game's
  filesystem state and crashed the process. Descriptors are now validated and the read length is
  clamped.
* `iidxfps_report.txt` gained a `cabinet` line that shows both rom signals.

Verified on a 240Hz desktop: LDJ `FPS=auto` → `draw=59.6..60.8`, TDJ (`-iidxtdj`) `FPS=auto` →
`draw=119.4..120.9`, all 122 enumerated display modes report the target rate, no tearing of the
mode table, no crash in either mode.

## v1.0.0

First public release.

* QPC based frame pacing: the game loop is held to the cabinet's native rate
  (120 fps TDJ / 60 fps LDJ) instead of following the monitor refresh rate.
* `timeGetTime` / `GetTickCount(64)` replaced with QPC derived milliseconds.
* `EnumDisplaySettings(A/W/ExA/ExW)` spoofed so the game sees the target refresh
  rate (Monitor Check / adapter FPS).
* Runtime detection of the pacing point (spice draw callback / swap chain /
  device present) instead of a hardcoded one.
* `iidxfps_report.txt` self contained status report for bug reports.
* Hands off completely when the desktop refresh rate already equals the target.
* Measure-only diagnostics mode.
* Standalone test harness (`test/`) that reproduces the behaviour without the game.
