# Changelog

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
